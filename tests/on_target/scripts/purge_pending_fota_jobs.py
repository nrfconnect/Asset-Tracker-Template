#!/usr/bin/env python3
##########################################################################################
# Copyright (c) 2026 Nordic Semiconductor
# SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
##########################################################################################

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from utils.logger import get_logger
from utils.nrfcloud import NRFCloudFOTA

logger = get_logger()


def main():
    api_key = os.getenv("NRFCLOUD_API_KEY")
    device_id = os.getenv("UUID")

    if not api_key:
        logger.info("NRFCLOUD_API_KEY not set, skipping pending FOTA job purge")
        return 0

    if not device_id:
        logger.info("UUID not set, skipping pending FOTA job purge")
        return 0

    try:
        NRFCloudFOTA(api_key=api_key).ensure_no_pending_fota_jobs(device_id)
    except Exception as e:
        logger.error(f"Failed to purge pending FOTA jobs for {device_id}: {e}")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
