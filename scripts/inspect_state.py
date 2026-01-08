#!/usr/bin/env python3
"""
Asset Tracker Template State Inspector

This script connects to an nRF91 device via J-Link, inspects the ELF file for
symbol locations, and reads the current state of various state machines in RAM.
It also allows interactive inspection of the full state structure of each module.

Prerequisites:
    pip install "pyelftools>=0.30" pylink-square
"""

import sys
import argparse
import logging
import traceback
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Any, Union

from elftools.elf.elffile import ELFFile
from elftools.dwarf.die import DIE
import pylink

# Configure logging
logging.basicConfig(level=logging.INFO, format='%(message)s')
logger = logging.getLogger(__name__)


# --- Type Definitions ---

@dataclass
class TypeInfo:
    """Base class for type information."""
    name: str
    size: int

@dataclass
class PrimitiveType(TypeInfo):
    """Represents basic types like int, bool, char."""
    pass

@dataclass
class StructMember:
    """Represents a member of a structure."""
    name: str
    offset: int
    type_info: 'TypeInfo'

@dataclass
class StructType(TypeInfo):
    """Represents a structure type."""
    members: List[StructMember] = field(default_factory=list)

@dataclass
class EnumType(TypeInfo):
    """Represents an enumeration type."""
    mapping: Dict[int, str] = field(default_factory=dict)

@dataclass
class PointerType(TypeInfo):
    """Represents a pointer type."""
    target_type: Optional['TypeInfo'] = None

@dataclass
class ArrayType(TypeInfo):
    """Represents an array type."""
    element_type: Optional['TypeInfo'] = None
    count: int = 0


# --- Configuration and Result Classes ---

@dataclass
class ModuleConfig:
    """Configuration for a module to inspect."""
    name: str
    file_name: str
    function_name: str
    variable_name: str
    states_array_name: str
    enum_type_name: Optional[str] = None


@dataclass
class SymbolInfo:
    """Resolved symbol information from the ELF file."""
    state_var_addr: Optional[int] = None
    states_array_addr: Optional[int] = None
    state_var_type: Optional[TypeInfo] = None
    # We still keep the top-level enum map for the main state machine state
    # usually found via the 'state' enum type.
    enum_map: Dict[int, str] = field(default_factory=dict)


# --- Module Definitions ---

MODULES = [
    ModuleConfig(
        name="Main",
        file_name="main.c",
        function_name="main",
        variable_name="main_state",
        states_array_name="states",
        enum_type_name="state"
    ),
    ModuleConfig(
        name="Cloud",
        file_name="cloud.c",
        function_name="cloud_module_thread",
        variable_name="cloud_state",
        states_array_name="states",
        enum_type_name="cloud_module_state"
    ),
    ModuleConfig(
        name="Location",
        file_name="location.c",
        function_name="location_module_thread",
        variable_name="location_state",
        states_array_name="states",
        enum_type_name="location_module_state"
    ),
    ModuleConfig(
        name="Network",
        file_name="network.c",
        function_name="network_module_thread",
        variable_name="network_state",
        states_array_name="states",
        enum_type_name="network_module_state"
    ),
    ModuleConfig(
        name="FOTA",
        file_name="fota.c",
        function_name="fota_module_thread",
        variable_name="fota_state",
        states_array_name="states",
        enum_type_name="fota_module_state"
    ),
    ModuleConfig(
        name="Env",
        file_name="environmental.c",
        function_name="env_module_thread",
        variable_name="environmental_state",
        states_array_name="states",
        enum_type_name="environmental_module_state"
    ),
    ModuleConfig(
        name="Power",
        file_name="power.c",
        function_name="power_module_thread",
        variable_name="power_state",
        states_array_name="states",
        enum_type_name="power_module_state"
    ),
    ModuleConfig(
        name="Storage",
        file_name="storage.c",
        function_name="storage_thread",
        variable_name="storage_state",
        states_array_name="states",
        enum_type_name="storage_module_state"
    ),
]

# Size of 'struct smf_state' in bytes.
SMF_STATE_SIZE_DEFAULT = 20
DW_OP_ADDR = 0x03


# --- DWARF Parsing Helpers ---

def _decode_name(attribute: Any) -> Optional[str]:
    """Helper to safely decode a DWARF attribute string."""
    if not attribute:
        return None
    try:
        return attribute.value.decode('utf-8', errors='ignore')
    except (AttributeError, UnicodeDecodeError):
        return None


def _extract_address_from_location(die: DIE) -> Optional[int]:
    """Extracts the address from a DW_AT_location attribute."""
    loc = die.attributes.get('DW_AT_location')

    if loc and len(loc.value) > 0 and loc.value[0] == DW_OP_ADDR:
        return int.from_bytes(loc.value[1:], byteorder='little')
    return None


class TypeParser:
    """Parses DWARF DIEs into TypeInfo objects."""

    def __init__(self, dwarfinfo):
        self.dwarfinfo = dwarfinfo
        self.cache: Dict[int, TypeInfo] = {}

    def _get_die_from_attribute(self, attr: Any, cu: Any) -> DIE:
        """Resolves a DIE from a DW_AT_type (or similar) attribute."""
        if attr.form in ('DW_FORM_ref_addr', 'DW_FORM_ref_sig8'):
            # Absolute global offset
            offset = attr.value
        elif attr.form in ('DW_FORM_ref1', 'DW_FORM_ref2', 'DW_FORM_ref4', 'DW_FORM_ref8', 'DW_FORM_ref_udata'):
            # Relative to CU start
            offset = attr.value + cu.cu_offset
        else:
            # Fallback (some versions of pyelftools might normalize differently?)
            # But usually it's one of above.
            offset = attr.value

        return self._get_die_at_offset(offset)

    def parse_type(self, die: DIE) -> TypeInfo:
        """Parses a type DIE and returns a TypeInfo object."""
        # Check cache to avoid infinite recursion on self-referential types
        if die.offset in self.cache:
            return self.cache[die.offset]

        tag = die.tag
        name = _decode_name(die.attributes.get('DW_AT_name')) or "<anon>"
        size = die.attributes.get('DW_AT_byte_size')
        size = size.value if size else 0

        # Handle type modifiers (const, volatile, typedef) by stripping them
        if tag in ('DW_TAG_const_type', 'DW_TAG_volatile_type', 'DW_TAG_typedef'):
            type_attr = die.attributes.get('DW_AT_type')

            if type_attr:
                target_die = self._get_die_from_attribute(type_attr, die.cu)
                parsed = self.parse_type(target_die)
                return parsed
            else:
                 # void or unknown
                return PrimitiveType(name="void", size=0)

    # Create placeholder in cache for recursive structures
        if tag == 'DW_TAG_structure_type':
            struct_type = StructType(name=name, size=size)
            self.cache[die.offset] = struct_type

            for child in die.iter_children():
                if child.tag == 'DW_TAG_member':
                    member_name = _decode_name(child.attributes.get('DW_AT_name')) or "<anon>"

                    # Get member offset
                    mem_loc = child.attributes.get('DW_AT_data_member_location')
                    offset = mem_loc.value if mem_loc else 0

                    # Parse member type
                    type_attr = child.attributes.get('DW_AT_type')
                    if type_attr:
                        target_die = self._get_die_from_attribute(type_attr, die.cu)
                        member_type = self.parse_type(target_die)
                        struct_type.members.append(StructMember(member_name, offset, member_type))

            return struct_type

        if tag == 'DW_TAG_pointer_type':
            ptr_type = PointerType(name=name, size=size or 4) # Assume 32-bit if size missing
            self.cache[die.offset] = ptr_type

            type_attr = die.attributes.get('DW_AT_type')
            if type_attr:
                target_die = self._get_die_from_attribute(type_attr, die.cu)
                ptr_type.target_type = self.parse_type(target_die)
            return ptr_type

        if tag == 'DW_TAG_array_type':
            array_type = ArrayType(name=name, size=size)
            self.cache[die.offset] = array_type

            type_attr = die.attributes.get('DW_AT_type')
            if type_attr:
                target_die = self._get_die_from_attribute(type_attr, die.cu)
                array_type.element_type = self.parse_type(target_die)

            # Find array range to determine count
            for child in die.iter_children():
                if child.tag == 'DW_TAG_subrange_type':
                    upper = child.attributes.get('DW_AT_upper_bound')
                    if upper:
                        array_type.count = upper.value + 1

            # Recalculate size if missing
            if array_type.size == 0 and array_type.element_type and array_type.count > 0:
                array_type.size = array_type.element_type.size * array_type.count

            return array_type

        if tag == 'DW_TAG_enumeration_type':
            enum_type = EnumType(name=name, size=size)
            self.cache[die.offset] = enum_type

            for child in die.iter_children():
                if child.tag == 'DW_TAG_enumerator':
                    const_name = _decode_name(child.attributes.get('DW_AT_name'))
                    const_val = child.attributes.get('DW_AT_const_value').value
                    if const_name:
                        enum_type.mapping[const_val] = const_name
            return enum_type

        # Fallback for base types and others
        return PrimitiveType(name=name, size=size)

    def _get_die_at_offset(self, offset: int) -> DIE:
        try:
            # This is available in pyelftools >= 0.27
            return self.dwarfinfo.get_DIE_from_refaddr(offset)
        except AttributeError:
            # Fallback: scan all CUs (VERY SLOW)
            for cu in self.dwarfinfo.iter_CUs():
                for die in cu.iter_DIEs():
                    if die.offset == offset:
                        return die
        raise ValueError(f"DIE at offset {offset} not found")


def get_symbol_info(dwarfinfo: Any, config: ModuleConfig) -> SymbolInfo:
    """Scans DWARF info to find symbol addresses and type info."""
    result = SymbolInfo()
    type_parser = TypeParser(dwarfinfo)

    for compilation_unit in dwarfinfo.iter_CUs():
        top_die = compilation_unit.get_top_DIE()
        die_path = top_die.get_full_path()

        if not die_path.endswith(config.file_name):
            continue

        for die in compilation_unit.iter_DIEs():

            # 1. Look for the function and its static variable
            if die.tag == 'DW_TAG_subprogram':
                func_name = _decode_name(die.attributes.get('DW_AT_name'))
                if func_name == config.function_name:
                    for child in die.iter_children():
                        if child.tag == 'DW_TAG_variable':
                            var_name = _decode_name(child.attributes.get('DW_AT_name'))
                            if var_name == config.variable_name:
                                addr = _extract_address_from_location(child)
                                if addr is not None:
                                    result.state_var_addr = addr
                                    # Parse the type of the variable
                                    type_attr = child.attributes.get('DW_AT_type')
                                    if type_attr:
                                        type_die = type_parser._get_die_from_attribute(type_attr, die.cu)
                                        result.state_var_type = type_parser.parse_type(type_die)

            # 2. Look for the 'states' array
            if die.tag == 'DW_TAG_variable':
                var_name = _decode_name(die.attributes.get('DW_AT_name'))
                if var_name == config.states_array_name:
                    addr = _extract_address_from_location(die)
                    if addr is not None:
                        result.states_array_addr = addr

            # 3. Look for the main state enum type (for the simple summary)
            if config.enum_type_name and die.tag == 'DW_TAG_enumeration_type':
                name = _decode_name(die.attributes.get('DW_AT_name'))
                if name == config.enum_type_name:
                    result.enum_map = type_parser.parse_type(die).mapping

        if result.state_var_addr is not None:
            break

    return result


def analyze_elf(elf_path: Path) -> Dict[str, SymbolInfo]:
    """Parses the ELF file to extract symbol information."""
    logger.info(f"Parsing ELF: {elf_path}")
    lookup: Dict[str, SymbolInfo] = {}

    try:
        with open(elf_path, 'rb') as f:
            elffile = ELFFile(f)
            if not elffile.has_dwarf_info():
                logger.error("ELF file has no DWARF debug info.")
                return {}

            dwarfinfo = elffile.get_dwarf_info()

            for module in MODULES:
                # logger.debug(f"Looking up symbols for {module.name}...")
                info = get_symbol_info(dwarfinfo, module)

                if info.state_var_addr is None:
                    # Only warn if it looks like we should have found it
                    pass
                else:
                    lookup[module.name] = info

    except Exception as e:
        logger.error("Error parsing ELF: %s", e)
        traceback.print_exc()
        return {}

    return lookup


# --- Memory Reading and Formatting ---

def read_smf_state_name(jlink, info: SymbolInfo, current_state_ptr: int) -> str:
    """Resolves the SMF state name from the current state pointer."""
    if not info.states_array_addr or current_state_ptr == 0:
        return "NULL"

    offset = current_state_ptr - info.states_array_addr

    if offset < 0:
        return f"Unknown (0x{current_state_ptr:X})"

    # Heuristic for index
    index = -1
    if offset % SMF_STATE_SIZE_DEFAULT == 0:
        index = offset // SMF_STATE_SIZE_DEFAULT
    elif offset % 16 == 0:
        index = offset // 16

    if index >= 0:
        return info.enum_map.get(index, f"State {index}")

    return f"0x{current_state_ptr:X}"


def read_value(jlink, address: int, type_info: TypeInfo, indent: int = 0) -> str:
    """Reads and formats a value from memory based on its type."""
    # prefix = " " * indent
    # Unused for now, but kept for future recursive printing if needed
    _ = indent

    try:
        if isinstance(type_info, PrimitiveType):
            # Read primitive based on size
            if type_info.size == 0:
                return "void"

            data = jlink.memory_read(address, type_info.size)
            val = int.from_bytes(data, byteorder='little')

            # Basic formatting
            if type_info.name == "bool":
                return "true" if val else "false"
            if "char" in type_info.name:
                return f"'{chr(val)}'" if 32 <= val <= 126 else f"{val}"
            return f"{val} (0x{val:X})"

        if isinstance(type_info, EnumType):
            data = jlink.memory_read(address, type_info.size or 4)
            val = int.from_bytes(data, byteorder='little')

            return f"{type_info.mapping.get(val, str(val))} ({val})"

        if isinstance(type_info, PointerType):
            data = jlink.memory_read(address, 4)
            val = int.from_bytes(data, byteorder='little')

            if val == 0:
                return "NULL"

            return f"0x{val:08X}"

        if isinstance(type_info, ArrayType):
            # Limit array display
            count = type_info.count

            if count > 16:
                return f"Array[{count}] (too large to display)"

            if isinstance(type_info.element_type, PrimitiveType) and type_info.element_type.size == 1:
                # Byte array / string
                data = jlink.memory_read(address, count)

                try:
                    # Try to decode as string if it looks like one
                    if 0 in data:
                        str_len = data.index(0)
                        s = bytes(data[:str_len]).decode('utf-8')

                        return f'"{s}"'
                except Exception: # pylint: disable=broad-except
                    pass

                return f"{list(data)}"
            return f"Array[{count}]"

        if isinstance(type_info, StructType):
            # We don't inline structs usually, unless asked.
            return f"{type_info.name} {{ ... }}"

    except Exception as e: # pylint: disable=broad-except
        return f"<Error: {e}>"

    return "?"


def print_struct(jlink, address: int, struct_type: StructType, info: SymbolInfo, indent: int = 0):
    """Recursively prints structure members."""
    prefix = " " * indent

    # Calculate max name length for alignment at this level
    if not struct_type.members:
        return
    max_name_len = max(len(m.name) for m in struct_type.members)

    for member in struct_type.members:
        member_addr = address + member.offset

        # Special handling for 'struct smf_ctx' to show the state name
        if member.type_info.name == "smf_ctx":
            # smf_ctx first member is 'current' pointer.
            # We can read it manually or just use our helper if we know it matches.
            try:
                data = jlink.memory_read(member_addr, 4)
                current_ptr = int.from_bytes(data, byteorder='little')
                state_name = read_smf_state_name(jlink, info, current_ptr)
                print(f"{prefix}{member.name:<{max_name_len}} : {state_name}")
                continue
            except Exception: # pylint: disable=broad-except
                pass

        val_str = read_value(jlink, member_addr, member.type_info)

        # Optionally expand nested structs if they are small or interesting?
        # For now, let's keep it flat unless the user drills down, but since this IS the drill down...
        # Let's expand nested structs if they are not pointers.
        if isinstance(member.type_info, StructType) and member.type_info.name != "smf_ctx":
            print(f"{prefix}{member.name:<{max_name_len}} : {val_str}")
            print(f"{prefix}  {{")
            print_struct(jlink, member_addr, member.type_info, info, indent + 4)
            print(f"{prefix}  }}")
        else:
            print(f"{prefix}{member.name:<{max_name_len}} : {val_str}")


def inspect_module_detail(jlink, info: SymbolInfo, module_name: str):
    """Reads and displays the full structure of the module state."""
    if not info.state_var_addr or not isinstance(info.state_var_type, StructType):
        print(f"No structure information available for {module_name}.")
        return

    print(f"\n--- {module_name} State Details ---")
    print(f"Address: 0x{info.state_var_addr:08X}")
    print(f"Type: {info.state_var_type.name}")
    print("-" * 40)

    print_struct(jlink, info.state_var_addr, info.state_var_type, info)
    print("-" * 40)


def print_summary(jlink, lookup):
    """Prints the summary table of all modules."""
    print(f"\n{'Module':<15} | {'Current State':<60} | {'Details'}")
    print("-" * 100)

    for name, info in lookup.items():
        if info.state_var_addr is None or info.states_array_addr is None:
            continue

        try:
            # Read first word (smf_ctx.current)
            data = jlink.memory_read32(info.state_var_addr, 1)
            current_state_ptr = data[0]

            state_name = read_smf_state_name(jlink, info, current_state_ptr)

            print(f"{name:<15} | {state_name:<60} | Ptr: 0x{current_state_ptr:08X}")

        except Exception: # pylint: disable=broad-except
            print(f"{name:<15} | {'???':<60} | Error reading")


def interactive_loop(lookup, device_name):
    """Main interactive loop."""

    print(f"Connecting to J-Link ({device_name})...")

    jlink = pylink.JLink()

    try:
        jlink.open()
        jlink.connect(device_name)

        while True:
            print_summary(jlink, lookup)

            print("\nOptions:")
            print("  q: Quit")
            print("  r: Refresh summary")

            # Create numbered list of modules
            modules = list(lookup.keys())

            for i, name in enumerate(modules):
                print(f"  {i+1}: Inspect {name}")

            choice = input("\nSelect option: ").strip().lower()

            if choice == 'q':
                break
            elif choice == 'r':
                continue

            try:
                idx = int(choice) - 1

                if 0 <= idx < len(modules):
                    inspect_module_detail(jlink, lookup[modules[idx]], modules[idx])
                    input("\nPress Enter to continue...")
                else:
                    print("Invalid selection.")
            except ValueError:
                pass

    except pylink.errors.JLinkException as e:
        logger.error(f"J-Link Error: {e}")
    finally:
        if jlink.opened():
            jlink.close()


def main():
    try:
        # pylint: disable=import-outside-toplevel
        import elftools
        from packaging import version

        if version.parse(elftools.__version__) < version.parse("0.30"):
            logger.error("pyelftools version %s is too old. Please upgrade to >= 0.30",
                         elftools.__version__)
            sys.exit(1)
    except ImportError:
        pass

    parser = argparse.ArgumentParser(description='Asset Tracker State Inspector',
                                     allow_abbrev=False)
    parser.add_argument('--elf', required=True, help='Path to zephyr.elf file')
    parser.add_argument(
        '--device',
        default='Cortex-M33',
        help='J-Link Device Name (default: Cortex-M33).'
    )
    args = parser.parse_args()

    elf_path = Path(args.elf)

    if not elf_path.exists():
        logger.error(f"ELF file not found: {elf_path}")
        sys.exit(1)

    lookup = analyze_elf(elf_path)

    if not lookup:
        logger.error(
            "No symbols found. Ensure the application was built with the provided "
            "static variable changes."
        )
        sys.exit(1)

    interactive_loop(lookup, args.device)


if __name__ == "__main__":
    main()
