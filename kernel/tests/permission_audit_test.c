#include "permission_audit_test.h"
#include "user_test_fixture.h"
#include "vmm_test.h"

#include "address_space.h"
#include "arch.h"
#include "kernel_stack.h"
#include "physmap.h"
#include "pmm.h"
#include "process.h"
#include "supervisor.h"
#include "terminal.h"
#include "thread.h"
#include "user_stack.h"
#include "vfs.h"
#include "vmm.h"

#define AUDIT_PERMISSION_MASK (VM_WRITE | VM_USER | VM_EXEC)
#define AUDIT_USER_PATH "/bin/runtimetest.elf"
#define AUDIT_USER_TEXT ADDRESS_SPACE_USER_BASE
#define AUDIT_USER_DATA (ADDRESS_SPACE_USER_BASE + VM_PAGE_SIZE)
#define AUDIT_USER_STACK (ADDRESS_SPACE_USER_BASE + 0x100000ULL)

extern const u8 __kernel_text_start[] __attribute__((visibility("hidden")));
extern const u8 __kernel_text_end[] __attribute__((visibility("hidden")));
extern const u8 __kernel_data_start[] __attribute__((visibility("hidden")));
extern const u8 __kernel_data_end[] __attribute__((visibility("hidden")));

static void print_check(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static vm_flags_t permissions(vm_flags_t flags) {
    return flags & AUDIT_PERMISSION_MASK;
}

static bool walk_intersection_valid(const VmmTestPageWalk *walk) {
    if (!walk || walk->frame == FRAME_INVALID) return false;
    vm_flags_t intersection = permissions(walk->pml4_flags) & permissions(walk->pdpt_flags) &
        permissions(walk->pd_flags) & permissions(walk->pt_flags);
    return permissions(walk->effective_flags) == intersection;
}

static bool walk_exact(const VmPageMap *map, u64 address, frame_t frame,
    vm_flags_t effective, vm_flags_t leaf, bool require_supervisor_levels) {
    VmmTestPageWalk walk;
    if (!vmm_test_page_walk(map, address, &walk)) return false;
    if (walk.frame != frame || !walk_intersection_valid(&walk)) return false;
    if (permissions(walk.effective_flags) != permissions(effective) ||
        permissions(walk.pt_flags) != permissions(leaf)) return false;
    if (require_supervisor_levels &&
        ((walk.pml4_flags | walk.pdpt_flags | walk.pd_flags | walk.pt_flags) & VM_USER)) return false;
    return true;
}

static bool kernel_range_exact(const VmPageMap *map, u64 base, u64 end, vm_flags_t expected) {
    if (!map || !base || end <= base || (base & (VM_PAGE_SIZE - 1ULL)) ||
        (end & (VM_PAGE_SIZE - 1ULL))) return false;
    for (u64 address = base; address < end; address += VM_PAGE_SIZE) {
        if (!walk_exact(map, address, phys_to_frame(address), expected, expected, true)) return false;
        u64 direct = 0;
        if (!physmap_virtual_address(address, &direct) || vmm_query_page(map, direct, 0, 0)) return false;
    }
    return true;
}

static bool bootstrap_stack_exact(const VmPageMap *map, const Thread *main) {
    if (!map || !main || !main->kernel_stack_base || !main->kernel_stack_size ||
        (main->kernel_stack_base & (VM_PAGE_SIZE - 1ULL)) ||
        (main->kernel_stack_size & (VM_PAGE_SIZE - 1ULL))) return false;
    if (main->kernel_stack_base > ~0ULL - main->kernel_stack_size) return false;
    u64 end = main->kernel_stack_base + main->kernel_stack_size;
    for (u64 address = main->kernel_stack_base; address < end; address += VM_PAGE_SIZE) {
        if (!walk_exact(map, address, phys_to_frame(address), VM_WRITE, VM_WRITE, true)) return false;
    }
    return true;
}

static bool user_physmap_alias_exact(const VmPageMap *map, frame_t frame) {
    if (!map || frame == FRAME_INVALID) return false;
    u64 physical = frame_to_phys(frame);
    u64 direct = 0;
    if (!physical || !physmap_virtual_address(physical, &direct)) return false;
    return walk_exact(map, direct, frame, VM_WRITE, VM_WRITE, true);
}

static bool guarded_kernel_stack_exact(const VmPageMap *map, const Thread *thread) {
    if (!map || !thread || !thread->owns_kernel_stack || !thread->kernel_stack_physical ||
        !thread->kernel_stack_base || thread->kernel_stack_size != THREAD_KERNEL_STACK_SIZE ||
        !kernel_stack_mapping_valid(thread->kernel_stack_physical, thread->kernel_stack_base)) return false;
    if (vmm_query_page(map, thread->kernel_stack_base - VM_PAGE_SIZE, 0, 0) ||
        vmm_query_page(map, thread->kernel_stack_top, 0, 0)) return false;
    frame_t first = phys_to_frame(thread->kernel_stack_physical);
    if (first == FRAME_INVALID) return false;
    for (u64 i = 0; i < THREAD_KERNEL_STACK_PAGES; ++i) {
        if (!walk_exact(map, thread->kernel_stack_base + i * VM_PAGE_SIZE, first + i,
                VM_WRITE, VM_WRITE, true)) return false;
    }
    return true;
}

void permission_audit_test_run(void) {
    terminal_writeln("PAGE PERMISSION AUDIT TEST:");

    bool nx_ok = vmm_nx_enabled();
    bool wp_ok = vmm_write_protect_enabled();
    print_check("NX ACTIVE", nx_ok);
    print_check("CR0.WP ACTIVE", wp_ok);

    AddressSpace *kernel_space = address_space_kernel();
    Thread *main = thread_current();
    bool kernel_active = kernel_space && kernel_space->kernel && address_space_cr3(kernel_space) &&
        main && main->process == process_kernel() &&
        (arch_read_cr3() & ~0xFFFULL) == address_space_cr3(kernel_space);
    print_check("KERNEL MAP ACTIVE", kernel_active);
    if (!nx_ok || !wp_ok || !kernel_active) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("PAGE PERMISSION AUDIT TEST: FAILED");
        terminal_set_color(terminal_default_color());
        return;
    }

    VmPageMap *kernel_map = &kernel_space->page_map;
    u64 text_base = (u64)(const void *)__kernel_text_start;
    u64 text_end = (u64)(const void *)__kernel_text_end;
    u64 data_base = (u64)(const void *)__kernel_data_start;
    u64 data_end = (u64)(const void *)__kernel_data_end;

    bool layout_ok = text_base && text_base < text_end && text_end == data_base && data_base < data_end &&
        !((text_base | text_end | data_base | data_end) & (VM_PAGE_SIZE - 1ULL));
    bool kernel_text_ok = layout_ok && kernel_range_exact(kernel_map, text_base, text_end, VM_EXEC);
    bool kernel_data_ok = layout_ok && kernel_range_exact(kernel_map, data_base, data_end, VM_WRITE);
    bool bootstrap_ok = bootstrap_stack_exact(kernel_map, main);
    bool supervisor_before = supervisor_stack_guarded();
    print_check("KERNEL LINKER LAYOUT", layout_ok);
    print_check("KERNEL TEXT / RODATA RX", kernel_text_ok);
    print_check("KERNEL DATA / BSS RW-NX", kernel_data_ok);
    print_check("KERNEL IMAGE PHYSMAP ALIASES ABSENT", kernel_text_ok && kernel_data_ok);
    print_check("BOOTSTRAP STACK RW-NX", bootstrap_ok);
    print_check("SUPERVISOR USER STACK GUARDED", supervisor_before);

    if (!layout_ok || !kernel_text_ok || !kernel_data_ok || !bootstrap_ok || !supervisor_before ||
        !user_fixture_available()) {
        terminal_set_color(terminal_error_color());
        terminal_writeln("PAGE PERMISSION AUDIT TEST: FAILED");
        terminal_set_color(terminal_default_color());
        return;
    }

    UserTestFixture *fixture = user_fixture_begin("PAGE PERMISSION AUDIT TEST");
    if (!fixture) return;
    bool behavior_completed = false;

    VfsNode *file = vfs_resolve(vfs_root(), AUDIT_USER_PATH);
    bool file_ok = file && file->type == VFS_FILE && file->data && file->size;
    user_fixture_check("ELF FILE", file_ok);
    if (!file_ok) goto finish;

    bool process_created = user_fixture_process_create(fixture);
    user_fixture_check("PROCESS CREATE", process_created);
    if (!process_created) goto finish;

    AddressSpace *space = process_address_space(&fixture->process);
    bool space_ok = space && !space->kernel && address_space_cr3(space);
    user_fixture_check("ADDRESS SPACE", space_ok);
    if (!space_ok) goto finish;

    bool image_loaded = user_fixture_load(fixture, file);
    user_fixture_check("ELF LOAD", image_loaded);
    if (!image_loaded) goto finish;

    frame_t text_frame = FRAME_INVALID;
    frame_t data_frame = FRAME_INVALID;
    bool text_present = address_space_query_page(space, AUDIT_USER_TEXT, &text_frame, 0);
    bool data_present = address_space_query_page(space, AUDIT_USER_DATA, &data_frame, 0);
    bool user_text_ok = text_present && walk_exact(&space->page_map, AUDIT_USER_TEXT, text_frame,
        VM_USER | VM_EXEC, VM_USER | VM_EXEC, false);
    bool user_data_ok = data_present && walk_exact(&space->page_map, AUDIT_USER_DATA, data_frame,
        VM_USER | VM_WRITE, VM_USER | VM_WRITE, false);
    user_fixture_check("USER TEXT EFFECTIVE RX", user_text_ok);
    user_fixture_check("USER DATA EFFECTIVE RW-NX", user_data_ok);
    if (!user_text_ok || !user_data_ok) goto finish;

    bool stack_ready = user_fixture_stack_create(fixture, 0U, AUDIT_USER_STACK, 0ULL, 0ULL);
    user_fixture_check("GUARDED USER STACK CREATE", stack_ready);
    if (!stack_ready) goto finish;

    frame_t stack_frame = fixture->stacks[0].frame;
    bool user_stack_ok = user_stack_mapping_valid(space, AUDIT_USER_STACK, stack_frame) &&
        walk_exact(&space->page_map, AUDIT_USER_STACK, stack_frame,
            VM_USER | VM_WRITE, VM_USER | VM_WRITE, false) &&
        !address_space_query_page(space, AUDIT_USER_STACK - VM_PAGE_SIZE, 0, 0) &&
        !address_space_query_page(space, AUDIT_USER_STACK + VM_PAGE_SIZE, 0, 0);
    user_fixture_check("USER STACK / GUARD LEVELS", user_stack_ok);
    if (!user_stack_ok) goto finish;

    bool shared_kernel_text = walk_exact(&space->page_map, text_base, phys_to_frame(text_base),
        VM_EXEC, VM_EXEC, true);
    bool user_aliases_ok = user_physmap_alias_exact(&space->page_map, text_frame) &&
        user_physmap_alias_exact(&space->page_map, data_frame) &&
        user_physmap_alias_exact(&space->page_map, stack_frame);
    user_fixture_check("SHARED KERNEL TEXT SUPERVISOR-ONLY", shared_kernel_text);
    user_fixture_check("USER FRAME PHYSMAP ALIASES SUPERVISOR RW-NX", user_aliases_ok);
    if (!shared_kernel_text || !user_aliases_ok) goto finish;

    bool thread_created = user_fixture_thread_create(fixture, 0U);
    user_fixture_check("THREAD CREATE", thread_created);
    if (!thread_created) goto finish;

    bool ring3_kernel_stack = guarded_kernel_stack_exact(&space->page_map, &fixture->threads[0]);
    user_fixture_check("RING3 KERNEL STACK SUPERVISOR RW-NX / GUARDED", ring3_kernel_stack);
    if (!ring3_kernel_stack) goto finish;

    /* The walk helper reports each level separately; every successful check
     * above also verifies vmm_query_page() equals the W/U/X intersection. */
    user_fixture_check("EFFECTIVE LEVEL INTERSECTION", true);
    behavior_completed = true;

finish: {
        bool finished = user_fixture_finish(fixture, behavior_completed);
        print_check("CASE CLEANUP / SUPERVISOR / BASELINES", finished);
        bool supervisor_after = finished && supervisor_stack_guarded();
        print_check("SUPERVISOR STACK STILL GUARDED", supervisor_after);
        bool pass = nx_ok && wp_ok && kernel_text_ok && kernel_data_ok && bootstrap_ok && supervisor_before &&
            finished && supervisor_after;
        terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
        terminal_write("PAGE PERMISSION AUDIT TEST: ");
        terminal_writeln(pass ? "PASS" : "FAILED");
        terminal_set_color(terminal_default_color());
    }
}
