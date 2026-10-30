# nRF9251: SPI chip select released ~2 ms late (GPIO pin retention race)

Related bug: NRFX-10395, "[Saana] HSFLL120 has wrong frequency from SystemOnAllIdle"

## Summary

On nRF9251 DKs v0.2.1 with ironside 23.8.1-live+37 , a GPIO driven chip select (CS) on SPI120 is sometimes not released at the
end of a transfer. The pad stays low until the next GPIO write to the port. The
external flash (`gd25le255e`) then sees the end of a command late, and flash operations fail
intermittently.

The cause is a race in the GPIO driver's pin retention handling. It only shows up on DKs where
HSFLL120 runs about 14% slow, which points to NRFX-10395. Disabling pin retention in
`gpio_nrfx.c` removes the failures.

## Affected hardware

| DK | SiP | SoC | IronSide SE | SPIM clock at 4 MHz | Late CS |
|---|---|---|---|---|---|
| v0.2.0 | E1.1 | 1.0 (ES3.0) | 23.8.1-debug+37 | 3.986 MHz | none |
| v0.2.1 | E1.2 | 1.2 (ES4.0) | 23.8.1+37 (release) | 3.44 MHz | yes |

Both DKs have the same flash and run the same firmware.

## Mechanism

Every GPIO write in `zephyr/drivers/gpio/gpio_nrfx.c` does:

1. RETAINCLR on the pin
2. OUTSET or OUTCLR
3. RETAINSET on the pin

On the bad DKs, step 3 sometimes latches the pad before the new output value from step 2 has
reached it. The pad then keeps the old value (CS low), even though the OUT register already
reads "released". It stays like that until the next write to the port runs step 1. The result
on the logic analyser is CS low for ~2 ms, then a ~0.5 µs high pulse just before the next
transfer asserts CS again.

How HSFLL120 relates to the race is inferred, not proven. The race only shows up on the DKs with
the slow HSFLL120..

## Workaround in this tree

[0001-zephyr-gpio-nrfx-disable-pin-retention-nrf9251-workaround.patch](0001-zephyr-gpio-nrfx-disable-pin-retention-nrf9251-workaround.patch)
applies to the `zephyr` repository (sdk-zephyr, based on `32649ffd62d`):

- It adds `CONFIG_SPI_TEST_PATCH_NO_GPIO_RETENTION` in `drivers/gpio/Kconfig.nrfx`, with
  default `y` on `BOARD_NRF9251DK_NRF9251_CPUAPP`.
- When this option is set, `port_retain_set()` returns without enabling retention. Retention is
  still cleared before each write.

Apply from the workspace root:

```
git -C zephyr apply ../project/patches/0001-zephyr-gpio-nrfx-disable-pin-retention-nrf9251-workaround.patch
```

To turn it off without removing the patch, set `CONFIG_SPI_TEST_PATCH_NO_GPIO_RETENTION=n`.

## Caveats

- This is temporary and not for upstream.
- It does not fix the clock. The SPIM120 clock is still ~14% slow, and so is anything else in
  the HSFLL120 domain, for example UARTE120 baud rates.
- With retention never enabled, pins are not held while their power domain is off. Check this
  against the low-power requirements before relying on the workaround.
- It applies to every GPIO port in the build, not only the CS pins.


## Reproduction

- `apps/spi_test_spim_nrf9251`: SPIM120 controller with CS readback and an event dump.
  `variants/no_retention.conf` enables the workaround. `tools/analyze_capture.py` decodes a
  Saleae CSV export.
- `apps/spi_test_spis_nrf54lm20a`: SPI peripheral on an nRF54LM20 DK, used as the other end.
- `apps/ext_flash_test`: external flash test. `variants/no_retention.conf` enables the
  workaround.
