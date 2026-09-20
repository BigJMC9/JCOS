#include "program.h"

#include "interrupts.h"
#include "lib.h"
#include "physmap.h"
#include "pmm.h"
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
        spec->startup_argument_count > JCOS_PROGRAM_STARTUP_MAX_ARGUMENTS) return false;

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
        instance->thread_created || instance->published || user_elf_needs_cleanup(&instance->image) ||
        instance->process.id || instance->process.initialized || instance->thread.id) return true;
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
    if (!instance->process_created) return false;

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
    return program_release(instance, false);
}

bool program_terminate(ProgramInstance *instance) {
    return program_release(instance, true);
}

u64 program_launch_count(void) {
    return g_program_launch_count;
}

bool program_launch(ProgramInstance *instance, const ProgramLaunchSpec *spec) {
    if (!instance || program_instance_needs_cleanup(instance) || !program_spec_valid(spec)) return false;

    k_memset(instance, 0, sizeof(*instance));
    instance->stack_frame = FRAME_INVALID;

    if (!process_create(&instance->process)) goto fail;
    instance->process_created = true;

    if (!user_elf_load(&instance->process, spec->file, &instance->image)) goto fail;

    AddressSpace *space = process_address_space(&instance->process);
    CapabilityTable *caps = process_capabilities(&instance->process);
    if (!space || !caps) goto fail;

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

    /* Publication commit. Keep the run queue indivisible with respect to PIT
     * preemption; no fallible ownership operation follows a successful add. */
    u64 flags = program_irq_save();
    bool published = scheduler_add(&instance->thread);
    if (published) {
        instance->published = true;
        if (g_program_launch_count != ~0ULL) ++g_program_launch_count;
    }
    program_irq_restore(flags);
    if (!published) goto fail;

    return true;

fail:
    (void)program_release(instance, true);
    return false;
}
