#!/usr/bin/env python3
"""Read the crash capture (struct crash_capture g_crash, fault.h) from the bench
tag over J-Link and print it: fault code, the hung RF command, registers, and
the last ~1 KB of debug output before the fault.

    tools/crash_dump.py [out_prefix]      (tag powered, e.g. tools/ppk.py hold)

Symbols come from build/Tag_FW_CC2630_TG-GR6000N.elf, which must match the
flashed image. Attaching keeps the CC26xx debug domain on until power is cut.
"""
import os, struct, subprocess, sys, tempfile

ELF = "build/Tag_FW_CC2630_TG-GR6000N.elf"
CRASH_MAGIC = 0xC4A5D0C5
DIRECT_CMDS = {0x0401: "CMD_ABORT", 0x0402: "CMD_STOP", 0x0403: "CMD_GET_RSSI", 0x0404: "CMD_TRIGGER",
               0x0405: "CMD_START_RAT", 0x0406: "CMD_PING", 0x0801: "CMD_GET_FW_INFO"}
DOORBELL_REGS = ["phase(1=CMDR busy,2=no ACK)", "CMDR", "CMDSTA", "RFHWIFG", "RFCPEIFG", "RFACKIFG",
                 "PRCM PDSTAT0", "PRCM PDSTAT0RFC", "PRCM RFCCLKG", "AON_WUC PWRSTAT",
                 "rf_cmd_rx.status", "rf_cmd_tx.status", "rf_cmd_setup.status", "rf_cmd_fs.status",
                 "RTC sec", "RTC subsec"]
HARDFAULT_REGS = ["r0", "r1", "r2", "r3", "r12", "lr", "pc", "xpsr", "CFSR", "HFSR", "BFAR", "MMFAR"]


def symbols():
    out = subprocess.run(["arm-none-eabi-nm", "-S", ELF], capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        f = line.split()
        if len(f) == 4:
            syms[f[3]] = (int(f[0], 16), int(f[1], 16))
    return syms


def read_mem(addr, size):
    with tempfile.TemporaryDirectory() as d:
        binf = os.path.join(d, "g_crash.bin")
        cmd = os.path.join(d, "cmd.txt")
        open(cmd, "w").write(f"connect\nCC2630F128\nT\n1000\n\nsavebin {binf} 0x{addr:08X} 0x{size:X}\nexit\n")
        subprocess.run(["timeout", "40", "JLinkExe", "-nogui", "1"], stdin=open(cmd), capture_output=True)
        if not os.path.exists(binf):
            sys.exit("J-Link read failed")
        return open(binf, "rb").read()


def main():
    syms = symbols()
    addr, size = syms["g_crash"]
    raw = read_mem(addr, size)
    magic, build, count, code, op = struct.unpack_from("<5I", raw, 0)
    regs = struct.unpack_from("<16I", raw, 20)
    (log_len,) = struct.unpack_from("<I", raw, 20 + 64)
    log = raw[20 + 64 + 4:20 + 64 + 4 + min(log_len, 1024)]
    if magic != CRASH_MAGIC:
        print(f"no crash capture (magic 0x{magic:08X})")
        return 1
    print(f"captures since power-up: {count} (build id 0x{build:08X})")
    if code == 0xDEAD0DB0:
        if op & 1:
            cid = (op >> 16) & 0xFFFF
            what = f"direct command 0x{cid:04X} {DIRECT_CMDS.get(cid, '')}"
        else:
            what = next((f"&{n}" for n, (a, s) in syms.items() if a == op), f"struct @0x{op:08X}")
        print(f"RF doorbell hang: {what}")
        names = DOORBELL_REGS
    elif code == 0xDEADD006:
        print("watchdog reset"); names = []
    else:
        print(f"HardFault at PC 0x{code:08X}, LR 0x{op:08X}")
        names = HARDFAULT_REGS
    for n, v in zip(names, regs):
        print(f"  {n:28s} 0x{v:08X}")
    text = log.replace(b"\0", b"").decode(errors="replace").replace("\r", "")
    print("--- last output before the fault ---")
    print(text)
    if len(sys.argv) > 1:
        open(sys.argv[1] + ".bin", "wb").write(raw)
        open(sys.argv[1] + ".txt", "w").write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
