# Notes for coding agents

Read README.txt and PROTOCOL.md first. Work autonomously; ask only for physical
actions (replugging the card, changing cables or sources). Keep docs short.

## Hardware rules (each was learned the hard way)

- Never write firmware, flash, EEPROM or SPI. All setup is volatile.
- I2C commands carry at most 32 data bytes.
- Access the PCA9539 (0x74) one register at a time; block access wraps.
- Never touch receiver maps while PCA bit 6 (receiver reset) is low.
- Keep the receiver's internal EDID off; the board EEPROM is the EDID.
- A wedged card (EP1 reads time out) needs a physical USB replug.
- Only one process can own the card; close OBS before `build/smoke`.

## Working

- Keep register order and access patterns in `src/cv710.c` unchanged unless an
  experiment proves otherwise.
- Test with `make smoke && build/smoke 10`, then in OBS.
- Log experiments briefly in `research/LOG.md`. Never commit captures or vendor
  material.
- If linking fails with "unknown architecture" in `.tbd` files, the default SDK
  is newer than the compiler: set `SDKROOT` to an older SDK.
