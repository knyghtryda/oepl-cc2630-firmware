# Flashing via TI XDS110 (LaunchPad) with UniFlash

An alternative to the J-Link / UART bootloader routes in the README, using the
XDS110 debug probe built into any TI LaunchPad (verified with a
**CC3220SF-LAUNCHXL**) and TI UniFlash's command-line tool `dslite`.

Verified 2026-09-21 on a stock TG-GR6000N, UniFlash 9.5.0.5651, XDS110
firmware 3.0.0.41, Windows 11. Flashed the June ELF and the v0.32 `.bin`;
both booted.

## TL;DR

```bash
tools/xflash.sh  binaries/Tag_FW_CC2630_TG-GR6000N.bin     # Linux / macOS / Git Bash
tools\xflash.ps1 binaries\Tag_FW_CC2630_TG-GR6000N.bin     # Windows PowerShell
```

or, by hand (the scripts run exactly this):

```
C:\ti\uniflash_9.5.0\dslite.bat --config=tools\cc2630_xds110.ccxml -e -f -v binaries\Tag_FW_CC2630_TG-GR6000N.bin,0x0
```

then **completely power cycle the board**. The XDS110's reset
pulse alone will *not* start the new image.

## Wiring

Remove (or just shift) the plastic jumpers for **TMS, TCK, RST and 3V3** from the LaunchPad's
isolation header (the row of jumpers between the XDS110 half and the target MCU
half of the board). Leave TDI/TDO unjumpered too. Connect wires to the **debugger
side** of the header only (the half nearest the USB connector):

| LaunchPad (debugger side) | Tag pad   | Notes                                                     |
|---------------------------|-----------|-----------------------------------------------------------|
| TCK                       | TCK       | cJTAG 2-pin                                               |
| TMS                       | TMS       | cJTAG 2-pin                                               |
| RST                       | Reset     | XDS110 drives RESET_N; needed for the connect sequence    |
| GND                       | GND       |                                                           |
| VBAT (or 3V3)             | BAT       | Powers the tag and gives the XDS110 its target-voltage reference |

Keep the TMS/TCK leads short (< 20 cm) and don't twist them together. The tag's
`TI_DN` pad (DIO11, the bootloader D/L pin) is **not** needed for this route.

## UniFlash configuration

The UniFlash GUI's "Detected devices" list only shows the LaunchPad's own MCU
(e.g. the CC3220), so don't pick that. The GUI still works for the tag if you
go to *New Configuration*, type **CC2630F128** into the device search box, choose
**Texas Instruments XDS110 USB Debug Probe** as the connection, and then apply
the three settings below under *Settings & Utilities*. It's just more clicking
every time — the scripts hand `dslite` a ready-made target configuration,
**`tools/cc2630_xds110.ccxml`**, with those settings already baked in.

The three settings that matter, all of which differ from UniFlash's defaults:

| Setting             | Value                          | Why                                                        |
|---------------------|--------------------------------|------------------------------------------------------------|
| SWD Mode Settings   | cJTAG (1149.7) 2-pin           | The tag only exposes TMS/TCK                               |
| Target Scan Format  | **OSCAN1** (`Value="0"`)       | OSCAN2 (the default) fails on this board with error -242   |
| JTAG TCLK Frequency | **500 kHz** (100 kHz also OK)  | Default clock is too fast for jumper wires to the tag      |

Device file must be `devices/cc2630f128.xml` — there is no `cc2630.xml`, and a
config that references one fails silently in the GUI.

## Flashing

`tools/xflash.sh` (bash) and `tools/xflash.ps1` (PowerShell) wrap the whole
thing: they find UniFlash (newest `~/ti/uniflash_*`, `/opt/ti/uniflash_*` or
`C:\ti\uniflash_*`, or `$UNIFLASH_DIR`), pick the right load address for the
file type, run flash + verify, and check the result.

```bash
tools/xflash.sh                                        # binaries/Tag_FW_CC2630_TG-GR6000N.bin
tools/xflash.sh build/Tag_FW_CC2630_TG-GR6000N.elf     # any .bin or .elf
```

Under the hood this is just `dslite` (`dslite.bat` / `dslite.sh` in the
UniFlash install root):

```
# .bin — full 128 KB image including CCFG; must be loaded at address 0
dslite.bat --config=tools/cc2630_xds110.ccxml -e -f -v Tag_FW_CC2630_TG-GR6000N.bin,0x0

# .elf — addresses come from the file, no offset
dslite.bat --config=tools/cc2630_xds110.ccxml -e -f -v Tag_FW_CC2630_TG-GR6000N.elf
```

`-f` flashes, `-v` verifies, `-e` prints progress. A good run ends with:

```
info: Cortex_M3_0: Program verification successful for ...
Success
```

## Booting the new image

After a successful flash the tag must be power cycled.

The tag draws the OEPL splash on power-up. Pulsing RESET_N from the XDS110
after flashing does **not** start the image — the screen stays as it was
until a real power cycle.

## Troubleshooting

All of these were hit while getting this working. Check them in order.

| Symptom | Cause / fix |
|---|---|
| UniFlash GUI only detects the LaunchPad's own MCU (CC3220 etc.) | Expected — the tag isn't auto-detected. Type `CC2630F128` into the device search manually, or skip the GUI and use the scripts. |
| `Error -242 ... A router subpath could not be accessed` | The XDS110 reaches the CC2630's IcePick but the DAP behind it doesn't answer. Seen with OSCAN2 (the default) and with a marginal TMS/TCK connection. Use OSCAN1; re-check the two JTAG wires. |
| `Error -2131 Unable to access device register` | Same root cause as -242, one step further in. Lower TCLK to 100 kHz; re-check TMS/TCK. |
| `Error -275` (target polling timeout) | Same family; OSCAN1 + slower clock. |
| Flash + verify succeed but screen doesn't change | Power cycle the tag |

Useful low-level checks (all in
`<uniflash>/deskdb/content/TICloudAgent/win/ccs_base/common/uscif/`):

```
# Is the XDS110 enumerated?
xds110\xdsdfu.exe -e

# Pulse the tag's RESET_N — the screen should flash if RST is wired correctly
dbgjtag.exe -f @xds110 -rv -o -Y reset,system=true,jtag=false

# Scan-chain integrity test in the same cJTAG 2-pin / OSCAN1 mode the flash uses.
# Passing this proves TMS/TCK/GND are good; a failing flash after this is a target-state problem.
dbgjtag.exe -f @xds110 -rv -o -S integrity -V apply,DOT7.DTS_USAGE=enable,DOT7.DTS_TYPE=xds110,DOT7.DTS_PROGRAM=emulator,DOT7.DTS_FREQUENCY=1.0MHz,DOT7.TS_FORMAT=oscan1,DOT7.TS_PIN_WIDTH=only_two,SWD.SWD_DEBUG=disabled

# Detailed log of a connect attempt (look for XDS_SELECTTAP return codes)
dslite.bat --config=tools/cc2630_xds110.ccxml -g dslite.log -S x
```

## If the firmware on the tag sleeps before the debugger connects

Not needed in testing, but worth knowing: both the stock and OEPL CCFG enable
the ROM bootloader backdoor on **DIO11 (`TI_DN` pad), active low**. Grounding
`TI_DN` while the XDS110 issues its board reset parks the chip in the ROM
bootloader, which never enters standby, so the JTAG connect can't lose the race
against a firmware that sleeps immediately after boot. Remove the ground before
power-cycling to boot the new image.
