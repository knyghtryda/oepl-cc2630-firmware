# Reference binaries

The analysis in `docs/` refers to firmware images that are **not distributed
here**, because they aren't ours to distribute:

| file | what it is |
|---|---|
| `stock.bin`, `stock_device2.bin` | 128 KB dumps of the Solum TG-GR6000N factory firmware, from two devices |
| `stock.elf` | the same, wrapped for Ghidra |
| `CC2630_5.8_OEPL_alpha.bin` | a third party's earlier CC2630 OEPL build |
| `CC2630_5.8_OEPL_alpha_combined.bin` | that image combined with a stock CCFG |
| `oepl_alpha.elf` | the same, wrapped for Ghidra |

Dumping the firmware off a tag you own in order to understand the hardware is
one thing; republishing the whole copyrighted image is another, and this repo
used to do the second by accident. Removed 2026-09-21.

`stock_ccfg.bin` is kept: 88 bytes of CC2630 customer configuration — clock
trims, the bootloader backdoor pin, debug gating. Register values the part
requires, with one correct answer, not expression.

## Getting your own

Read it off a tag you own, over the UART ROM bootloader:

```bash
cc2538-bsl.py -p /dev/ttyUSB0 --bootloader-invert-lines -r reference/stock.bin
```

or over J-Link (`tools/jflash.sh` has the wiring). **Take this dump before you
flash anything**, and keep it — it is the only way back to factory firmware, and
nobody else can give you one.

## What was learned from them, and is in this repo

Everything actually needed is written down as facts rather than kept as a blob:

- the board pin map — `DEVELOPMENT.md`, "Board pin map"
- the UC8159 600x448 initialisation sequence — `docs/COMPLETE_INIT_SEQUENCE.md`
- the CCFG values, DC/DC settings and standby sequence, which turned out to be
  identical to ours — `DEVELOPMENT.md`, "Power"

So the documentation stands on its own; the binaries were only ever the working
material.
