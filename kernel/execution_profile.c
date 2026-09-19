#include "execution_profile.h"

#include "arch.h"

#define EXECUTION_CR0_MP (1ULL << 1)
#define EXECUTION_CR0_EM (1ULL << 2)
#define EXECUTION_CR0_TS (1ULL << 3)

bool execution_profile_fp_simd_restricted(void) {
    u64 cr0 = arch_read_cr0();
    return (cr0 & EXECUTION_CR0_MP) && !(cr0 & EXECUTION_CR0_EM) &&
        (cr0 & EXECUTION_CR0_TS);
}

bool execution_profile_init(void) {
    u64 cr0 = arch_read_cr0();

    /*
     * MP=1 extends TS handling to WAIT/FWAIT. EM remains clear because the
     * machine has a hardware FPU; TS is the deliberate availability gate.
     * No JCOS path clears TS in the restricted R4 profile.
     */
    cr0 |= EXECUTION_CR0_MP | EXECUTION_CR0_TS;
    cr0 &= ~EXECUTION_CR0_EM;
    arch_write_cr0(cr0);

    return execution_profile_fp_simd_restricted();
}
