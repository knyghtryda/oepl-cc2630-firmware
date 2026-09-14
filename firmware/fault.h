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

#endif
