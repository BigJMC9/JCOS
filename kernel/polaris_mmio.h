#ifndef JA_OS_POLARIS_MMIO_H
#define JA_OS_POLARIS_MMIO_H
#include "display_probe.h"
#include "pci_assignment.h"
#include "vmm.h"
#include "amd_polaris_registers.h"

#define POLARIS_MMIO_PAGE_COUNT 4U
#define POLARIS_MMIO_BINDING_CAPACITY 4U
#define POLARIS_MMIO_HOLD_CAPACITY 16U
/* Reserved in the existing, shared supervisor-only PML4 slot zero. */
#define POLARIS_MMIO_WINDOW_BASE 0x0000006000000000ULL
#define POLARIS_MMIO_WINDOW_STRIDE 0x10000ULL

typedef struct {
    BootDisplayBar bar;
    u32 page_offsets[POLARIS_MMIO_PAGE_COUNT];
    u32 page_count;
    u16 command;
    u16 pmcsr;
    u8 pm_offset;
} PolarisMmioPlan;

typedef struct {
    PciAssignment *assignment;
    Process *owner;
    VmPageMap *map;
    u64 identity;
    u64 assignment_id;
    u64 owner_id;
    frame_t root_frame;
    u64 virtual_base;
    PolarisMmioPlan plan;
    u32 mapped_pages;
    u32 active_holds;
    bool location_mapped;
    bool child_pinned;
    bool access_pinned;
    bool prepared;
    bool closing;
    bool poisoned;
    bool initialized;
} PolarisMmioBinding;

typedef struct {
    PolarisMmioBinding *binding;
    u64 binding_identity;
    u64 identity;
    bool initialized;
} PolarisMmioHold;

/* Kernel operation retention only, never GPU-read permission. Holds prevent
 * leaf removal even after owner death/quarantine. IDs reject stale releases
 * after token/binding storage reuse. Caller storage must remain valid until
 * successful release; release only after the retained CPU operation stops.
 * No raw address, register load or hardware completion proof is exposed. */
u64 polaris_mmio_binding_identity(const PolarisMmioBinding *binding);
bool polaris_mmio_hold_acquire(PolarisMmioHold *hold, PolarisMmioBinding *binding,
    u64 expected_binding_identity, Process *owner, u64 *out_identity);
bool polaris_mmio_hold_release(PolarisMmioHold *hold, u64 expected_hold_identity);
u32 polaris_mmio_hold_count(void);
/* Kernel-only bring-up semantic load under an exact retained hold. Requires
 * active kernel CR3 and enabled interrupts; no raw offset or user mapping.
 * The experiment controller owns pipe selection/state and total read budget. */
bool polaris_mmio_read_held(PolarisMmioHold *hold, u64 expected_hold_identity,
    Process *owner, u32 pipe, PolarisDisplayRegister field, u32 *out);

bool polaris_mmio_read_witness_held(PolarisMmioHold *hold,u64 expected_hold_identity,
    Process *owner,u32 pipe,PolarisPlaneWitnessRegister field,u32 *out);

/* Optional fifth RO/NX/UC page, for the two fixed GMC/HDP location fields.
 * Separate from the four-page pipe plan. No hold may exist during preparation. */
typedef enum {
    POLARIS_LOCATION_MC_FB = 0,
    POLARIS_LOCATION_HDP_BASE,
    POLARIS_LOCATION_COUNT
} PolarisLocationRegister;
bool polaris_mmio_prepare_location(PolarisMmioBinding *binding, const BootInfo *boot,
    Process *owner, u64 expected_binding_identity);
bool polaris_mmio_read_location_held(PolarisMmioHold *hold, u64 expected_hold_identity,
    Process *owner, PolarisLocationRegister field, u32 *out);

/* Permanent admission closure; safe without active kernel CR3. Cleanup also
 * closes admission before attempting teardown and refuses while holds remain. */
bool polaris_mmio_close(PolarisMmioBinding *binding);

/* Pure preparation plan for the identified board candidate. Requires an exact,
 * complete one-descriptor firmware BAR5 response, current D0/decode observation
 * and all semantic fields in bounds. This is not a safe-read qualification. */
bool polaris_mmio_plan(const BootInfo *boot, const PciDevice *device,
    const DisplayProbeInfo *info, u16 pmcsr, PolarisMmioPlan *out);

/* Kernel-only staging: no capability, read function, activation or user mapping.
 * Creates supervisor-only RO/NX/UC leaves for the four required register pages.
 * Caller must be on the active kernel page map, UP/no-PCID. There are no PCI
 * writes, BAR sizing, MMIO transactions or scanout changes.
 * false may retain a registered cleanup record and both assignment pins. */
bool polaris_mmio_prepare(PolarisMmioBinding *binding, const BootInfo *boot,
    PciAssignment *assignment, Process *owner, CapabilityHandle assignment_handle);
bool polaris_mmio_binding_valid(const PolarisMmioBinding *binding);
bool polaris_mmio_binding_prepared(const PolarisMmioBinding *binding);
u32 polaris_mmio_binding_count(void);
/* Retryable after owner death/quarantine. Removes leaves before releasing pins.
 * Never frees MMIO frames. Unexpected identity/leaf changes retain the record.
 * Exported kernel page-table scaffolding is bounded and boot-pinned by the VMM. */
bool polaris_mmio_cleanup(PolarisMmioBinding *binding);
#endif
