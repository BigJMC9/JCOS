/* Production page-table code, hosted page-table RAM and CPU register doubles.
 * Maps a fabricated MMIO frame number; never dereferences that leaf. */
#include <stdio.h>
#include <string.h>
#include "../../kernel/vmm.h"
#include "../../kernel/tests/vmm_test.h"
#include "../../kernel/arch.h"
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "device-vmm line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static _Alignas(4096) u64 pool[16][512];
static bool used[16], pat_supported = true;
static u64 pat = 0x0007040600070406ULL, cr0 = 1ULL << 16, cr3, cr4, efer;
void interrupts_disable(void) {}
void interrupts_enable(void) {}
u64 frame_to_phys(frame_t f) { return f * 4096ULL; }
frame_t phys_to_frame(u64 p) { return p / 4096ULL; }
frame_t frame_alloc(void) {
    for (u32 i = 0; i < 16U; ++i) if (!used[i]) { used[i] = true; return phys_to_frame((u64)pool[i]); }
    return FRAME_INVALID;
}
bool frame_free(frame_t f) {
    for (u32 i = 0; i < 16U; ++i) if (f == phys_to_frame((u64)pool[i]) && used[i]) {
        used[i] = false; return true;
    }
    return false;
}
void *phys_to_virt(u64 p) { return (void *)p; }
u64 arch_read_cr0(void) { return cr0; }
void arch_write_cr0(u64 value) { cr0 = value; }
u64 arch_read_cr3(void) { return cr3; }
void arch_write_cr3(u64 value) { cr3 = value; }
u64 arch_read_cr4(void) { return cr4; }
void arch_cpuid(u32 leaf, u32 sub, u32 *a, u32 *b, u32 *c, u32 *d) {
    (void)sub; *a = *b = *c = *d = 0;
    if (leaf == 0x80000000U) *a = 0x80000001U;
    if (leaf == 0x80000001U) *d = 1U << 20;
    if (leaf == 1U && pat_supported) *d = 1U << 16;
}
u64 arch_read_msr(u32 msr) { return msr == 0x277U ? pat : efer; }
void arch_write_msr(u32 msr, u64 value) { if (msr != 0x277U) efer = value; }
static u64 *leaf_entry(VmPageMap *map, u64 address) {
    u64 *t = (u64 *)frame_to_phys(map->root_frame);
    for (u32 shift = 39U; shift > 12U; shift -= 9U)
        t = (u64 *)(t[(address >> shift) & 511U] & 0x000FFFFFFFFFF000ULL);
    return &t[(address >> 12) & 511U];
}
int main(void) {
    CHECK(!vmm_device_memory_supported() && vmm_enable_nx() && vmm_device_memory_supported());
    VmPageMap map;
    CHECK(vmm_page_map_create(&map));
    u64 address = 0x6000000000ULL;
    frame_t mmio = phys_to_frame(0x63406000ULL), actual = FRAME_INVALID;
    vm_flags_t flags = 0;
    for (u32 failure = 0; failure < 6U; ++failure) {
        u64 saved_pat = pat, saved_cr0 = cr0;
        if (failure == 0) pat = (pat & ~(0xFFULL << 24)) | (6ULL << 24);
        if (failure == 1) pat_supported = false;
        if (failure == 2) cr0 &= ~(1ULL << 16);
        if (failure == 3) cr0 |= 1ULL << 30;
        if (failure == 5) cr0 |= 1ULL << 29;
        if (failure == 4) cr4 |= 1ULL << 17;
        CHECK(!vmm_device_memory_supported());
        CHECK(!vmm_map_page(&map, address, mmio, VM_DEVICE));
        CHECK(!vmm_query_page(&map, address, 0, 0));
        pat = saved_pat; cr0 = saved_cr0; pat_supported = true; cr4 = 0;
    }
    CHECK(!vmm_map_page(&map, address, mmio, VM_DEVICE | VM_UNCACHED));
    CHECK(vmm_map_page(&map, address, mmio, VM_DEVICE));
    CHECK(vmm_query_page(&map, address, &actual, &flags) && actual == mmio && flags == VM_DEVICE);
    u64 pte = *leaf_entry(&map, address);
    CHECK((pte & (1ULL << 63)) && (pte & 0x18ULL) == 0x18ULL && !(pte & 0x86ULL));
    VmmTestPageWalk walk;
    CHECK(vmm_test_page_walk(&map, address, &walk) && walk.effective_flags == VM_DEVICE &&
        walk.pt_flags == VM_DEVICE);
    *leaf_entry(&map,address)|=(1ULL<<5)|(1ULL<<6);
    u64 before=*leaf_entry(&map,address);
    CHECK(!vmm_set_writable(&map,address,mmio+1,VM_DEVICE,true));
    CHECK(!vmm_set_writable(&map,address,mmio,VM_DEVICE|VM_USER,true));
    CHECK(vmm_set_writable(&map,address,mmio,VM_DEVICE,true));
    CHECK(*leaf_entry(&map,address)==(before|2ULL));
    CHECK(vmm_set_writable(&map,address,mmio,VM_DEVICE|VM_WRITE,false));
    CHECK(*leaf_entry(&map,address)==before);
    *leaf_entry(&map,address)|=1ULL<<8;
    CHECK(!vmm_set_writable(&map,address,mmio,VM_DEVICE,true));
    *leaf_entry(&map,address)=before;
    CHECK(!vmm_set_writable(&map,address+1,mmio,VM_DEVICE,true));
    cr4|=1ULL<<17;CHECK(!vmm_set_writable(&map,address,mmio,VM_DEVICE,true));cr4=0;
    CHECK(vmm_unmap_page(&map, address, &actual) && actual == mmio);
    CHECK(vmm_map_page(&map, address, mmio, VM_UNCACHED));
    CHECK(vmm_query_page(&map, address, &actual, &flags) && flags == VM_UNCACHED);
    CHECK((*leaf_entry(&map, address) & 0x18ULL) == 0x10ULL); /* Legacy unchanged. */
    CHECK(vmm_unmap_page(&map, address, &actual) && vmm_page_map_destroy(&map));
    for (u32 i = 0; i < 16U; ++i) CHECK(!used[i]);
    puts("PASS: production VMM strict UC PAT entry, RO/NX/supervisor PTE, CPU refusal cases,"
         " legacy cache flag preserved and complete table cleanup; no leaf access.");
    return 0;
}
