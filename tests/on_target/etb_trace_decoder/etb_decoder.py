#!/usr/bin/env python3
#
# Copyright (c) 2023 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause

import subprocess
import re
from elftools.elf.elffile import ELFFile
import tempfile
import argparse
import struct
import os
from colorama import Fore, Back, Style
from collections import namedtuple

ETB_DECODER_PATH = os.path.realpath(os.path.dirname(__file__)) + "/decoder"
OBJDUMP_PATH = "arm-zephyr-eabi-objdump"
OBJCOPY_PATH = "arm-zephyr-eabi-objcopy"

CORTEX_M_EXCEPTIONS = { 1: "Reset", 2: "NMI", 3: "HardFault", 4: "MemManage", 5: "BusFault", 6: "UsageFault", 11: "SVCall", 12: "DebugMonitor", 14: "PendSV", 15: "SysTick" }

FORMAT_EXC_START = Back.RED + Fore.WHITE
FORMAT_EXC_END = Back.GREEN + Fore.BLACK
FORMAT_RECURSION = Back.YELLOW + Fore.BLACK
FORMAT_RESET = Style.RESET_ALL
PRINT_ASSEMBLY = True
PRINT_SYMBOL_INFO_ADDRESS = False
PRINT_SYMBOL_INFO_REASON = False

def disassemble_elf(file_name):
	# Regex to pick out the symbol name and address from the disassembly
	symbol_regex = r"([0-9a-fA-F]+) <(\S+)>:"
	instruction_regex = r"([0-9a-fA-F]+):.*"

	disas = subprocess.run([OBJDUMP_PATH, "-d", file_name], stdout=subprocess.PIPE)
	disas = disas.stdout.decode("utf-8")
	disas = disas.split("\n")

	disas_results = []
	sym_results = []

	current_symbol = ""

	# Find the symbol name each instruction address belongs to, and put them in a dictionary
	for line in disas:
		disas_result = re.search(instruction_regex, line)

		if disas_result != None:
			disas_results.append((int(disas_result.group(1), 16), ("0x" + disas_result.group(0), current_symbol)))
			continue

		sym_result = re.search(symbol_regex, line)

		if sym_result != None:
			current_symbol = sym_result.group(2)
			sym_results.append(sym_result)


	return dict(disas_results)


InstrRangeLine = namedtuple('InstrRangeLine', ['idx', 'trc_id', 'addr_start', 'addr_end', 'num_i', 'last_sz', 'isa', 'last_instr_executed', 'last_instr_type', 'last_instr_subtype'])
ExceptionLine = namedtuple('ExceptionLine', ['idx', 'trc_id', 'ret_addr', 'ex_num'])
ExceptionReturnLine = namedtuple('ExceptionReturnLine', ['idx', 'trc_id'])

ChangeSymbolLine = namedtuple('ChangeSymbol', ['symbol', 'address', 'reason'])
RecursionLine = namedtuple('Recursion', ['symbol', 'address', 'count'])

def parse_decoder_output(decoded_trace):

	# Group 1: Index, Group 2: Trace ID, Group 3: First instruction, Group 4: Last instruction, Group 5: Number of instructions, Group 6: Last instruction size, Group 7: ISA, Group 8: Branch taken or not, Group 9: Branch type, Group 10: Detected branch purpose
	regex_instruction = r"^Idx:(\d+); TrcID:(0x[a-fA-F0-9]+); OCSD_GEN_TRC_ELEM_INSTR_RANGE\(exec range=(0x[a-fA-F0-9]+):\[(0x[a-fA-F0-9]+)\] num_i\((\d+)\) last_sz\((\d)\)\s*\(ISA=(.*?)\)\s*(\w*)\s*(\w*)\s*(.*)\)"
	# Group 1: Index, Group 2: Trace ID, Group 3: Return address, Group 4: Exception number
	regex_exception = r"^Idx:(\d+); TrcID:(0x[a-fA-F0-9]+); OCSD_GEN_TRC_ELEM_EXCEPTION\(pref ret addr:(0x[a-fA-F0-9]+).*; excep num \((0x[a-fA-F0-9]+)\) \)"
	# Regex to detect exception return in the trace
	regex_exception_return = r"^Idx:(\d+); TrcID:(0x[a-fA-F0-9]+); OCSD_GEN_TRC_ELEM_EXCEPTION_RET\(\)"

	linetypes = [
		(regex_instruction, InstrRangeLine),
		(regex_exception, ExceptionLine),
		(regex_exception_return, ExceptionReturnLine),
	]

	parse_result = []

	for line in decoded_trace.split("\n"):
		for pattern, container in linetypes:
			m = re.search(pattern, line)
			if m:
				parse_result.append(container._make(m.groups()))
				break

	return parse_result

def group_by_symbol(parse_result, assembly):
	result = []
	current_symbol = ""
	change_symbol_reason = "branch"
	last_symbol_change_idx = None

	for line in parse_result:
		if type(line) is InstrRangeLine:
			addr_start = int(line.addr_start, base=16)

			_, symbol = assembly.get(addr_start, (None,f"UNKNOWN SYMBOL @ {hex(addr_start)}"))
			if current_symbol != symbol:
				result.append(ChangeSymbolLine(symbol, line.addr_start, change_symbol_reason))
				change_symbol_reason = "branch"
				current_symbol = symbol
				last_symbol_change_idx = len(result) -1
			elif change_symbol_reason == "call":
				if type(result[last_symbol_change_idx]) is RecursionLine:
					result[last_symbol_change_idx] = \
					RecursionLine(
						result[last_symbol_change_idx].symbol,
						result[last_symbol_change_idx].address,
						result[last_symbol_change_idx].count+1
					)
				elif symbol == current_symbol:
					result.append(RecursionLine(symbol, line.addr_start, 2))
					last_symbol_change_idx = len(result) -1

			if line.last_instr_subtype == 'V7:impl ret':
				change_symbol_reason = "return"
			if line.last_instr_subtype == 'b+link ':
				change_symbol_reason = "call"
		elif type(line) is ExceptionLine:
			change_symbol_reason = "exception"
		elif type(line) is ExceptionReturnLine:
			change_symbol_reason = "exception_return"
		result.append(line)

	return result

def get_initial_indentation(parse_result):
	min_indentation = 0
	indentation = 0

	for line in parse_result:
		if type(line) is ChangeSymbolLine:
			if line.reason == "return":
				indentation -= 1
			elif line.reason == "call":
				indentation += 1

			if indentation < min_indentation:
				min_indentation = indentation

	return abs(min_indentation)

def match_trace_to_assembly(parse_result, assembly):
	output = ""

	indentation = 0
	indent_string = "\t" * indentation
	in_exception = False
	exception_indent = 0

	if not PRINT_ASSEMBLY:
		indentation = get_initial_indentation(parse_result)

	for line in parse_result:
		if type(line) is InstrRangeLine and PRINT_ASSEMBLY:
			addr_start = int(line.addr_start, base=16)
			addr_end = int(line.addr_end, base=16)

			# Iterating as if all intructions were 2 bytes (16 Bit), invalid addresses are skipped
			for address in range(addr_start, addr_end, 2):
				if address not in assembly:
					continue

				asm_line, _ = assembly[address]
				output = output + f"{indent_string}\t{asm_line}\n"
		elif type(line) is ChangeSymbolLine or type(line) is RecursionLine:
			symbol_info = ""

			if type(line) is ChangeSymbolLine:
				reason = line.reason
			else:
				reason = "call"

			if PRINT_SYMBOL_INFO_ADDRESS:
				symbol_info = f"({line.address})"

			if PRINT_SYMBOL_INFO_REASON:
				symbol_info = f"{symbol_info} ({reason})"

			if PRINT_ASSEMBLY:
				output = output + f"\n{line.symbol} {symbol_info}\n"
				continue

			if reason == "return":
				if in_exception:
					exception_indent -= 1
					indent_string = "\t" * exception_indent
				else:
					indentation -= 1
					indent_string = "\t" * indentation
			elif reason == "call":
				if in_exception:
					exception_indent += 1
					indent_string = "\t" * exception_indent
				else:
					indentation += 1
					indent_string = "\t" * indentation

			if type(line) is RecursionLine:
				output = output + f"{indent_string}{line.symbol} {symbol_info} {FORMAT_RECURSION}(recursed {line.count} times){FORMAT_RESET}\n"
			else:
				output = output + f"{indent_string}{line.symbol} {symbol_info}\n"

		elif type(line) is ExceptionLine:
			in_exception = True
			indent_string = "\t" * exception_indent
			output = output + f"\n{FORMAT_EXC_START}Exception occurred: {CORTEX_M_EXCEPTIONS.get(int(line.ex_num, 16), int(line.ex_num, 16))} ({line.ex_num}), return address: {line.ret_addr} {FORMAT_RESET}\n"
		elif type(line) is ExceptionReturnLine:
			in_exception = False
			indent_string = "\t" * indentation
			output = output + f"{FORMAT_EXC_END}Exception return{FORMAT_RESET}\n\n"

	return output


def read_data_from_elf(elf: ELFFile, start, size):
	for segment in elf.iter_segments():
		start_addr = segment.header['p_vaddr']
		end_addr = start_addr + segment.header['p_filesz']
		if start in range(start_addr, end_addr):
			start_off = start - start_addr
			result = segment.data()[start_off:start_off + size]
			return result

def extract_etb_buf(symbols: ELFFile, coredump: str, tempdir: str):
	symtab = symbols.get_section_by_name('.symtab')
	if not symtab:
		print('No symbol table available!')
		exit(1)

	etb_buf = symtab.get_symbol_by_name("etb_buf")[0]
	etb_buf_address = etb_buf.entry['st_value']
	etb_buf_size = etb_buf.entry['st_size']
	etb_buf_valid_address = symtab.get_symbol_by_name("etb_buf_valid")[0].entry['st_value']

	with open(coredump, "rb") as f:
		coredump = ELFFile(f)
		etb_buf_valid = read_data_from_elf(coredump, etb_buf_valid_address, 4)
		if etb_buf_valid != bytes([0xef, 0xbe, 0xad, 0xde]):
			print(f"etb_buf_valid has unexpected value: {etb_buf_valid}")

		etb_buf_filename = os.path.join(tempdir, "etb_buf")

		with open(etb_buf_filename, "wb") as f:
			etb_buf = read_data_from_elf(coredump, etb_buf_address, etb_buf_size)
			f.write(etb_buf)
		return etb_buf_filename


def decode_trace(trace_file, elf_file, extract_etb):
	with open(elf_file, "rb") as f:
		elf = ELFFile(f)
		start_addr = elf.get_section_by_name('rom_start').header.sh_addr
		etm_trctraceidr = 0x10
		try:
			etm_trctraceidr = find_symbol_init_value(elf_file, "etm_trctraceidr")
		except TypeError:
			pass


		# Workaround to avoid stopping trace decoding when return stack overflow is detected.
		# This ignores if the ETM is configured to trace return stack.
		# TOOD: Figure out why this happens only when reading out the memory from .elf and not when using objdump
		etm_trcconfigr = 8
		try:
			etm_trcconfigr = find_symbol_init_value(elf_file, "etm_trcconfigr") & 0xFFF
		except TypeError:
			pass

		with tempfile.TemporaryDirectory() as tmp:
			if extract_etb:
				# treat trace_file as coredump ELF
				trace_file = extract_etb_buf(elf, extract_etb, tmp)

			tmp_file = os.path.join(tmp, os.path.basename(elf_file))
			subprocess.run([OBJCOPY_PATH, "-O", "binary", elf_file, tmp_file])
			decoded_trace = subprocess.run([ETB_DECODER_PATH, "-i", trace_file, "-m", tmp_file, "-decode", "-a", str(start_addr), "-id", str(etm_trctraceidr), "-config", str(etm_trcconfigr)], stdout=subprocess.PIPE)

	return decoded_trace.stdout.decode("utf-8")

def find_symbol_init_value(elf_file, symbol_name):
	with open(elf_file, "rb") as f:
		elf = ELFFile(f)
		# Get the symbol table entry for the respective symbol
		symtab = elf.get_section_by_name('.symtab')
		if not symtab:
			print('No symbol table available!')
			exit(1)

		sym = symtab.get_symbol_by_name(symbol_name)[0]
		if not sym:
			print('Symbol {} not found')
			exit(1)

		# Find the segment where the symbol is loaded to, as the symbol table points to
		# the loaded address, not the offset in the file
		file_offset = None
		for seg in elf.iter_segments():
			if seg.header['p_type'] != 'PT_LOAD':
				continue
			# If the symbol is inside the range of a LOADed segment, calculate the file
			# offset by subtracting the virtual start address and adding the file offset
			# of the loaded section(s)
			if sym['st_value'] >= seg['p_vaddr'] and sym['st_value'] < seg['p_vaddr'] + seg['p_filesz']:
				file_offset = sym['st_value'] - seg['p_vaddr'] + seg['p_offset']
				break

		if not file_offset:
			print('Error getting file offset from ELF data')
			exit(1)

		# Forward the file stream to the identified offset
		elf.stream.seek(file_offset)
		# Read the value as a 4 byte integer and print it
		value = struct.unpack('i', elf.stream.read(4))[0]
		print('Variable {} at address {} (file offset {}) has value {}'.format(symbol_name, hex(sym['st_value']), hex(file_offset), hex(value)))

		return value

def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("-d", "--coredump", help= "Extract trace from coredump ELF")
	parser.add_argument("-s", "--symbols", help = "Symbols ELF File (full path)", required=True)
	parser.add_argument("-t", "--trace", help = "Trace File (full path), not needed if coredump ELF is given")
	parser.add_argument("-o", "--output", help = "Output file")
	parser.add_argument("-n", "--no-assembly", help = "Do not print assembly, only symbol names", action="store_true")
	parser.add_argument("-c", "--color", help = "Enable colors in the output", action="store_true")
	parser.add_argument("-objdump", "--objdump", help = "Path to objdump (default: arm-zephyr-eabi-objdump)")
	parser.add_argument("-objcopy", "--objcopy", help = "Path to objcopy (default: arm-zephyr-eabi-objcopy)")
	parser.add_argument("-r", "--reason", help = "Print the reson for a symbol change", action="store_true")
	parser.add_argument("-a", "--address", help = "Print the address of a symbol", action="store_true")

	args = parser.parse_args()

	if not args.coredump and not args.trace:
		print("Please specify either trace BIN or coredump ELF.")
		parser.print_help()
		exit()

	if args.objdump:
		global OBJDUMP_PATH
		OBJDUMP_PATH = args.objdump
	else:
		print("No objdump path given, using default: " + OBJDUMP_PATH + "")

	if args.objcopy:
		global OBJCOPY_PATH
		OBJCOPY_PATH = args.objcopy
	else:
		print("No objcopy path given, using default: " + OBJCOPY_PATH + "")

	if not args.color:
		global FORMAT_EXC_START, FORMAT_EXC_END, FORMAT_RESET, FORMAT_RECURSION
		FORMAT_EXC_START = ""
		FORMAT_EXC_END = ""
		FORMAT_RECURSION = ""
		FORMAT_RESET = ""

	if args.no_assembly:
		global PRINT_ASSEMBLY
		PRINT_ASSEMBLY = False

	if args.reason:
		global PRINT_SYMBOL_INFO_REASON
		PRINT_SYMBOL_INFO_REASON = True

	if args.address:
		global PRINT_SYMBOL_INFO_ADDRESS
		PRINT_SYMBOL_INFO_ADDRESS = True

	decoded_trace = decode_trace(args.trace, args.symbols, args.coredump)
	assembly_dict = disassemble_elf(args.symbols)
	parse_result = parse_decoder_output(decoded_trace)
	parse_result = group_by_symbol(parse_result, assembly_dict)
	output = match_trace_to_assembly(parse_result, assembly_dict)

	if args.output:
		with open(args.output, "w") as f:
			f.write(output)
			print("Output written to " + args.output + "")
	else:
		print(output)

if __name__ == '__main__':
	main()
