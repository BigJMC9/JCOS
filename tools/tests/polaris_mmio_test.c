/* Actual binding/plan/firmware decoders; routing/VM doubles inject failures.
 * No GPU or CPU MMIO access occurs. Leaf flags get independent production-VMM
 * coverage in vmm_device_test.c; real assignment taint gets QEMU coverage. */
#include <stdio.h>
#include <string.h>
#include "../../kernel/tests/polaris_mmio_fixture.h"
#include "../../kernel/tests/polaris_mmio_plan_test.h"
#include "../../kernel/address_space.h"
#include "../../kernel/arch.h"
#include "../../kernel/physmap.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "binding line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static BootInfo boot;
static PciDevice device;
static DisplayProbeInfo info;
static u8 config[PCI_CONFIG_SPACE_SIZE];
static BootMemoryDescriptor memory;
static Process owner;
static PciAssignment assignment;
static AddressSpace kernel;
static bool exited, supported = true, wrong_context, fail_child, fail_access;
static bool fail_access_release, fail_child_release, change_after_map, corrupt_unmap_result;
static u32 map_calls, unmap_calls, config_reads, fail_map, fail_unmap;
static u64 alias;
static bool change_location;
static bool load_interrupts = true;
static u32 load_calls;
static u64 load_address;
bool device_mmio_interrupts_enabled(void) { return load_interrupts; }
u32 device_mmio_load32(u64 address) {
    ++load_calls; load_address = address; return 0x12345678U;
}
typedef struct { u64 address; frame_t frame; vm_flags_t flags; bool present; } Leaf;
static Leaf leaves[16];
void interrupts_disable(void) {}
void interrupts_enable(void) {}
u64 arch_read_cr3(void) { return wrong_context ? 0x8000ULL : frame_to_phys(kernel.page_map.root_frame); }
u64 arch_read_cr4(void) { return 0; }
frame_t phys_to_frame(u64 p) { return p / VM_PAGE_SIZE; }
u64 frame_to_phys(frame_t f) { return f * VM_PAGE_SIZE; }
AddressSpace *address_space_kernel(void) { return &kernel; }
bool vmm_device_memory_supported(void) { return supported; }
bool process_storage_in_use(const Process *p) { return p == &owner; }
bool process_exit_info(const Process *p, ProcessExitInfo *exit) { (void)exit; return p == &owner && exited; }
bool pci_device_is_enumerated(const PciDevice *p) { return p == &device; }
bool pci_address_valid(PciAddress a) { return a.device < 32U && a.function < 8U; }
u8 pci_config_read8(PciAddress a, u16 o) { (void)a; ++config_reads; return config[o]; }
u16 pci_config_read16(PciAddress a, u16 o) {
    return (u16)pci_config_read8(a, o) | ((u16)pci_config_read8(a, o + 1U) << 8);
}
u32 pci_config_read32(PciAddress a, u16 o) {
    return (u32)pci_config_read16(a, o) | ((u32)pci_config_read16(a, o + 2U) << 16);
}
bool pci_assignment_valid(const PciAssignment *a) { return a == &assignment && a->initialized; }
DeviceResource *pci_assignment_resource(PciAssignment *a) { return pci_assignment_valid(a) ? &a->resource : 0; }
u64 pci_assignment_owner_process_id(const PciAssignment *a) { return pci_assignment_valid(a) ? a->owner_process_id : 0; }
PciAssignmentState pci_assignment_state(const PciAssignment *a) { return pci_assignment_valid(a) ? a->state : PCI_ASSIGNMENT_INVALID; }
const PciDevice *pci_assignment_enumerated_device(const PciAssignment *a) { return pci_assignment_valid(a) ? &device : 0; }
bool pci_assignment_authorized(PciAssignment *a, Process *p, CapabilityHandle h) {
    return pci_assignment_valid(a) && p == &owner && h == 17U && a->state == PCI_ASSIGNMENT_ASSIGNED &&
        a->owner_process_id == p->id;
}
bool pci_assignment_dma_gate_active(const PciAssignment *a) { return a->dma_gate_active; }
bool pci_assignment_note_inherited_bus_master(PciAssignment *a, u64 id) {
    if (!pci_assignment_valid(a) || a->owner_process_id != id || a->state != PCI_ASSIGNMENT_ASSIGNED) return false;
    a->bus_master_exposed = true; return true;
}
bool pci_assignment_child_acquire(PciAssignment *a, Process *p, CapabilityHandle h) {
    if (fail_child || !pci_assignment_authorized(a, p, h)) return false;
    ++a->child_resource_count; return true;
}
bool pci_assignment_access_acquire(PciAssignment *a, u64 id) {
    if (fail_access || !pci_assignment_valid(a) || a->owner_process_id != id) return false;
    ++a->active_access_count; return true;
}
bool pci_assignment_access_release(PciAssignment *a, u64 id) {
    if (fail_access_release || !pci_assignment_valid(a) || a->owner_process_id != id || !a->active_access_count) return false;
    --a->active_access_count; return true;
}
bool pci_assignment_child_release(PciAssignment *a, u64 id) {
    if (fail_child_release || !pci_assignment_valid(a) || a->owner_process_id != id || !a->child_resource_count) return false;
    --a->child_resource_count; return true;
}
bool vmm_query_page(const VmPageMap *m, u64 address, frame_t *f, vm_flags_t *flags) {
    if (m != &kernel.page_map) return false;
    if (address == alias && alias) return true;
    for (u32 i = 0; i < 16U; ++i) {
        if (!leaves[i].present || leaves[i].address != address) continue;
        if (f) *f = leaves[i].frame;
        if (flags) *flags = leaves[i].flags;
        return true;
    }
    return false;
}
bool vmm_map_page(VmPageMap *m, u64 address, frame_t f, vm_flags_t flags) {
    ++map_calls;
    if (map_calls == fail_map || m != &kernel.page_map || flags != VM_DEVICE ||
        address < POLARIS_MMIO_WINDOW_BASE || address >= POLARIS_MMIO_WINDOW_BASE + 0x40000ULL ||
        vmm_query_page(m, address, 0, 0)) return false;
    if (change_location && map_calls == 5U) config[8]++;
    if (change_after_map && map_calls == 4U) config[8]++; /* identity drift after leaves */
    for (u32 i = 0; i < 16U; ++i) {
        if (leaves[i].present) continue;
        leaves[i] = (Leaf){address, f, flags, true}; return true;
    }
    return false;
}
bool vmm_unmap_page(VmPageMap *m, u64 address, frame_t *old) {
    ++unmap_calls;
    if (unmap_calls == fail_unmap || m != &kernel.page_map ||
        !assignment.active_access_count || !assignment.child_resource_count) return false;
    for (u32 i = 0; i < 16U; ++i) {
        if (!leaves[i].present || leaves[i].address != address) continue;
        *old = leaves[i].frame + (corrupt_unmap_result ? 1U : 0U); leaves[i].present = false; return true;
    }
    return false;
}
static bool reset(void) {
    if (polaris_mmio_binding_count()) return false;
    memset(leaves, 0, sizeof(leaves));
    memset(&owner, 0, sizeof(owner)); owner.id = 9U; owner.initialized = true;
    memset(&assignment, 0, sizeof(assignment));
    assignment.resource.id = 101U; assignment.initialized = true;
    assignment.state = PCI_ASSIGNMENT_ASSIGNED; assignment.owner_process_id = owner.id;
    memset(&kernel, 0, sizeof(kernel)); kernel.page_map.root_frame = 5U;
    memset(&memory, 0, sizeof(memory));
    memory.type = 11U; memory.physical_start = 0x63400000ULL; memory.number_of_pages = 64U;
    change_location=false;
    supported = true; exited = wrong_context = fail_child = fail_access = false;
    fail_access_release = fail_child_release = change_after_map = corrupt_unmap_result = false;
    map_calls = unmap_calls = config_reads = fail_map = fail_unmap = 0; alias = 0;
    if (!polaris_mmio_fixture(&boot, &device, &info, config)) return false;
    boot.memory_map = (u64)&memory; boot.memory_map_size = sizeof(memory);
    boot.memory_map_descriptor_size = sizeof(memory);
    return true;
}
static bool empty(void) {
    if (polaris_mmio_binding_count() || polaris_mmio_hold_count() ||
        assignment.child_resource_count || assignment.active_access_count)
        return false;
    for (u32 i = 0; i < 16U; ++i) if (leaves[i].present) return false;
    return true;
}
int main(int argc, char **argv) {
    CHECK(polaris_mmio_plan_test());
    CHECK(!polaris_mmio_binding_valid((void *)1));
    CHECK(!polaris_mmio_binding_prepared((void *)1));
    CHECK(!polaris_mmio_cleanup((void *)1));
    PolarisMmioBinding b, duplicate;
    if (argc==2 && strcmp(argv[1],"location-poison")==0) {
        CHECK(reset());
        CHECK(polaris_mmio_prepare(&b,&boot,&assignment,&owner,17U));
        CHECK(polaris_mmio_prepare_location(&b,&boot,&owner,polaris_mmio_binding_identity(&b)));
        corrupt_unmap_result=true;
        CHECK(!polaris_mmio_cleanup(&b) && b.poisoned && !b.location_mapped &&
            b.mapped_pages==4 && assignment.active_access_count==1 && assignment.child_resource_count==1);
        corrupt_unmap_result=false;
        CHECK(!polaris_mmio_cleanup(&b) && unmap_calls==1);
        puts("PASS: inconsistent global-page unmap retains poisoned binding, four pipe leaves and both pins.");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "poison") == 0) {
        CHECK(reset());
        CHECK(polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U));
        corrupt_unmap_result = true;
        CHECK(!polaris_mmio_cleanup(&b) && b.poisoned && b.mapped_pages == 3U &&
            !polaris_mmio_binding_prepared(&b) && polaris_mmio_binding_valid(&b) &&
            assignment.active_access_count == 1U && assignment.child_resource_count == 1U);
        corrupt_unmap_result = false;
        CHECK(!polaris_mmio_cleanup(&b) && unmap_calls == 1U);
        puts("PASS: inconsistent committed unmap poisons record; remaining leaves and both pins stay retained.");
        return 0;
    }
    /* Optional global page retains the same assignment pins and cleanup ledger. */
    CHECK(reset());
    CHECK(polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U));
    u64 location_binding_id = polaris_mmio_binding_identity(&b), location_hold_id = 0;
    PolarisMmioHold location_hold;
    u32 location_value = ~0U;
    CHECK(!polaris_mmio_prepare_location(&b, &boot, &owner, location_binding_id+1));
    CHECK(!polaris_mmio_prepare_location(&b, &boot, (Process *)1, location_binding_id));
    CHECK(polaris_mmio_hold_acquire(&location_hold, &b, location_binding_id, &owner, &location_hold_id));
    CHECK(!polaris_mmio_prepare_location(&b, &boot, &owner, location_binding_id));
    CHECK(!polaris_mmio_read_location_held(&location_hold, location_hold_id, &owner,
        POLARIS_LOCATION_MC_FB, &location_value) && !location_value && !load_calls);
    CHECK(polaris_mmio_hold_release(&location_hold, location_hold_id));
    CHECK(polaris_mmio_prepare_location(&b, &boot, &owner, location_binding_id));
    CHECK(b.location_mapped && map_calls==5 && b.mapped_pages==4 &&
        assignment.active_access_count==1 && assignment.child_resource_count==1);
    CHECK(!polaris_mmio_prepare_location(&b, &boot, &owner, location_binding_id));
    CHECK(polaris_mmio_hold_acquire(&location_hold, &b, location_binding_id, &owner, &location_hold_id));
    for (unsigned field=0; field<2; ++field) {
        CHECK(polaris_mmio_read_location_held(&location_hold, location_hold_id, &owner,
            (PolarisLocationRegister)field, &location_value) && location_value==0x12345678U &&
            load_address==b.virtual_base+0x4000ULL+(field ? 0xC04ULL : 0x24ULL));
    }
    CHECK(load_calls==2);
    for (unsigned refusal=0; refusal<9; ++refusal) {
        if (refusal==0) load_interrupts=false;
        if (refusal==1) wrong_context=true;
        if (refusal==2) exited=true;
        if (refusal==3) assignment.state=PCI_ASSIGNMENT_QUARANTINED;
        if (refusal==4) assignment.dma_gate_active=true;
        if (refusal==5) leaves[4].flags=VM_DEVICE|VM_USER;
        if (refusal==6) leaves[4].frame++;
        CHECK(!polaris_mmio_read_location_held(&location_hold, location_hold_id+(refusal==7),
            &owner, (PolarisLocationRegister)(refusal==8 ? 2 : 0), &location_value) &&
            !location_value && load_calls==2);
        load_interrupts=true; wrong_context=exited=false; assignment.state=PCI_ASSIGNMENT_ASSIGNED;
        assignment.dma_gate_active=false; leaves[4].flags=VM_DEVICE;
        if (refusal==6) leaves[4].frame--;
    }
    CHECK(!polaris_mmio_cleanup(&b) && !unmap_calls && b.location_mapped);
    CHECK(polaris_mmio_hold_release(&location_hold, location_hold_id));
    fail_unmap=1;
    CHECK(!polaris_mmio_cleanup(&b) && b.location_mapped && b.mapped_pages==4);
    fail_unmap=0;
    CHECK(polaris_mmio_cleanup(&b) && empty() && unmap_calls==6);
    CHECK(reset());
    CHECK(polaris_mmio_prepare(&b,&boot,&assignment,&owner,17U));
    change_location=true;
    CHECK(!polaris_mmio_prepare_location(&b,&boot,&owner,polaris_mmio_binding_identity(&b)) &&
        b.location_mapped && b.closing && !polaris_mmio_binding_prepared(&b));
    CHECK(polaris_mmio_cleanup(&b) && empty());
    load_calls=0;
    for (unsigned refusal=0; refusal<7; ++refusal) {
        CHECK(reset());
        CHECK(polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U));
        location_binding_id=polaris_mmio_binding_identity(&b);
        if (refusal==0) fail_map=5;
        if (refusal==1) alias=b.plan.bar.base+0x2000;
        if (refusal==2) {
            CHECK(physmap_virtual_address(b.plan.bar.base+0x2000,&alias));
        }
        if (refusal==3) alias=b.virtual_base+0x4000;
        if (refusal==4) memory.type=7;
        if (refusal==5) config[4]=0;
        if (refusal==6) wrong_context=true;
        CHECK(!polaris_mmio_prepare_location(&b,&boot,&owner,location_binding_id) && !b.location_mapped);
        alias=0; memory.type=11; wrong_context=false;
        CHECK(polaris_mmio_cleanup(&b) && empty());
    }
    CHECK(reset());
    CHECK(polaris_mmio_prepare(&b,&boot,&assignment,&owner,17U));
    PolarisMmioHold witness_hold;
    u64 witness_hold_id=0;
    CHECK(polaris_mmio_hold_acquire(&witness_hold,&b,polaris_mmio_binding_identity(&b),&owner,&witness_hold_id));
    const u32 witness_offset[6][3]={
        {0x6844,0x6848,0x6E98},{0x7044,0x7048,0x7698},{0x7844,0x7848,0x7E98},
        {0x10044,0x10048,0x10698},{0x10844,0x10848,0x10E98},{0x11044,0x11048,0x11698}};
    u32 witness_value=~0U;
    for (u32 pipe=0;pipe<6;++pipe) for (u32 field=0;field<3;++field) {
        u32 offset=0;
        CHECK(jcos_polaris_witness_offset(pipe,(PolarisPlaneWitnessRegister)field,0x40000,&offset) &&
            offset==witness_offset[pipe][field]);
        CHECK(!jcos_polaris_witness_offset(pipe,(PolarisPlaneWitnessRegister)field,offset+3ULL,&offset) &&
            !offset);
        u32 slot=0;
        while (slot<b.plan.page_count && b.plan.page_offsets[slot]!=(witness_offset[pipe][field]&~0xFFFU)) ++slot;
        CHECK(polaris_mmio_read_witness_held(&witness_hold,witness_hold_id,&owner,pipe,
            (PolarisPlaneWitnessRegister)field,&witness_value) && witness_value==0x12345678U &&
            load_address==b.virtual_base+(u64)slot*VM_PAGE_SIZE+(witness_offset[pipe][field]&0xFFFU));
    }
    CHECK(load_calls==18);
    CHECK(!polaris_mmio_read_witness_held(&witness_hold,witness_hold_id,&owner,6,
        POLARIS_WITNESS_UPDATE,&witness_value) && !witness_value && load_calls==18);
    CHECK(!polaris_mmio_read_witness_held(&witness_hold,witness_hold_id,&owner,0,
        (PolarisPlaneWitnessRegister)~0U,&witness_value) && !witness_value && load_calls==18);
    load_interrupts=false;
    CHECK(!polaris_mmio_read_witness_held(&witness_hold,witness_hold_id,&owner,0,
        POLARIS_WITNESS_UPDATE,&witness_value) && !witness_value && load_calls==18);
    load_interrupts=true; leaves[0].flags=VM_DEVICE|VM_WRITE;
    CHECK(!polaris_mmio_read_witness_held(&witness_hold,witness_hold_id,&owner,0,
        POLARIS_WITNESS_UPDATE,&witness_value) && !witness_value && load_calls==18);
    leaves[0].flags=VM_DEVICE;
    CHECK(polaris_mmio_close(&b));
    CHECK(!polaris_mmio_read_witness_held(&witness_hold,witness_hold_id,&owner,0,
        POLARIS_WITNESS_UPDATE,&witness_value) && !witness_value && load_calls==18);
    CHECK(polaris_mmio_hold_release(&witness_hold,witness_hold_id) && polaris_mmio_cleanup(&b) && empty());
    load_calls=0;
    CHECK(reset());
    CHECK(polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U));
    CHECK(polaris_mmio_binding_prepared(&b) && map_calls == 4U &&
        assignment.child_resource_count == 1U && assignment.active_access_count == 1U);
    CHECK(!polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U));
    CHECK(!polaris_mmio_prepare(&duplicate, &boot, &assignment, &owner, 17U));
    CHECK(polaris_mmio_cleanup(&b) && empty());
    CHECK(reset());
    CHECK(polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U));
    u64 binding_id = polaris_mmio_binding_identity(&b), hold_id = 0;
    PolarisMmioHold hold, copied_hold, replacement_hold;
    CHECK(binding_id && !polaris_mmio_binding_identity((void *)1));
    CHECK(!polaris_mmio_hold_release((void *)1, 1ULL));
    CHECK(!polaris_mmio_hold_acquire(&hold, &b, binding_id + 1ULL, &owner, &hold_id) && !hold_id);
    CHECK(!polaris_mmio_hold_acquire(&hold, &b, binding_id, (Process *)1, &hold_id) && !hold_id);
    CHECK(polaris_mmio_hold_acquire(&hold, &b, binding_id, &owner, &hold_id) &&
        hold_id && b.active_holds == 1U && polaris_mmio_hold_count() == 1U);
    u32 value = ~0U;
    for (u32 pipe = 0; pipe < POLARIS_DISPLAY_PIPE_COUNT; ++pipe) {
        for (u32 field = 0; field < POLARIS_DISPLAY_REGISTER_COUNT; ++field) {
            u32 offset = 0;
            CHECK(jcos_polaris_register_offset(pipe, (PolarisDisplayRegister)field, b.plan.bar.size, &offset));
            u32 page = 0;
            while (page < b.plan.page_count && b.plan.page_offsets[page] != (offset & ~0xFFFU)) ++page;
            CHECK(polaris_mmio_read_held(&hold, hold_id, &owner, pipe, (PolarisDisplayRegister)field, &value) &&
                value == 0x12345678U && load_address == b.virtual_base + (u64)page * VM_PAGE_SIZE + (offset & 0xFFFU));
        }
    }
    CHECK(load_calls == 90U);
    for (u32 refusal = 0; refusal < 8U; ++refusal) {
        if (refusal == 0) load_interrupts = false;
        if (refusal == 1) wrong_context = true;
        if (refusal == 2) exited = true;
        if (refusal == 3) assignment.state = PCI_ASSIGNMENT_QUARANTINED;
        if (refusal == 4) assignment.dma_gate_active = true;
        if (refusal == 5) leaves[0].flags = VM_DEVICE | VM_WRITE;
        if (refusal == 6) leaves[0].frame++;
        value = ~0U;
        CHECK(!polaris_mmio_read_held(&hold, hold_id + (refusal == 7), &owner, 0U,
            POLARIS_REG_CRTC_CONTROL, &value) && !value && load_calls == 90U);
        load_interrupts = true; wrong_context = exited = false;
        assignment.state = PCI_ASSIGNMENT_ASSIGNED; assignment.dma_gate_active = false;
        leaves[0].flags = VM_DEVICE;
        if (refusal == 6) leaves[0].frame--;
    }
    CHECK(!polaris_mmio_read_held(&hold, hold_id, &owner, 6U, POLARIS_REG_CRTC_CONTROL, &value));
    CHECK(!polaris_mmio_read_held(&hold, hold_id, &owner, 0U, POLARIS_DISPLAY_REGISTER_COUNT, &value));
    CHECK(!polaris_mmio_read_held((void *)1, hold_id, &owner, 0U, POLARIS_REG_CRTC_CONTROL, &value));
    CHECK(load_calls == 90U);
    copied_hold = hold;
    CHECK(!polaris_mmio_hold_release(&copied_hold, hold_id));
    u64 rejected_id = ~0ULL;
    CHECK(!polaris_mmio_hold_acquire(&hold, &b, binding_id, &owner, &rejected_id) && !rejected_id);
    CHECK(!polaris_mmio_cleanup(&b) && b.closing && b.mapped_pages == 4U && !unmap_calls &&
        assignment.active_access_count == 1U && assignment.child_resource_count == 1U);
    CHECK(!polaris_mmio_hold_acquire(&replacement_hold, &b, binding_id, &owner, &rejected_id) && !rejected_id);
    exited = true; assignment.state = PCI_ASSIGNMENT_QUARANTINED;
    CHECK(!polaris_mmio_hold_release(&hold, hold_id + 1ULL));
    CHECK(polaris_mmio_hold_release(&hold, hold_id) && !b.active_holds &&
        !polaris_mmio_hold_release(&hold, hold_id));
    CHECK(polaris_mmio_cleanup(&b) && empty());
    CHECK(reset());
    CHECK(polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U));
    CHECK(polaris_mmio_binding_identity(&b) != binding_id);
    CHECK(!polaris_mmio_hold_acquire(&hold, &b, binding_id, &owner, &rejected_id));
    binding_id = polaris_mmio_binding_identity(&b);
    u64 new_hold_id = 0;
    CHECK(polaris_mmio_hold_acquire(&hold, &b, binding_id, &owner, &new_hold_id) && new_hold_id != hold_id);
    CHECK(!polaris_mmio_hold_release(&hold, hold_id) && b.active_holds == 1U);
    CHECK(polaris_mmio_hold_release(&hold, new_hold_id));
    CHECK(polaris_mmio_close(&b) && !polaris_mmio_binding_prepared(&b));
    CHECK(!polaris_mmio_hold_acquire(&hold, &b, binding_id, &owner, &rejected_id));
    CHECK(polaris_mmio_cleanup(&b) && empty());
    CHECK(reset());
    CHECK(polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U));
    binding_id = polaris_mmio_binding_identity(&b);
    PolarisMmioHold held[POLARIS_MMIO_HOLD_CAPACITY];
    u64 held_ids[POLARIS_MMIO_HOLD_CAPACITY];
    for (u32 i = 0; i < POLARIS_MMIO_HOLD_CAPACITY; ++i)
        CHECK(polaris_mmio_hold_acquire(&held[i], &b, binding_id, &owner, &held_ids[i]));
    CHECK(!polaris_mmio_hold_acquire(&hold, &b, binding_id, &owner, &rejected_id) && !rejected_id &&
        polaris_mmio_hold_count() == POLARIS_MMIO_HOLD_CAPACITY);
    CHECK(!polaris_mmio_cleanup(&b) && !unmap_calls);
    for (u32 i = 0; i < POLARIS_MMIO_HOLD_CAPACITY; ++i)
        CHECK(polaris_mmio_hold_release(&held[i], held_ids[i]));
    CHECK(polaris_mmio_cleanup(&b) && empty());
    for (u32 failure = 1U; failure <= 4U; ++failure) {
        CHECK(reset()); fail_map = failure;
        CHECK(!polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U));
        CHECK(map_calls == failure && unmap_calls == failure - 1U && empty());
    }
    CHECK(reset()); fail_map = 4U; fail_unmap = 2U;
    CHECK(!polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U));
    CHECK(polaris_mmio_binding_valid(&b) && !polaris_mmio_binding_prepared(&b) &&
        b.mapped_pages == 2U && assignment.active_access_count == 1U && assignment.child_resource_count == 1U);
    fail_unmap = 0; exited = true; assignment.state = PCI_ASSIGNMENT_QUARANTINED;
    CHECK(polaris_mmio_cleanup(&b) && empty());
    CHECK(reset()); change_after_map = true;
    CHECK(!polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U) && empty());
    for (u32 stage = 0; stage < 2U; ++stage) {
        CHECK(reset()); fail_child = stage == 0; fail_access = stage == 1;
        CHECK(!polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U) && empty() && !map_calls);
    }
    for (u32 stage = 0; stage < 2U; ++stage) {
        CHECK(reset());
        CHECK(polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U));
        fail_access_release = stage == 0; fail_child_release = stage == 1;
        CHECK(!polaris_mmio_cleanup(&b) && polaris_mmio_binding_valid(&b) && !b.mapped_pages);
        CHECK(assignment.child_resource_count == 1U && assignment.active_access_count == (stage == 0 ? 1U : 0U));
        fail_access_release = fail_child_release = false;
        CHECK(polaris_mmio_cleanup(&b) && empty());
    }
    for (u32 failure = 1U; failure <= 4U; ++failure) {
        CHECK(reset());
        CHECK(polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U));
        fail_unmap = failure;
        CHECK(!polaris_mmio_cleanup(&b) && b.mapped_pages == 5U - failure &&
            assignment.active_access_count == 1U && assignment.child_resource_count == 1U);
        fail_unmap = 0;
        CHECK(polaris_mmio_cleanup(&b) && empty());
    }
    CHECK(reset());
    CHECK(polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U));
    assignment.resource.id++;
    CHECK(!polaris_mmio_binding_prepared(&b) && !polaris_mmio_cleanup(&b) && !unmap_calls);
    assignment.resource.id--;
    wrong_context = true;
    CHECK(!polaris_mmio_cleanup(&b) && !unmap_calls);
    wrong_context = false;
    leaves[3].flags = VM_DEVICE | VM_WRITE;
    CHECK(!polaris_mmio_cleanup(&b) && !unmap_calls);
    leaves[3].flags = VM_DEVICE; leaves[3].frame++;
    CHECK(!polaris_mmio_cleanup(&b) && !unmap_calls);
    leaves[3].frame--;
    CHECK(polaris_mmio_cleanup(&b) && empty());
    for (u32 which = 0; which < 4U; ++which) {
        CHECK(reset());
        if (which == 0) alias = 0x63406000ULL;
        if (which == 1) alias = PHYS_MAP_BASE + 0x63406000ULL;
        if (which == 2) alias = POLARIS_MMIO_WINDOW_BASE;
        if (which == 3) memory.type = 7U; /* Firmware RAM conflict. */
        CHECK(!polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U) && empty() && !map_calls);
    }
    CHECK(reset()); supported = false;
    CHECK(!polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U) && !config_reads && empty());
    CHECK(reset()); config[4] |= PCI_COMMAND_BUS_MASTER; fail_map = 2U;
    CHECK(!polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U) && empty());
    CHECK(assignment.bus_master_exposed); /* Cleanup cannot clear observed DMA history. */
    CHECK(reset()); config[4] |= PCI_COMMAND_BUS_MASTER;
    boot.framebuffer_bar_queries[3].prefix[3] = 0x41U;
    CHECK(!polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U) && empty() &&
        assignment.bus_master_exposed && !map_calls);
    CHECK(reset()); config[4] |= PCI_COMMAND_BUS_MASTER; config[0x50] = 0xFFU;
    CHECK(!polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U) && empty() &&
        assignment.bus_master_exposed && !map_calls);
    CHECK(reset()); config[0x52] = 0U;
    CHECK(!polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U) && empty() && !map_calls);
    CHECK(reset()); assignment.dma_gate_active = true;
    CHECK(!polaris_mmio_prepare(&b, &boot, &assignment, &owner, 17U) && empty() && !config_reads);
    puts("PASS: complete BAR5/D0 plan; all map-failure boundaries; retained rollback/release pins;"
         " owner fault/quarantine; stale identity/context/leaf; alias/RAM refusal; inherited DMA history; bounded holds/close/drain/incarnation; guarded semantic load routes and refusals.");
    return 0;
}
