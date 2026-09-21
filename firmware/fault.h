// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Nathan Bigelow
#ifndef FAULT_H
#define FAULT_H

#include <stdint.h>
#include <stdbool.h>
#include "oepl_radio_cc2630.h"   // TAG_FW_VERSION

// HardFault post-mortem record. Lives in .noinit so it survives the reset the
// handler performs; main() consumes and clears it at the next boot.
#define FAULT_MAGIC 0xFA17DEAD

// Pseudo-PCs for non-HardFault resets that use the same record
#define FAULT_PC_RF_DOORBELL 0xDEAD0DB0   // RF core stopped answering the doorbell
#define FAULT_PC_WATCHDOG    0xDEADD006   // watchdog reset (no record from the hang itself)

// Records in .noinit survive the reset that follows a fault -- but SRAM also
// keeps its contents through a brief power cycle and through reflashing, so a
// magic word alone let stale bytes from a previous build look like a fresh
// fault. Every record therefore carries a build id and a checksum, and the
// reader also sanity-checks the PC.
#define FAULT_BUILD_ID ((uint32_t)(TAG_FW_VERSION) ^ 0x8ULL ^ \
                        (uint32_t)(sizeof(struct fault_record)) ^ 0xB011D000u)

struct fault_record {
    uint32_t magic;
    uint32_t build;
    uint32_t pc;
    uint32_t lr;
    uint32_t sp;
    uint32_t cfsr;
    uint32_t bfar;
    uint32_t sum;      // ~(magic + build + pc + lr + sp + cfsr + bfar)
};

static inline uint32_t fault_record_sum(const volatile struct fault_record *f)
{
    return ~(f->magic + f->build + f->pc + f->lr + f->sp + f->cfsr + f->bfar);
}

// Is this address something the CPU could have been executing?
static inline bool fault_pc_plausible(uint32_t pc)
{
    return (pc < 0x00020000u) ||                      // flash
           (pc >= 0x20000000u && pc < 0x20000200u) || // .ramfunc (OTA apply)
           (pc >= 0x10000000u && pc < 0x10040000u) || // ROM
           (pc & 0xFFFF0000u) == 0xDEAD0000u;         // our pseudo-PCs
}

static inline bool fault_record_valid(const volatile struct fault_record *f)
{
    return f->magic == FAULT_MAGIC && f->build == FAULT_BUILD_ID &&
           f->sum == fault_record_sum(f) && fault_pc_plausible(f->pc);
}

static inline void fault_record_seal(volatile struct fault_record *f)
{
    f->magic = FAULT_MAGIC;
    f->build = FAULT_BUILD_ID;
    f->sum = fault_record_sum(f);
}

extern volatile struct fault_record g_fault;

// Crash capture: the last CRASH_LOG_SIZE characters of debug output before a
// fault/hang plus a few registers, frozen into .noinit at the moment of the
// fault so they survive the reset. Read it with a debugger any time before
// power is removed (tools/crash_dump.py); the next fault overwrites it.
#define CRASH_MAGIC     0xC4A5D0C5
#define CRASH_LOG_SIZE  1024
#define CRASH_NREGS     16

struct crash_capture {
    uint32_t magic;
    uint32_t build;                  // FAULT_BUILD_ID; stale RAM won't match
    uint32_t count;                  // faults captured since power-up (noinit, best effort)
    uint32_t code;                   // fault_record.pc value (pseudo-PC or real PC)
    uint32_t op;                     // doorbell: command word; HardFault: LR
    uint32_t reg[CRASH_NREGS];       // see the capture sites
    uint32_t log_len;
    char     log[CRASH_LOG_SIZE];    // oldest first
};

extern struct crash_capture g_crash;

// Freeze the recent debug output and the given registers into g_crash
void crash_capture(uint32_t code, uint32_t op, const uint32_t *regs, unsigned nregs);

#endif
