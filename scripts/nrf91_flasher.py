#!/usr/bin/env python3
#
# Copyright (c) 2024 Nordic Semiconductor ASA
#
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause

from pyocd.core.helpers import ConnectHelper
from pyocd.flash.file_programmer import FileProgrammer
from pyocd.core.target import Target
from pyocd.target.family.target_nRF91 import ModemUpdater
from pyocd.core.exceptions import TargetError
from intelhex import IntelHex
from tempfile import TemporaryDirectory
import os
from timeit import default_timer as timer
import argparse

import logging
logging.basicConfig(level=logging.INFO)

parser = argparse.ArgumentParser(description="Wrapper around pyOCD to flash nRF91")
parser.add_argument("-e", "--erase", help = "perform mass_erase", action='store_true')
parser.add_argument("-p", "--program", help = "program file (hex, elf, bin)")
parser.add_argument("-m", "--modem", help = "modem update zip file")
parser.add_argument("-u", "--uid", help = "probe uid")

args = parser.parse_args()

options = {
    "target_override" : "nrf91",
    "cmsis_dap.limit_packets" : False,
    "frequency" : 8000000,
    "logging" : {
	"loggers": {
		"pyocd.coresight" : {"level": logging.INFO}
	}
    }
}

with ConnectHelper.session_with_chosen_probe(unique_id=args.uid, options=options) as session:

    board = session.board
    target = board.target
    flash = target.memory_map.get_boot_memory()

    if args.erase:
        logging.info("perform mass erase")
        target.mass_erase()

    if args.program:
        if args.program.endswith("hex"):
            # Load firmware into device.
            logging.info("flashing program")
            input = IntelHex(args.program)
            flash = input[0x00000000:0x00100000]
            uicr  = input[0x00FF8000:0x00FF9000]
            with TemporaryDirectory() as tempdir:
                if len(uicr.segments()) > 0:
                    logging.info("writing UICR")
                    for start, end in uicr.segments():
                        target.write_flash(start, uicr[start:end].tobinarray())

                logging.info("writing flash")
                flash_file = os.path.join(tempdir, "flash.hex")
                flash.tofile(flash_file, format="hex")
                FileProgrammer(session).program(flash_file)
        else:
            logging.info("not a HEX file, flashing without range checks")
            FileProgrammer(session).program(args.program)

    if args.modem:
        modem_needs_update = False

        # Check modem firmware version
        try:
            ModemUpdater(session).verify(args.modem)
        except TargetError as e:
            modem_needs_update = True

        # Update modem firmware.
        if modem_needs_update:
            logging.warning("modem verify failed, updating modem firmware")
            start = timer()
            ModemUpdater(session).program_and_verify(args.modem)
            end = timer()
            logging.info(f"modem update took {end-start} seconds")
    # Reset, run.
    logging.info("resetting device")
    target.reset()
