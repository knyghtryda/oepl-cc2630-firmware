#ifndef FAULT_H
#define FAULT_H

#include <stdint.h>

// HardFault post-mortem record. Lives in .noinit so it survives the reset the
// handler performs; main() consumes and clears it at the next boot.
#define FAULT_MAGIC 0xFA17DEAD

// Pseudo-PCs for non-HardFault resets that use the same record
#define FAULT_PC_RF_DOORBELL 0xDEAD0DB0   // RF core stopped answering the doorbell
#define FAULT_PC_WATCHDOG    0xDEADD006   // watchdog reset (no record from the hang itself)

struct fault_record {
    uint32_t magic;
    uint32_t pc;
    uint32_t lr;
    uint32_t sp;
    uint32_t cfsr;
    uint32_t bfar;
};

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
