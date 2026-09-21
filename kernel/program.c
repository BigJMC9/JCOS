#include "program.h"

#include "arch.h"
#include "interrupts.h"
#include "lib.h"
#include "physmap.h"
#include "pmm.h"
#include "process_exit_queue.h"
#include "scheduler.h"
#include "task.h"
#include "user_stack.h"
#include "vmm.h"

#define PROGRAM_RFLAGS_IF (1ULL << 9)

static u64 g_program_launch_count;

static u64 program_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void program_irq_restore(u64 flags) {
    if (flags & PROGRAM_RFLAGS_IF) interrupts_enable();
}

static bool program_spec_valid(const ProgramLaunchSpec *spec) {
    if (!spec || !spec->file || spec->file->type != VFS_FILE ||
        !spec->file->data || !spec->file->size ||
        spec->startup_grant_count > JCOS_PROGRAM_STARTUP_MAX_CAPABILITIES ||
        spec->startup_argument_count > JCOS_PROGRAM_STARTUP_MAX_ARGUMENTS ||
        (spec->exit_queue && !process_exit_queue_storage_in_use(spec->exit_queue))) return false;
    if ((spec->readonly_data || spec->readonly_size || spec->readonly_virtual_base) &&
        (!spec->readonly_data || !spec->readonly_size ||
         (spec->readonly_virtual_base & (VM_PAGE_SIZE - 1ULL)) ||
         spec->readonly_virtual_base < ADDRESS_SPACE_USER_BASE ||
         spec->readonly_virtual_base >= ADDRESS_SPACE_USER_LIMIT)) return false;

    for (u32 i = 0; i < spec->startup_grant_count; ++i) {
        const ProgramGrantSpec *grant = &spec->startup_grants[i];
        if (!grant->authority_table || grant->authority_handle == CAPABILITY_INVALID_HANDLE ||
            !grant->object || grant->type == CAPABILITY_TYPE_NONE || !grant->rights ||
            (grant->rights & ~CAPABILITY_RIGHT_ALL)) return false;

        void *authorized = 0;
        CapabilityRights required = grant->rights | CAPABILITY_RIGHT_TRANSFER;
        if (!capability_lookup_rights(grant->authority_table, grant->authority_handle,
                grant->type, required, &authorized) || authorized != grant->object) return false;
    }
    return true;
}

bool program_instance_needs_cleanup(const ProgramInstance *instance) {
    if (!instance) return false;
    if (instance->process_created || instance->stack_frame_allocated || instance->stack_mapped ||
        instance->thread_created || instance->published || instance->unpublished_space ||
        instance->unpublished_stack || instance->unlinked_table || user_elf_needs_cleanup(&instance->image) ||
        instance->readonly_page_count || instance->process.id ||
        instance->process.initialized || instance->thread.id) return true;
    for (u32 i = 0; i < JCOS_PROGRAM_STARTUP_MAX_CAPABILITIES; ++i) {
        if (instance->startup_handles[i] != CAPABILITY_INVALID_HANDLE) return true;
    }
    return false;
}

static bool program_threads_dead(const ProgramInstance *instance) {
    if (!instance || !instance->process_created) return false;
    u64 count = process_thread_count(&instance->process);
    Thread *thread = process_thread_first(&instance->process);
    for (u64 i = 0; i < count; ++i) {
        if (!thread || thread->process != &instance->process || thread->state != THREAD_STATE_DEAD ||
            thread->on_run_queue || thread_wait_active(thread)) return false;
        thread = thread->process_next;
    }
    return thread == 0;
}

static bool program_release(ProgramInstance *instance, bool force) {
    if (!instance) return false;
    if (!program_instance_needs_cleanup(instance)) return true;

    /* These allocations remain module-owned, but this launch owes their retry.
     * Reclaim them even when no Process was successfully constructed. */
    if (instance->unpublished_stack) {
        if (!thread_reclaim_unpublished_stack()) return false;
        instance->unpublished_stack = false;
    }
    if (instance->unpublished_space) {
        if (!address_space_reclaim_unpublished()) return false;
        instance->unpublished_space = false;
    }
    if (instance->unlinked_table) {
        if (!vmm_reclaim_unlinked_table()) return false;
        instance->unlinked_table = false;
    }
    if (!instance->process_created) {
        if (program_instance_needs_cleanup(instance)) return false;
        k_memset(instance, 0, sizeof(*instance));
        return true;
    }

    if (!force && !program_threads_dead(instance)) return false;
    if (!task_quiesce_process(&instance->process)) return false;

    if (process_thread_count(&instance->process) || instance->process.thread_head ||
        instance->process.thread_tail) return false;
    CapabilityTable *caps = process_capabilities(&instance->process);
    if (!caps || !capability_table_empty(caps)) return false;

    instance->thread_created = false;
    instance->published = false;
    for (u32 i = 0; i < JCOS_PROGRAM_STARTUP_MAX_CAPABILITIES; ++i) {
        instance->startup_handles[i] = CAPABILITY_INVALID_HANDLE;
    }

    AddressSpace *space = process_address_space(&instance->process);
    while (instance->readonly_page_count) {
        u32 index = instance->readonly_page_count - 1U;
        u64 virtual_address = instance->readonly_virtual_base + (u64)index * VM_PAGE_SIZE;
        frame_t expected = instance->readonly_first_frame + index;
        frame_t mapped = FRAME_INVALID;
        frame_t old = FRAME_INVALID;
        if (!space || !address_space_query_page(space, virtual_address, &mapped, 0) ||
            mapped != expected || !address_space_unmap_page(space, virtual_address, &old) ||
            old != expected) return false;
        instance->readonly_page_count = index;
    }
    if (instance->stack_mapped) {
        frame_t mapped = FRAME_INVALID;
        frame_t old = FRAME_INVALID;
        if (!space || !user_stack_mapping_valid(space, USER_STACK_INITIAL_BASE, instance->stack_frame)) return false;
        if (!address_space_query_page(space, USER_STACK_INITIAL_BASE, &mapped, 0) ||
            mapped != instance->stack_frame) return false;
        if (!address_space_unmap_page(space, USER_STACK_INITIAL_BASE, &old)) return false;
        instance->stack_mapped = false;
        if (old != instance->stack_frame) return false;
    }

    if (instance->stack_frame_allocated) {
        if (!frame_free(instance->stack_frame)) return false;
        instance->stack_frame_allocated = false;
        instance->stack_frame = FRAME_INVALID;
    }

    if (user_elf_needs_cleanup(&instance->image) &&
        !user_elf_unload(&instance->process, &instance->image)) return false;

    if (!process_destroy(&instance->process)) return false;
    instance->process_created = false;

    k_memset(instance, 0, sizeof(*instance));
    return true;
}

bool program_reap(ProgramInstance *instance) {
    u64 flags = program_irq_save();
    bool released = program_release(instance, false);
    program_irq_restore(flags);
    return released;
}

bool program_terminate(ProgramInstance *instance) {
    u64 flags = program_irq_save();
    bool released = program_release(instance, true);
    program_irq_restore(flags);
    return released;
}

u64 program_launch_count(void) {
    return g_program_launch_count;
}

static bool program_launch_locked(ProgramInstance *instance, const ProgramLaunchSpec *spec) {
    if (!instance || program_instance_needs_cleanup(instance) || !program_spec_valid(spec)) return false;
    /* Do not adopt another caller's retained constructor allocation. */
    if (address_space_creation_cleanup_pending() || thread_creation_cleanup_pending() ||
        vmm_unlinked_table_cleanup_pending()) return false;

    k_memset(instance, 0, sizeof(*instance));
    instance->stack_frame = FRAME_INVALID;

    if (!process_create(&instance->process)) goto fail;
    instance->process_created = true;

    if (!user_elf_load(&instance->process, spec->file, &instance->image)) goto fail;

    AddressSpace *space = process_address_space(&instance->process);
    CapabilityTable *caps = process_capabilities(&instance->process);
    if (!space || !caps) goto fail;

    if (spec->readonly_size) {
        u64 physical = 0;
        if (!virt_to_phys(spec->readonly_data, &physical))
            physical = (u64)spec->readonly_data;
        u64 page_offset = physical & (VM_PAGE_SIZE - 1ULL);
        u64 first_physical = physical - page_offset;
        if (spec->readonly_size > ~0ULL - page_offset) goto fail;
        u64 span = spec->readonly_size + page_offset;
        if (span > ~0ULL - (VM_PAGE_SIZE - 1ULL)) goto fail;
        u64 page_count = (span + VM_PAGE_SIZE - 1ULL) / VM_PAGE_SIZE;
        if (!page_count || page_count > 0xFFFFFFFFULL ||
            page_count > (ADDRESS_SPACE_USER_LIMIT - spec->readonly_virtual_base) / VM_PAGE_SIZE)
            goto fail;
        frame_t first_frame = phys_to_frame(first_physical);
        if (first_frame == FRAME_INVALID) goto fail;
        instance->readonly_virtual_base = spec->readonly_virtual_base;
        instance->readonly_first_frame = first_frame;
        for (u32 i = 0; i < (u32)page_count; ++i) {
            if (!address_space_map_page(space,
                    spec->readonly_virtual_base + (u64)i * VM_PAGE_SIZE,
                    first_frame + i, 0)) goto fail;
            ++instance->readonly_page_count;
        }
    }

    instance->stack_frame = frame_alloc();
    if (instance->stack_frame == FRAME_INVALID) goto fail;
    instance->stack_frame_allocated = true;

    if (!user_stack_map_page(space, USER_STACK_INITIAL_BASE, instance->stack_frame)) goto fail;
    instance->stack_mapped = true;

    for (u32 i = 0; i < spec->startup_grant_count; ++i) {
        const ProgramGrantSpec *grant = &spec->startup_grants[i];
        if (!capability_insert(caps, grant->object, grant->type, grant->rights,
                &instance->startup_handles[i])) goto fail;
    }

    u8 *stack = (u8 *)phys_to_virt(frame_to_phys(instance->stack_frame));
    if (!stack) goto fail;
    k_memset(stack, 0, (usize)VM_PAGE_SIZE);

    u64 initial_rsp = USER_STACK_INITIAL_BASE + VM_PAGE_SIZE - sizeof(JcosProgramStartup);
    if (initial_rsp & 0xFULL) goto fail;
    u64 offset = initial_rsp - USER_STACK_INITIAL_BASE;
    JcosProgramStartup *startup = (JcosProgramStartup *)(void *)(stack + offset);
    startup->magic = JCOS_PROGRAM_STARTUP_MAGIC;
    startup->version = JCOS_PROGRAM_STARTUP_VERSION;
    startup->size = (u32)sizeof(*startup);
    startup->flags = JCOS_PROGRAM_STARTUP_FLAG_NONE;
    startup->capability_count = spec->startup_grant_count;
    startup->argument_count = spec->startup_argument_count;
    startup->environment_count = 0U;
    for (u32 i = 0; i < spec->startup_grant_count; ++i) {
        startup->capabilities[i] = instance->startup_handles[i];
    }
    for (u32 i = 0; i < spec->startup_argument_count; ++i) {
        startup->arguments[i] = spec->startup_arguments[i];
    }

    if (!thread_create(&instance->thread, &instance->process)) goto fail;
    instance->thread_created = true;

    if (!thread_prepare_user(&instance->thread, instance->image.entry, initial_rsp)) goto fail;

    /* Reserve terminal-event storage before execution becomes observable. This
     * is the final fallible ownership step; IRQ exclusion closes the gap between
     * watch registration and scheduler publication. */
    if (spec->exit_queue && !process_exit_queue_watch(spec->exit_queue, &instance->process)) goto fail;

    /* Publication commit, still under the transaction's IRQ exclusion.
     * No fallible ownership operation follows a successful add. */
    bool published = scheduler_add(&instance->thread);
    if (published) {
        instance->published = true;
        if (g_program_launch_count != ~0ULL) ++g_program_launch_count;
    }
    if (!published) goto fail;

    return true;

fail:
    /* A launch that never published is not a process-exit event. If the final
     * scheduler publication fails after watch reservation, detach it before
     * force-quiesce can classify the unpublished thread as TERMINATED. */
    if (!instance->published && instance->process_created &&
        (instance->process.exit_queue || instance->process.exit_queue_id) &&
        !process_exit_queue_unwatch_process(&instance->process)) cpu_halt_forever();

    /* Admission required empty slots and IRQ exclusion prevents an unrelated
     * constructor from intervening before we attribute retained ownership. */
    instance->unpublished_space = address_space_creation_cleanup_pending();
    instance->unpublished_stack = thread_creation_cleanup_pending();
    instance->unlinked_table = vmm_unlinked_table_cleanup_pending();
    (void)program_release(instance, true);
    return false;
}

bool program_launch(ProgramInstance *instance, const ProgramLaunchSpec *spec) {
    /* UP-only transaction: no waits or callbacks; preserve the caller's IF.
     * ELF validation bounds page work before walking candidate mappings. */
    u64 flags = program_irq_save();
    bool launched = program_launch_locked(instance, spec);
    program_irq_restore(flags);
    return launched;
}
