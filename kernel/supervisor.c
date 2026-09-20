#include "supervisor.h"

#include "address_space.h"
#include "capability.h"
#include "endpoint.h"
#include "interrupts.h"
#include "ipc.h"
#include "lib.h"
#include "process.h"
#include "program.h"
#include "scheduler.h"
#include "thread.h"
#include "timer.h"
#include "vfs.h"
#include "user_stack.h"

#define SUPERVISOR_PATH "/bin/supervisor.elf"
#define SUPERVISOR_CALL_TIMEOUT_SECONDS 2ULL
#define SUPERVISOR_BOOT_START_MAX_TURNS 64U
#define SUPERVISOR_RFLAGS_IF (1ULL << 9)

typedef struct {
    ProgramInstance program;
    Endpoint command_endpoint;
    Endpoint reply_endpoint;
    CapabilityHandle kernel_send_handle;
    CapabilityHandle kernel_receive_handle;
    CapabilityHandle kernel_command_grant_handle;
    CapabilityHandle kernel_reply_grant_handle;
    bool command_created;
    bool reply_created;
    bool kernel_send_cap;
    bool kernel_receive_cap;
    bool kernel_command_grant_cap;
    bool kernel_reply_grant_cap;
    bool active;
} SupervisorState;

static SupervisorState g_supervisor;

static u64 supervisor_interrupt_save(void) {
    u64 flags = 0;

    __asm__ volatile (
        "pushfq\n\t"
        "popq %0"
        : "=r"(flags)
        :
        : "memory"
    );

    interrupts_disable();
    return flags;
}

static void supervisor_interrupt_restore(u64 flags) {
    if (flags & SUPERVISOR_RFLAGS_IF) interrupts_enable();
}

static bool supervisor_schedule_once(void) {
    u64 flags = supervisor_interrupt_save();
    bool result = scheduler_yield();

    supervisor_interrupt_restore(flags);
    return result;
}

static u64 supervisor_call_timeout_ticks(void) {
    if (!timer_initialized()) return 0;
    u32 frequency = timer_frequency();
    return frequency ? (u64)frequency * SUPERVISOR_CALL_TIMEOUT_SECONDS : 0;
}

static Thread *supervisor_thread(void) {
    return g_supervisor.program.thread_created ? &g_supervisor.program.thread : 0;
}

static bool supervisor_ready(void) {
    Thread *thread = supervisor_thread();
    return thread && thread->state == THREAD_STATE_BLOCKED &&
        !thread->on_run_queue && thread->interrupt_context_ready && thread->interrupt_rsp &&
        endpoint_receiver_waiting(&g_supervisor.command_endpoint);
}

static bool supervisor_wait_ready(void) {
    u64 timeout_ticks = supervisor_call_timeout_ticks();
    u64 started = timeout_ticks ? timer_ticks() : 0;
    u32 turns = 0;

    for (;;) {
        if (supervisor_ready()) return true;

        /* If main is executing on this UP kernel, the not-yet-ready service
         * can only be READY. DEAD/BLOCKED-without-receive is a failed start. */
        Thread *thread = supervisor_thread();
        if (!thread || thread->state != THREAD_STATE_READY || !thread->on_run_queue) return false;

        if (timeout_ticks) {
            if ((u64)(timer_ticks() - started) >= timeout_ticks) return false;
        } else {
            /* Early boot starts the supervisor before PIT initialization.
             * With no timer preemption one turn is normally sufficient, but
             * keep the fallback finite rather than relying on one exact turn. */
            if (turns >= SUPERVISOR_BOOT_START_MAX_TURNS) return false;
        }

        if (!supervisor_schedule_once()) return false;
        ++turns;
    }
}

static bool supervisor_thread_dead(void) {
    Thread *thread = supervisor_thread();
    return thread && thread->state == THREAD_STATE_DEAD &&
        !thread->on_run_queue && !thread->interrupt_context_ready && !thread->interrupt_rsp;
}

static bool supervisor_wait_thread_dead(u64 timeout_ticks) {
    if (!timeout_ticks || !timer_initialized()) return false;

    u64 started = timer_ticks();
    while (!supervisor_thread_dead()) {
        if ((u64)(timer_ticks() - started) >= timeout_ticks) return false;

        /* If the reply woke the caller before the supervisor executed its
         * THREAD_EXIT syscall, keep scheduling boundedly until that terminal
         * transition happens. READY is the only valid non-dead state here. */
        Thread *thread = supervisor_thread();
        if (!thread || thread->state != THREAD_STATE_READY || !thread->on_run_queue) return false;
        if (!supervisor_schedule_once()) return false;
    }
    return true;
}

static bool supervisor_needs_cleanup(void) {
    return program_instance_needs_cleanup(&g_supervisor.program) ||
        g_supervisor.command_created || g_supervisor.reply_created ||
        g_supervisor.kernel_send_cap || g_supervisor.kernel_receive_cap ||
        g_supervisor.kernel_command_grant_cap || g_supervisor.kernel_reply_grant_cap ||
        g_supervisor.kernel_send_handle != CAPABILITY_INVALID_HANDLE ||
        g_supervisor.kernel_receive_handle != CAPABILITY_INVALID_HANDLE ||
        g_supervisor.kernel_command_grant_handle != CAPABILITY_INVALID_HANDLE ||
        g_supervisor.kernel_reply_grant_handle != CAPABILITY_INVALID_HANDLE;
}

static bool supervisor_release(bool force_program) {
    if (program_instance_needs_cleanup(&g_supervisor.program)) {
        bool released = force_program ? program_terminate(&g_supervisor.program) : program_reap(&g_supervisor.program);
        if (!released) return false;
    }

    Process *kernel_process = process_kernel();
    CapabilityTable *kernel_caps = kernel_process ? process_capabilities(kernel_process) : 0;

    if (g_supervisor.kernel_send_cap) {
        if (!kernel_caps || !capability_revoke(kernel_caps, g_supervisor.kernel_send_handle)) return false;
        g_supervisor.kernel_send_cap = false;
    }
    if (g_supervisor.kernel_receive_cap) {
        if (!kernel_caps || !capability_revoke(kernel_caps, g_supervisor.kernel_receive_handle)) return false;
        g_supervisor.kernel_receive_cap = false;
    }
    if (g_supervisor.kernel_command_grant_cap) {
        if (!kernel_caps || !capability_revoke(kernel_caps, g_supervisor.kernel_command_grant_handle)) return false;
        g_supervisor.kernel_command_grant_cap = false;
    }
    if (g_supervisor.kernel_reply_grant_cap) {
        if (!kernel_caps || !capability_revoke(kernel_caps, g_supervisor.kernel_reply_grant_handle)) return false;
        g_supervisor.kernel_reply_grant_cap = false;
    }
    if (g_supervisor.command_created) {
        if (!endpoint_destroy(&g_supervisor.command_endpoint)) return false;
        g_supervisor.command_created = false;
    }
    if (g_supervisor.reply_created) {
        if (!endpoint_destroy(&g_supervisor.reply_endpoint)) return false;
        g_supervisor.reply_created = false;
    }

    k_memset(&g_supervisor, 0, sizeof(g_supervisor));
    return true;
}

bool supervisor_start(void) {
    if (g_supervisor.active) return false;

    if (!supervisor_release(true)) return false;

    Process *kernel_process = process_kernel();
    Thread *current = thread_current();

    if (!kernel_process || !current || current->process != kernel_process ||
        current->state != THREAD_STATE_RUNNING || !current->on_run_queue || scheduler_thread_count() != 1ULL) {
        return false;
    }

    CapabilityTable *kernel_caps = process_capabilities(kernel_process);
    if (!kernel_caps) return false;

    if (!endpoint_create(&g_supervisor.command_endpoint)) goto fail;
    g_supervisor.command_created = true;

    if (!endpoint_create(&g_supervisor.reply_endpoint)) goto fail;
    g_supervisor.reply_created = true;

    if (!capability_insert(kernel_caps, &g_supervisor.command_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND, &g_supervisor.kernel_send_handle)) goto fail;
    g_supervisor.kernel_send_cap = true;

    if (!capability_insert(kernel_caps, &g_supervisor.reply_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE, &g_supervisor.kernel_receive_handle)) goto fail;
    g_supervisor.kernel_receive_cap = true;

    if (!capability_insert(kernel_caps, &g_supervisor.command_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE | CAPABILITY_RIGHT_TRANSFER,
            &g_supervisor.kernel_command_grant_handle)) goto fail;
    g_supervisor.kernel_command_grant_cap = true;

    if (!capability_insert(kernel_caps, &g_supervisor.reply_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND | CAPABILITY_RIGHT_TRANSFER,
            &g_supervisor.kernel_reply_grant_handle)) goto fail;
    g_supervisor.kernel_reply_grant_cap = true;

    VfsNode *file = vfs_resolve(vfs_root(), SUPERVISOR_PATH);
    if (!file || file->type != VFS_FILE || !file->data || !file->size) goto fail;

    ProgramLaunchSpec spec;
    k_memset(&spec, 0, sizeof(spec));
    spec.file = file;
    spec.startup_grant_count = 2U;
    spec.startup_grants[0].authority_table = kernel_caps;
    spec.startup_grants[0].authority_handle = g_supervisor.kernel_command_grant_handle;
    spec.startup_grants[0].object = &g_supervisor.command_endpoint;
    spec.startup_grants[0].type = CAPABILITY_TYPE_ENDPOINT;
    spec.startup_grants[0].rights = CAPABILITY_RIGHT_RECEIVE;
    spec.startup_grants[1].authority_table = kernel_caps;
    spec.startup_grants[1].authority_handle = g_supervisor.kernel_reply_grant_handle;
    spec.startup_grants[1].object = &g_supervisor.reply_endpoint;
    spec.startup_grants[1].type = CAPABILITY_TYPE_ENDPOINT;
    spec.startup_grants[1].rights = CAPABILITY_RIGHT_SEND;

    if (!program_launch(&g_supervisor.program, &spec)) goto fail;

    g_supervisor.active = true;
    if (supervisor_wait_ready()) return true;

    g_supervisor.active = false;
    (void)supervisor_release(true);
    return false;

fail:
    (void)supervisor_release(true);
    return false;
}

bool supervisor_ping(u64 cookie, u64 *out_cookie) {
    if (!g_supervisor.active || !out_cookie) return false;

    Process *kernel_process = process_kernel();
    Thread *thread = supervisor_thread();

    if (!kernel_process || !thread) return false;
    if (thread->state != THREAD_STATE_BLOCKED) return false;
    if (!endpoint_receiver_waiting(&g_supervisor.command_endpoint)) return false;

    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 2U;
    request.words[0] = SUPERVISOR_MESSAGE_PING;
    request.words[1] = cookie;

    if (!ipc_try_send(kernel_process, g_supervisor.kernel_send_handle, &request)) return false;

    IpcMessage reply;
    k_memset(&reply, 0, sizeof(reply));

    /* Do not assume one scheduler turn is enough. The caller blocks until the
     * reply actually arrives, peer death/cancellation completes the wait, or a
     * bounded PIT-backed timeout expires. */
    u64 timeout = supervisor_call_timeout_ticks();
    if (!timeout || !ipc_receive_blocking_for(kernel_process, g_supervisor.kernel_receive_handle,
            &reply, timeout)) return false;

    bool valid = reply.word_count == 2U && reply.words[0] == SUPERVISOR_REPLY_PONG &&
        reply.words[1] == cookie && thread->state == THREAD_STATE_BLOCKED &&
        !thread->on_run_queue && endpoint_receiver_waiting(&g_supervisor.command_endpoint);

    if (!valid) return false;

    *out_cookie = reply.words[1];
    return true;
}

bool supervisor_stop(void) {
    if (!g_supervisor.active) {
        return supervisor_needs_cleanup() && supervisor_release(true);
    }

    Process *kernel_process = process_kernel();
    Thread *thread = supervisor_thread();

    if (!kernel_process || !thread) return false;
    if (thread->state != THREAD_STATE_BLOCKED ||
        !endpoint_receiver_waiting(&g_supervisor.command_endpoint)) return false;

    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 1U;
    request.words[0] = SUPERVISOR_MESSAGE_SHUTDOWN;

    if (!ipc_try_send(kernel_process, g_supervisor.kernel_send_handle, &request)) return false;

    IpcMessage reply;
    k_memset(&reply, 0, sizeof(reply));

    /* Shutdown is a two-phase synchronous operation: receive the STOPPED reply,
     * then observe the supervisor's THREAD_EXIT. Neither phase may depend on one
     * particular scheduler turn. */
    u64 timeout = supervisor_call_timeout_ticks();
    if (!timeout || !ipc_receive_blocking_for(kernel_process, g_supervisor.kernel_receive_handle,
            &reply, timeout)) return false;

    bool reply_ok = reply.word_count == 1U && reply.words[0] == SUPERVISOR_REPLY_STOPPED;
    bool thread_dead = reply_ok && supervisor_wait_thread_dead(timeout);

    bool endpoints_idle = !endpoint_message_ready(&g_supervisor.command_endpoint) &&
        !endpoint_message_ready(&g_supervisor.reply_endpoint) &&
        !endpoint_receiver_waiting(&g_supervisor.command_endpoint) &&
        !endpoint_sender_waiting(&g_supervisor.command_endpoint) &&
        !endpoint_receiver_waiting(&g_supervisor.reply_endpoint) &&
        !endpoint_sender_waiting(&g_supervisor.reply_endpoint);

    if (!reply_ok || !thread_dead || !endpoints_idle) return false;

    /* Thread is dead now, so transactional resource destruction is safe. */
    g_supervisor.active = false;
    return supervisor_release(false);
}

bool supervisor_running(void) {
    return g_supervisor.active && g_supervisor.program.process_created &&
        g_supervisor.program.thread_created && g_supervisor.program.published;
}

u64 supervisor_process_id(void) {
    return supervisor_running() ? g_supervisor.program.process.id : 0;
}

u64 supervisor_thread_id(void) {
    return supervisor_running() ? g_supervisor.program.thread.id : 0;
}

bool supervisor_stack_guarded(void) {
    if (!supervisor_running() || !g_supervisor.program.stack_mapped) return false;
    AddressSpace *space = process_address_space(&g_supervisor.program.process);
    return space && user_stack_mapping_valid(space, USER_STACK_INITIAL_BASE,
        g_supervisor.program.stack_frame);
}
