from pyocd.core.helpers import ConnectHelper
from intelhex import IntelHex

def read_flash_memory(session, core_number, start, end, output_file):
    # Select the specified core
    target = session.target
    target.selected_core = core_number

    # Calculate the length of memory to read
    length = end - start

    # Read the flash memory range and store the contents
    flash_data = target.read_memory_block8(start, length)

    # Create an IntelHex object and populate it with the flash data
    ih = IntelHex()
    ih[start:end] = list(flash_data)

    # Write the data to an Intel HEX file
    ih.write_hex_file(output_file)
    print(f"Flash memory data saved to {output_file}")

def main():
    # Define your parameters
    core_number = 0                # Core number to read from
    start_address = 0x00000000      # Starting address of flash memory
    end_address = 0x00100000        # Ending address of flash memory
    output_file = "flash_memory.hex"  # Output HEX file name

    # Open a session with the default target
    with ConnectHelper.session_with_chosen_probe() as session:
        if session is None:
            print("Failed to connect to the target.")
            return

        # Read and save flash memory for the specified core and address range
        read_flash_memory(session, core_number, start_address, end_address, output_file)

if __name__ == "__main__":
    main()
