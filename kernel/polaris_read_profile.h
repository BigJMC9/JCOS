#ifndef JA_OS_POLARIS_READ_PROFILE_H
#define JA_OS_POLARIS_READ_PROFILE_H
#include "polaris_mmio.h"
#include "polaris_capture.h"
#define POLARIS_READ_REPORT_VERSION 2U
#define POLARIS_READ_BLOCK_TARGET (1U << 0)
#define POLARIS_READ_BLOCK_BAR_PLAN (1U << 1)
#define POLARIS_READ_BLOCK_POWER (1U << 2)
#define POLARIS_READ_BLOCK_CPU (1U << 3)
#define POLARIS_READ_BLOCK_UART (1U << 4)
#define POLARIS_READ_BLOCK_SCANOUT (1U << 5)
#define POLARIS_READ_BLOCK_REGISTER_SAFETY (1U << 16)
#define POLARIS_READ_BLOCK_SERIAL_CONTROL (1U << 17)
#define POLARIS_READ_BLOCK_PHYSICAL_EVIDENCE (1U << 18)
#define POLARIS_READ_BLOCK_PIPE_POWER (1U << 19)
/* No caller input, successful inspection or serial connection can remove these
 * unresolved qualifications. This report is never an access/grant token. */
#define POLARIS_READ_UNQUALIFIED (POLARIS_READ_BLOCK_REGISTER_SAFETY | \
    POLARIS_READ_BLOCK_SERIAL_CONTROL | POLARIS_READ_BLOCK_PHYSICAL_EVIDENCE | POLARIS_READ_BLOCK_PIPE_POWER)
typedef struct {
    u32 version;
    u32 blockers;
    PciAddress address;
    u16 command, pmc, pmcsr;
    u64 bar_base, bar_size;
    u32 page_count;
} PolarisReadReport;
bool polaris_read_profile_evaluate(const BootInfo *boot, const PciDevice *device,
    const DisplayProbeInfo *info, u16 pmc, u16 pmcsr, bool cpu_ready,
    bool uart_available, bool firmware_valid, PolarisReadReport *out);
/* Current read-only config observations only: no assignment, mapping, grant,
 * MMIO, reset or display writes. Unknown selected target stays blocked. */
bool polaris_read_profile_inspect(const BootInfo *boot, PolarisReadReport *out);
void polaris_read_profile_print(const BootInfo *boot);
#endif
