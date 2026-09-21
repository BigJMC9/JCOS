#include "supervisor.h"

#include "address_space.h"
#include "capability.h"
#include "endpoint.h"
#include "interrupts.h"
#include "ipc.h"
#include "lib.h"
#include "process.h"
#include "process_exit_queue.h"
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
    ProcessExitQueue exit_queue;
    Endpoint command_endpoint;
    Endpoint reply_endpoint;
    CapabilityHandle kernel_send_handle;
    CapabilityHandle kernel_receive_handle;
    CapabilityHandle kernel_command_grant_handle;
    CapabilityHandle kernel_reply_grant_handle;
    bool exit_queue_created;
    bool command_created;
    bool reply_created;
    bool kernel_send_cap;
    bool kernel_receive_cap;
    bool kernel_command_grant_cap;
    bool kernel_reply_grant_cap;
    SupervisorLifecycleState state;
} SupervisorRuntime;

static SupervisorRuntime g_supervisor;
static ProcessExitInfo g_last_exit;
static bool g_last_exit_valid;
static SupervisorStopResult g_last_stop_result;

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
    return g_supervisor.command_created && thread && thread->state == THREAD_STATE_BLOCKED &&
        !thread->on_run_queue && thread->interrupt_context_ready && thread->interrupt_rsp &&
        endpoint_receiver_waiting(&g_supervisor.command_endpoint);
}

static bool supervisor_thread_dead(void) {
    Thread *thread = supervisor_thread();
    return thread && thread->state == THREAD_STATE_DEAD &&
        !thread->on_run_queue && !thread->interrupt_context_ready && !thread->interrupt_rsp;
}

static bool supervisor_record_pending_exit(u64 expected_pid, bool required) {
    if (!g_supervisor.exit_queue_created) return !required;
    u32 pending = process_exit_queue_pending_count(&g_supervisor.exit_queue);
    if (!pending) return !required;
    if (pending != 1U) return false;

    ProcessExitInfo info;
    k_memset(&info, 0, sizeof(info));
    if (!process_exit_queue_try_receive(&g_supervisor.exit_queue, &info) ||
        info.reason == PROCESS_EXIT_NONE || (expected_pid && info.process_id != expected_pid)) return false;

    g_last_exit = info;
    g_last_exit_valid = true;
    return process_exit_queue_pending_count(&g_supervisor.exit_queue) == 0U;
}

static void supervisor_refresh_terminal(void) {
    if ((g_supervisor.state == SUPERVISOR_STATE_STOPPED) || !g_supervisor.program.process_created) return;

    u64 pid = g_supervisor.program.process.id;
    if (g_supervisor.exit_queue_created && process_exit_queue_pending_count(&g_supervisor.exit_queue)) {
        if (!supervisor_record_pending_exit(pid, true)) {
            g_supervisor.state = SUPERVISOR_STATE_FAILED;
            return;
        }
        if (g_supervisor.state == SUPERVISOR_STATE_CLOSING &&
            g_last_exit.reason == PROCESS_EXIT_NORMAL) {
            g_supervisor.state = SUPERVISOR_STATE_REAP_PENDING;
        } else {
            g_supervisor.state = SUPERVISOR_STATE_FAILED;
        }
        return;
    }

    ProcessExitInfo info;
    if (process_exit_info(&g_supervisor.program.process, &info)) {
        /* A watched terminal Process must also have published its reserved queue
         * event. Missing delivery is a lifecycle invariant failure, not health. */
        g_supervisor.state = SUPERVISOR_STATE_FAILED;
    }
}

static bool supervisor_wait_ready(void) {
    u64 timeout_ticks = supervisor_call_timeout_ticks();
    u64 started = timeout_ticks ? timer_ticks() : 0;
    u32 turns = 0;

    for (;;) {
        supervisor_refresh_terminal();
        if (g_supervisor.state == SUPERVISOR_STATE_FAILED) return false;
        if (supervisor_ready()) return true;

        Thread *thread = supervisor_thread();
        if (!thread || thread->state != THREAD_STATE_READY || !thread->on_run_queue) return false;

        if (timeout_ticks) {
            if ((u64)(timer_ticks() - started) >= timeout_ticks) return false;
        } else {
            if (turns >= SUPERVISOR_BOOT_START_MAX_TURNS) return false;
        }

        if (!supervisor_schedule_once()) return false;
        ++turns;
    }
}

static bool supervisor_wait_thread_dead(u64 timeout_ticks) {
    if (!timeout_ticks || !timer_initialized()) return false;

    u64 started = timer_ticks();
    while (!supervisor_thread_dead()) {
        if ((u64)(timer_ticks() - started) >= timeout_ticks) return false;
        Thread *thread = supervisor_thread();
        if (!thread || thread->state != THREAD_STATE_READY || !thread->on_run_queue) return false;
        if (!supervisor_schedule_once()) return false;
    }
    return true;
}

static bool supervisor_endpoints_idle(void) {
    return g_supervisor.command_created && g_supervisor.reply_created &&
        !endpoint_message_ready(&g_supervisor.command_endpoint) &&
        !endpoint_message_ready(&g_supervisor.reply_endpoint) &&
        !endpoint_receiver_waiting(&g_supervisor.command_endpoint) &&
        !endpoint_sender_waiting(&g_supervisor.command_endpoint) &&
        !endpoint_receiver_waiting(&g_supervisor.reply_endpoint) &&
        !endpoint_sender_waiting(&g_supervisor.reply_endpoint);
}

static bool supervisor_needs_cleanup(void) {
    return program_instance_needs_cleanup(&g_supervisor.program) ||
        g_supervisor.exit_queue_created || g_supervisor.command_created || g_supervisor.reply_created ||
        g_supervisor.kernel_send_cap || g_supervisor.kernel_receive_cap ||
        g_supervisor.kernel_command_grant_cap || g_supervisor.kernel_reply_grant_cap ||
        g_supervisor.kernel_send_handle != CAPABILITY_INVALID_HANDLE ||
        g_supervisor.kernel_receive_handle != CAPABILITY_INVALID_HANDLE ||
        g_supervisor.kernel_command_grant_handle != CAPABILITY_INVALID_HANDLE ||
        g_supervisor.kernel_reply_grant_handle != CAPABILITY_INVALID_HANDLE;
}

static bool supervisor_release(bool force_program) {
    u64 expected_pid = g_supervisor.program.process_created ? g_supervisor.program.process.id : 0ULL;

    /* A spontaneous or graceful exit may already have delivered before reap.
     * Consume it while the Process identity is still available for validation. */
    if (g_supervisor.exit_queue_created && process_exit_queue_pending_count(&g_supervisor.exit_queue) &&
        !supervisor_record_pending_exit(expected_pid, true)) {
        g_supervisor.state = SUPERVISOR_STATE_REAP_PENDING;
        return false;
    }

    if (program_instance_needs_cleanup(&g_supervisor.program)) {
        bool released = force_program ? program_terminate(&g_supervisor.program) : program_reap(&g_supervisor.program);
        if (!released) {
            g_supervisor.state = SUPERVISOR_STATE_REAP_PENDING;
            return false;
        }
    }

    /* Forced quiesce may have generated the terminal event during the call. */
    if (g_supervisor.exit_queue_created && process_exit_queue_pending_count(&g_supervisor.exit_queue) &&
        !supervisor_record_pending_exit(expected_pid, true)) {
        g_supervisor.state = SUPERVISOR_STATE_REAP_PENDING;
        return false;
    }
    if (g_supervisor.exit_queue_created &&
        (process_exit_queue_watch_count(&g_supervisor.exit_queue) ||
         process_exit_queue_pending_count(&g_supervisor.exit_queue))) {
        g_supervisor.state = SUPERVISOR_STATE_REAP_PENDING;
        return false;
    }

    /* The userspace Process no longer retains these ownerless transport
     * objects. Closing them clears any unconsumed command/reply left by a
     * failed or timed-out protocol before capability release and destruction. */
    if (g_supervisor.command_created && !endpoint_closed(&g_supervisor.command_endpoint) &&
        !ipc_endpoint_close(&g_supervisor.command_endpoint)) {
        g_supervisor.state = SUPERVISOR_STATE_REAP_PENDING;
        return false;
    }
    if (g_supervisor.reply_created && !endpoint_closed(&g_supervisor.reply_endpoint) &&
        !ipc_endpoint_close(&g_supervisor.reply_endpoint)) {
        g_supervisor.state = SUPERVISOR_STATE_REAP_PENDING;
        return false;
    }

    Process *kernel_process = process_kernel();
    CapabilityTable *kernel_caps = kernel_process ? process_capabilities(kernel_process) : 0;

    if (g_supervisor.kernel_send_cap) {
        if (!kernel_caps || !capability_revoke(kernel_caps, g_supervisor.kernel_send_handle)) goto retained;
        g_supervisor.kernel_send_cap = false;
    }
    if (g_supervisor.kernel_receive_cap) {
        if (!kernel_caps || !capability_revoke(kernel_caps, g_supervisor.kernel_receive_handle)) goto retained;
        g_supervisor.kernel_receive_cap = false;
    }
    if (g_supervisor.kernel_command_grant_cap) {
        if (!kernel_caps || !capability_revoke(kernel_caps, g_supervisor.kernel_command_grant_handle)) goto retained;
        g_supervisor.kernel_command_grant_cap = false;
    }
    if (g_supervisor.kernel_reply_grant_cap) {
        if (!kernel_caps || !capability_revoke(kernel_caps, g_supervisor.kernel_reply_grant_handle)) goto retained;
        g_supervisor.kernel_reply_grant_cap = false;
    }
    if (g_supervisor.command_created) {
        if (!endpoint_destroy(&g_supervisor.command_endpoint)) goto retained;
        g_supervisor.command_created = false;
    }
    if (g_supervisor.reply_created) {
        if (!endpoint_destroy(&g_supervisor.reply_endpoint)) goto retained;
        g_supervisor.reply_created = false;
    }
    if (g_supervisor.exit_queue_created) {
        if (!g_supervisor.exit_queue.closed && !process_exit_queue_close(&g_supervisor.exit_queue)) goto retained;
        if (!process_exit_queue_destroy(&g_supervisor.exit_queue)) goto retained;
        g_supervisor.exit_queue_created = false;
    }

    k_memset(&g_supervisor, 0, sizeof(g_supervisor));
    return true;

retained:
    g_supervisor.state = SUPERVISOR_STATE_REAP_PENDING;
    return false;
}

bool supervisor_start(void) {
    if (g_supervisor.state != SUPERVISOR_STATE_STOPPED || supervisor_needs_cleanup()) return false;

    Process *kernel_process = process_kernel();
    Thread *current = thread_current();
    if (!kernel_process || !current || current->process != kernel_process ||
        current->state != THREAD_STATE_RUNNING || !current->on_run_queue || scheduler_thread_count() != 1ULL) {
        return false;
    }

    CapabilityTable *kernel_caps = process_capabilities(kernel_process);
    if (!kernel_caps) return false;

    g_supervisor.state = SUPERVISOR_STATE_STARTING;

    if (!process_exit_queue_create(&g_supervisor.exit_queue)) goto fail;
    g_supervisor.exit_queue_created = true;

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
    spec.exit_queue = &g_supervisor.exit_queue;
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
    if (!supervisor_wait_ready()) goto fail;

    g_supervisor.state = SUPERVISOR_STATE_RUNNING;
    return true;

fail:
    g_supervisor.state = SUPERVISOR_STATE_FAILED;
    (void)supervisor_release(true);
    return false;
}

SupervisorLifecycleState supervisor_state(void) {
    u64 flags = supervisor_interrupt_save();
    supervisor_refresh_terminal();
    SupervisorLifecycleState state = g_supervisor.state;
    supervisor_interrupt_restore(flags);
    return state;
}

bool supervisor_running(void) {
    if (supervisor_state() != SUPERVISOR_STATE_RUNNING ||
        !g_supervisor.program.process_created || !g_supervisor.program.thread_created ||
        !g_supervisor.program.published) return false;
    Thread *thread = supervisor_thread();
    return thread && thread->state != THREAD_STATE_DEAD;
}

bool supervisor_idle(void) {
    return supervisor_running() && supervisor_ready();
}

static bool supervisor_request_reply(const IpcMessage *request, u64 expected_reply, IpcMessage *reply) {
    if (!request || !reply || !supervisor_idle()) return false;
    Process *kernel_process = process_kernel();
    if (!kernel_process || !ipc_try_send(kernel_process, g_supervisor.kernel_send_handle, request)) return false;

    k_memset(reply, 0, sizeof(*reply));
    u64 timeout = supervisor_call_timeout_ticks();
    if (!timeout || !ipc_receive_blocking_for(kernel_process, g_supervisor.kernel_receive_handle, reply, timeout)) {
        supervisor_refresh_terminal();
        if (g_supervisor.state == SUPERVISOR_STATE_RUNNING) g_supervisor.state = SUPERVISOR_STATE_FAILED;
        return false;
    }
    /* Receiving the reply only proves that the service completed this request.
     * Under live preemption the caller can resume before userspace has looped
     * back into its next blocking RECEIVE. Settle that READY->BLOCKED window
     * boundedly instead of treating scheduler ordering as a service failure. */
    if (!reply->word_count || reply->words[0] != expected_reply || !supervisor_wait_ready()) {
        supervisor_refresh_terminal();
        if (g_supervisor.state == SUPERVISOR_STATE_RUNNING) g_supervisor.state = SUPERVISOR_STATE_FAILED;
        return false;
    }
    return true;
}

bool supervisor_ping(u64 cookie, u64 *out_cookie) {
    if (!out_cookie) return false;
    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 2U;
    request.words[0] = SUPERVISOR_MESSAGE_PING;
    request.words[1] = cookie;

    IpcMessage reply;
    if (!supervisor_request_reply(&request, SUPERVISOR_REPLY_PONG, &reply) ||
        reply.word_count != 2U || reply.words[1] != cookie) return false;
    *out_cookie = reply.words[1];
    return true;
}

SupervisorStopResult supervisor_stop_bounded(void) {
    g_last_stop_result = SUPERVISOR_STOP_NONE;
    SupervisorLifecycleState state = supervisor_state();
    if (state == SUPERVISOR_STATE_STOPPED) return SUPERVISOR_STOP_NONE;

    bool graceful = false;
    u64 expected_pid = g_supervisor.program.process_created ? g_supervisor.program.process.id : 0ULL;

    if (state == SUPERVISOR_STATE_RUNNING && supervisor_ready()) {
        g_supervisor.state = SUPERVISOR_STATE_CLOSING;

        IpcMessage request;
        k_memset(&request, 0, sizeof(request));
        request.word_count = 1U;
        request.words[0] = SUPERVISOR_MESSAGE_SHUTDOWN;

        Process *kernel_process = process_kernel();
        IpcMessage reply;
        k_memset(&reply, 0, sizeof(reply));
        u64 timeout = supervisor_call_timeout_ticks();
        bool sent = kernel_process && ipc_try_send(kernel_process, g_supervisor.kernel_send_handle, &request);
        bool replied = sent && timeout && ipc_receive_blocking_for(kernel_process,
            g_supervisor.kernel_receive_handle, &reply, timeout);
        bool reply_ok = replied && reply.word_count == 1U && reply.words[0] == SUPERVISOR_REPLY_STOPPED;
        bool thread_dead = reply_ok && supervisor_wait_thread_dead(timeout);
        bool event_ok = thread_dead && supervisor_record_pending_exit(expected_pid, true) &&
            g_last_exit_valid && g_last_exit.reason == PROCESS_EXIT_NORMAL;
        graceful = event_ok && supervisor_endpoints_idle();

        if (graceful) {
            g_supervisor.state = SUPERVISOR_STATE_REAP_PENDING;
            if (supervisor_release(false)) {
                g_last_stop_result = SUPERVISOR_STOP_GRACEFUL;
                return g_last_stop_result;
            }
            g_last_stop_result = SUPERVISOR_STOP_FAILED;
            return g_last_stop_result;
        }
    }

    /* Any non-running, failed, hung, or protocol-invalid incarnation is reduced
     * to the same bounded force-quiesce/reap path. No request is replayed. */
    g_supervisor.state = SUPERVISOR_STATE_FAILED;
    if (supervisor_release(true)) {
        g_last_stop_result = SUPERVISOR_STOP_FORCED;
        return g_last_stop_result;
    }

    g_last_stop_result = SUPERVISOR_STOP_FAILED;
    return g_last_stop_result;
}

bool supervisor_stop(void) {
    SupervisorStopResult result = supervisor_stop_bounded();
    return result == SUPERVISOR_STOP_GRACEFUL || result == SUPERVISOR_STOP_FORCED;
}

bool supervisor_recover(void) {
    SupervisorLifecycleState state = supervisor_state();
    if (state == SUPERVISOR_STATE_STOPPED) return true;
    if (state == SUPERVISOR_STATE_RUNNING) return false;
    g_supervisor.state = SUPERVISOR_STATE_FAILED;
    return supervisor_release(true);
}

bool supervisor_restart(void) {
    SupervisorLifecycleState state = supervisor_state();
    if (state == SUPERVISOR_STATE_RUNNING) {
        if (!supervisor_stop()) return false;
    } else if (state != SUPERVISOR_STATE_STOPPED && !supervisor_recover()) {
        return false;
    }
    return supervisor_start();
}

SupervisorStopResult supervisor_last_stop_result(void) {
    return g_last_stop_result;
}

bool supervisor_last_exit_info(ProcessExitInfo *out) {
    if (!out) return false;
    u64 flags = supervisor_interrupt_save();
    k_memset(out, 0, sizeof(*out));
    bool valid = g_last_exit_valid;
    if (valid) *out = g_last_exit;
    supervisor_interrupt_restore(flags);
    return valid;
}

bool supervisor_test_arm_shutdown_hang(void) {
    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 1U;
    request.words[0] = SUPERVISOR_MESSAGE_DIAGNOSTIC_ARM_SHUTDOWN_HANG;
    IpcMessage reply;
    return supervisor_request_reply(&request, SUPERVISOR_REPLY_DIAGNOSTIC_ARMED, &reply) &&
        reply.word_count == 1U;
}

bool supervisor_test_fault(void) {
    if (!supervisor_idle()) return false;
    u64 expected_pid = g_supervisor.program.process.id;
    Process *kernel_process = process_kernel();
    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    request.word_count = 1U;
    request.words[0] = SUPERVISOR_MESSAGE_DIAGNOSTIC_FAULT;
    if (!kernel_process || !ipc_try_send(kernel_process, g_supervisor.kernel_send_handle, &request) ||
        !supervisor_schedule_once()) return false;

    supervisor_refresh_terminal();
    return g_supervisor.state == SUPERVISOR_STATE_FAILED && g_last_exit_valid &&
        g_last_exit.reason == PROCESS_EXIT_FAULT && g_last_exit.process_id == expected_pid &&
        g_last_exit.vector == 6ULL;
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
