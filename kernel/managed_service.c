#include "managed_service.h"

#include "interrupts.h"
#include "ipc.h"
#include "lib.h"
#include "process.h"
#include "scheduler.h"
#include "thread.h"
#include "timer.h"
#include "vfs.h"

#define MANAGED_SERVICE_CALL_TIMEOUT_SECONDS 2ULL
#define MANAGED_SERVICE_START_MAX_TURNS 64U
#define MANAGED_SERVICE_RFLAGS_IF (1ULL << 9)

static u64 g_next_managed_service_incarnation = 1ULL;

static u64 service_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void service_irq_restore(u64 flags) {
    if (flags & MANAGED_SERVICE_RFLAGS_IF) interrupts_enable();
}

static bool service_schedule_once(void) {
    u64 flags = service_irq_save();
    bool result = scheduler_yield();
    service_irq_restore(flags);
    return result;
}

static u64 service_timeout_ticks(void) {
    if (!timer_initialized()) return 0ULL;
    u32 frequency = timer_frequency();
    return frequency ? (u64)frequency * MANAGED_SERVICE_CALL_TIMEOUT_SECONDS : 0ULL;
}

static Thread *service_thread(ManagedService *service) {
    return service && service->program.thread_created ? &service->program.thread : 0;
}

static bool service_ready(ManagedService *service) {
    Thread *thread = service_thread(service);
    return service && service->command_created && thread &&
        thread->state == THREAD_STATE_BLOCKED && !thread->on_run_queue &&
        thread->interrupt_context_ready && thread->interrupt_rsp &&
        endpoint_receiver_waiting(&service->command_endpoint);
}

static bool service_thread_dead(ManagedService *service) {
    Thread *thread = service_thread(service);
    return thread && thread->state == THREAD_STATE_DEAD && !thread->on_run_queue &&
        !thread->interrupt_context_ready && !thread->interrupt_rsp;
}

static bool service_record_pending_exit(ManagedService *service, u64 expected_pid, bool required) {
    if (!service || !service->exit_queue_created) return !required;
    u32 pending = process_exit_queue_pending_count(&service->exit_queue);
    if (!pending) return !required;
    if (pending != 1U) return false;

    ProcessExitInfo info;
    k_memset(&info, 0, sizeof(info));
    if (!process_exit_queue_try_receive(&service->exit_queue, &info) ||
        info.reason == PROCESS_EXIT_NONE || (expected_pid && info.process_id != expected_pid)) return false;

    k_memcpy(&service->last_exit, &info, sizeof(service->last_exit));
    service->last_exit_valid = true;
    return process_exit_queue_pending_count(&service->exit_queue) == 0U;
}

static void service_refresh_terminal(ManagedService *service) {
    if (!service || service->state == MANAGED_SERVICE_STOPPED || !service->program.process_created) return;

    u64 pid = service->program.process.id;
    if (service->exit_queue_created && process_exit_queue_pending_count(&service->exit_queue)) {
        if (!service_record_pending_exit(service, pid, true)) {
            service->state = MANAGED_SERVICE_FAILED;
            return;
        }
        if (service->state == MANAGED_SERVICE_CLOSING &&
            service->last_exit.reason == PROCESS_EXIT_NORMAL) {
            service->state = MANAGED_SERVICE_REAP_PENDING;
        } else {
            service->state = MANAGED_SERVICE_FAILED;
        }
        return;
    }

    ProcessExitInfo info;
    if (process_exit_info(&service->program.process, &info)) {
        /* A watched terminal process must publish its reserved event first. */
        service->state = MANAGED_SERVICE_FAILED;
    }
}

static bool service_wait_ready(ManagedService *service) {
    u64 timeout = service_timeout_ticks();
    u64 started = timeout ? timer_ticks() : 0ULL;
    u32 turns = 0U;

    for (;;) {
        service_refresh_terminal(service);
        if (service->state == MANAGED_SERVICE_FAILED) return false;
        if (service_ready(service)) return true;

        Thread *thread = service_thread(service);
        if (!thread || thread->state != THREAD_STATE_READY || !thread->on_run_queue) return false;

        if (timeout) {
            if ((u64)(timer_ticks() - started) >= timeout) return false;
        } else if (turns >= MANAGED_SERVICE_START_MAX_TURNS) {
            return false;
        }

        if (!service_schedule_once()) return false;
        ++turns;
    }
}

static bool service_wait_thread_dead(ManagedService *service, u64 timeout) {
    if (!timeout || !timer_initialized()) return false;
    u64 started = timer_ticks();
    while (!service_thread_dead(service)) {
        if ((u64)(timer_ticks() - started) >= timeout) return false;
        Thread *thread = service_thread(service);
        if (!thread || thread->state != THREAD_STATE_READY || !thread->on_run_queue) return false;
        if (!service_schedule_once()) return false;
    }
    return true;
}

static bool service_endpoints_idle(const ManagedService *service) {
    return service && service->command_created && service->reply_created &&
        !endpoint_message_ready(&service->command_endpoint) &&
        !endpoint_message_ready(&service->reply_endpoint) &&
        !endpoint_receiver_waiting(&service->command_endpoint) &&
        !endpoint_sender_waiting(&service->command_endpoint) &&
        !endpoint_receiver_waiting(&service->reply_endpoint) &&
        !endpoint_sender_waiting(&service->reply_endpoint);
}

static bool service_needs_cleanup(const ManagedService *service) {
    return service && (program_instance_needs_cleanup(&service->program) ||
        service->exit_queue_created || service->command_created || service->reply_created ||
        service->kernel_send_cap || service->kernel_receive_cap ||
        service->kernel_command_grant_cap || service->kernel_reply_grant_cap ||
        service->kernel_send_handle != CAPABILITY_INVALID_HANDLE ||
        service->kernel_receive_handle != CAPABILITY_INVALID_HANDLE ||
        service->kernel_command_grant_handle != CAPABILITY_INVALID_HANDLE ||
        service->kernel_reply_grant_handle != CAPABILITY_INVALID_HANDLE);
}

static bool service_release(ManagedService *service, bool force_program) {
    if (!service) return false;
    u64 expected_pid = service->program.process_created ? service->program.process.id : 0ULL;

    if (service->exit_queue_created && process_exit_queue_pending_count(&service->exit_queue) &&
        !service_record_pending_exit(service, expected_pid, true)) goto retained;

    if (program_instance_needs_cleanup(&service->program)) {
        bool released = force_program ? program_terminate(&service->program) : program_reap(&service->program);
        if (!released) goto retained;
    }

    if (service->exit_queue_created && process_exit_queue_pending_count(&service->exit_queue) &&
        !service_record_pending_exit(service, expected_pid, true)) goto retained;
    if (service->exit_queue_created &&
        (process_exit_queue_watch_count(&service->exit_queue) ||
         process_exit_queue_pending_count(&service->exit_queue))) goto retained;

    if (service->command_created && !endpoint_closed(&service->command_endpoint) &&
        !ipc_endpoint_close(&service->command_endpoint)) goto retained;
    if (service->reply_created && !endpoint_closed(&service->reply_endpoint) &&
        !ipc_endpoint_close(&service->reply_endpoint)) goto retained;

    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;

    if (service->kernel_send_cap) {
        if (!caps || !capability_revoke(caps, service->kernel_send_handle)) goto retained;
        service->kernel_send_cap = false;
    }
    if (service->kernel_receive_cap) {
        if (!caps || !capability_revoke(caps, service->kernel_receive_handle)) goto retained;
        service->kernel_receive_cap = false;
    }
    if (service->kernel_command_grant_cap) {
        if (!caps || !capability_revoke(caps, service->kernel_command_grant_handle)) goto retained;
        service->kernel_command_grant_cap = false;
    }
    if (service->kernel_reply_grant_cap) {
        if (!caps || !capability_revoke(caps, service->kernel_reply_grant_handle)) goto retained;
        service->kernel_reply_grant_cap = false;
    }

    if (service->command_created) {
        if (!endpoint_destroy(&service->command_endpoint)) goto retained;
        service->command_created = false;
    }
    if (service->reply_created) {
        if (!endpoint_destroy(&service->reply_endpoint)) goto retained;
        service->reply_created = false;
    }
    if (service->exit_queue_created) {
        if (!service->exit_queue.closed && !process_exit_queue_close(&service->exit_queue)) goto retained;
        if (!process_exit_queue_destroy(&service->exit_queue)) goto retained;
        service->exit_queue_created = false;
    }

    ProcessExitInfo last_exit;
    k_memcpy(&last_exit, &service->last_exit, sizeof(last_exit));
    bool last_exit_valid = service->last_exit_valid;
    ManagedServiceStopResult last_stop = service->last_stop_result;
    k_memset(service, 0, sizeof(*service));
    k_memcpy(&service->last_exit, &last_exit, sizeof(service->last_exit));
    service->last_exit_valid = last_exit_valid;
    service->last_stop_result = last_stop;
    service->state = MANAGED_SERVICE_STOPPED;
    return true;

retained:
    service->state = MANAGED_SERVICE_REAP_PENDING;
    return false;
}

static bool service_spec_valid(const ManagedServiceSpec *spec) {
    return spec && spec->path && spec->path[0] && spec->shutdown_message && spec->shutdown_reply;
}

static bool service_extras_valid(const ManagedServiceLaunchExtras *extras) {
    if (!extras) return true;
    if (extras->startup_grant_count > MANAGED_SERVICE_MAX_EXTRA_CAPABILITIES ||
        extras->startup_argument_count > MANAGED_SERVICE_MAX_EXTRA_ARGUMENTS ||
        extras->shutdown_request_word_count > IPC_MESSAGE_MAX_WORDS) return false;
    if (extras->shutdown_request_word_count && !extras->shutdown_request_words[0]) return false;
    return true;
}

static bool service_allocate_incarnation(u64 *out) {
    if (!out || !g_next_managed_service_incarnation) return false;
    *out = g_next_managed_service_incarnation++;
    return *out != 0ULL;
}

static bool managed_service_start_resolved_ex(ManagedService *service, const VfsNode *file,
    u64 shutdown_message, u64 shutdown_reply, const ManagedServiceLaunchExtras *extras) {
    if (!service || !file || file->type != VFS_FILE || !file->data || !file->size ||
        !shutdown_message || !shutdown_reply || !service_extras_valid(extras) ||
        service->state != MANAGED_SERVICE_STOPPED || service_needs_cleanup(service)) return false;

    ProcessExitInfo previous_exit;
    k_memcpy(&previous_exit, &service->last_exit, sizeof(previous_exit));
    bool previous_exit_valid = service->last_exit_valid;
    k_memset(service, 0, sizeof(*service));
    k_memcpy(&service->last_exit, &previous_exit, sizeof(service->last_exit));
    service->last_exit_valid = previous_exit_valid;
    service->state = MANAGED_SERVICE_STARTING;
    service->shutdown_message = shutdown_message;
    service->shutdown_reply = shutdown_reply;
    if (extras && extras->shutdown_request_word_count) {
        service->shutdown_request_word_count = extras->shutdown_request_word_count;
        k_memcpy(service->shutdown_request_words, extras->shutdown_request_words,
            sizeof(service->shutdown_request_words));
    } else {
        service->shutdown_request_word_count = 1U;
        service->shutdown_request_words[0] = shutdown_message;
    }

    if (!service_allocate_incarnation(&service->incarnation)) goto fail;

    Process *kernel = process_kernel();
    Thread *current = thread_current();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!kernel || !current || current->process != kernel || current->state != THREAD_STATE_RUNNING ||
        !current->on_run_queue || !caps) goto fail;

    if (!process_exit_queue_create(&service->exit_queue)) goto fail;
    service->exit_queue_created = true;
    if (!endpoint_create(&service->command_endpoint)) goto fail;
    service->command_created = true;
    if (!endpoint_create(&service->reply_endpoint)) goto fail;
    service->reply_created = true;

    if (!capability_insert(caps, &service->command_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND, &service->kernel_send_handle)) goto fail;
    service->kernel_send_cap = true;
    if (!capability_insert(caps, &service->reply_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE, &service->kernel_receive_handle)) goto fail;
    service->kernel_receive_cap = true;
    if (!capability_insert(caps, &service->command_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE | CAPABILITY_RIGHT_TRANSFER,
            &service->kernel_command_grant_handle)) goto fail;
    service->kernel_command_grant_cap = true;
    if (!capability_insert(caps, &service->reply_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND | CAPABILITY_RIGHT_TRANSFER,
            &service->kernel_reply_grant_handle)) goto fail;
    service->kernel_reply_grant_cap = true;

    ProgramLaunchSpec launch;
    k_memset(&launch, 0, sizeof(launch));
    launch.file = file;
    launch.exit_queue = &service->exit_queue;
    launch.startup_grants[0].authority_table = caps;
    launch.startup_grants[0].authority_handle = service->kernel_command_grant_handle;
    launch.startup_grants[0].object = &service->command_endpoint;
    launch.startup_grants[0].type = CAPABILITY_TYPE_ENDPOINT;
    launch.startup_grants[0].rights = CAPABILITY_RIGHT_RECEIVE;
    launch.startup_grants[1].authority_table = caps;
    launch.startup_grants[1].authority_handle = service->kernel_reply_grant_handle;
    launch.startup_grants[1].object = &service->reply_endpoint;
    launch.startup_grants[1].type = CAPABILITY_TYPE_ENDPOINT;
    launch.startup_grants[1].rights = CAPABILITY_RIGHT_SEND;
    u32 extra_grants = extras ? extras->startup_grant_count : 0U;
    launch.startup_grant_count = MANAGED_SERVICE_BASE_STARTUP_CAPABILITIES + extra_grants;
    for (u32 i = 0; i < extra_grants; ++i) {
        k_memcpy(&launch.startup_grants[MANAGED_SERVICE_BASE_STARTUP_CAPABILITIES + i],
            &extras->startup_grants[i], sizeof(ProgramGrantSpec));
    }

    u32 extra_arguments = extras ? extras->startup_argument_count : 0U;
    launch.startup_argument_count = MANAGED_SERVICE_BASE_STARTUP_ARGUMENTS + extra_arguments;
    launch.startup_arguments[0] = service->incarnation;
    for (u32 i = 0; i < extra_arguments; ++i) {
        launch.startup_arguments[MANAGED_SERVICE_BASE_STARTUP_ARGUMENTS + i] = extras->startup_arguments[i];
    }
    if (extras) launch.borrowed_mapping = extras->borrowed_mapping;

    if (!program_launch(&service->program, &launch) || !service_wait_ready(service)) goto fail;
    service->state = MANAGED_SERVICE_RUNNING;
    return true;

fail:
    service->state = MANAGED_SERVICE_FAILED;
    (void)service_release(service, true);
    return false;
}

bool managed_service_start_file_ex(ManagedService *service, const VfsNode *file,
    u64 shutdown_message, u64 shutdown_reply, const ManagedServiceLaunchExtras *extras) {
    return managed_service_start_resolved_ex(service, file, shutdown_message, shutdown_reply, extras);
}

bool managed_service_start_ex(ManagedService *service, const ManagedServiceSpec *spec,
    const ManagedServiceLaunchExtras *extras) {
    if (!service_spec_valid(spec) || !service_extras_valid(extras)) return false;
    VfsNode *file = vfs_resolve(vfs_root(), spec->path);
    if (!file) return false;
    return managed_service_start_resolved_ex(service, file, spec->shutdown_message,
        spec->shutdown_reply, extras);
}

bool managed_service_start(ManagedService *service, const ManagedServiceSpec *spec) {
    return managed_service_start_ex(service, spec, 0);
}

ManagedServiceState managed_service_state(ManagedService *service) {
    if (!service) return MANAGED_SERVICE_FAILED;
    u64 flags = service_irq_save();
    service_refresh_terminal(service);
    ManagedServiceState state = service->state;
    service_irq_restore(flags);
    return state;
}

bool managed_service_running(ManagedService *service) {
    if (!service || managed_service_state(service) != MANAGED_SERVICE_RUNNING ||
        !service->program.process_created || !service->program.thread_created || !service->program.published) return false;
    Thread *thread = service_thread(service);
    return thread && thread->state != THREAD_STATE_DEAD;
}

bool managed_service_idle(ManagedService *service) {
    return managed_service_running(service) && service_ready(service);
}

u64 managed_service_incarnation(const ManagedService *service) {
    return service && service->state != MANAGED_SERVICE_STOPPED ? service->incarnation : 0ULL;
}

u64 managed_service_process_id(ManagedService *service) {
    return managed_service_running(service) ? service->program.process.id : 0ULL;
}

u64 managed_service_thread_id(ManagedService *service) {
    return managed_service_running(service) ? service->program.thread.id : 0ULL;
}

bool managed_service_connect_kernel(ManagedService *service, ManagedServiceConnection *out) {
    if (!out) return false;
    k_memset(out, 0, sizeof(*out));
    if (!managed_service_running(service) || !service->kernel_send_cap || !service->kernel_receive_cap) return false;
    out->incarnation = service->incarnation;
    out->send_handle = service->kernel_send_handle;
    out->receive_handle = service->kernel_receive_handle;
    return true;
}

bool managed_service_connection_current(ManagedService *service, const ManagedServiceConnection *connection) {
    return service && connection && managed_service_running(service) && connection->incarnation &&
        connection->incarnation == service->incarnation &&
        connection->send_handle == service->kernel_send_handle &&
        connection->receive_handle == service->kernel_receive_handle;
}

bool managed_service_send_oneway(ManagedService *service, const ManagedServiceConnection *connection,
    const IpcMessage *request) {
    if (!request || !managed_service_connection_current(service, connection) || !managed_service_idle(service)) return false;
    Process *kernel = process_kernel();
    return kernel && ipc_try_send(kernel, connection->send_handle, request);
}

bool managed_service_call(ManagedService *service, const ManagedServiceConnection *connection,
    const IpcMessage *request, IpcMessage *reply) {
    if (!request || !reply || !managed_service_connection_current(service, connection) ||
        !managed_service_idle(service)) return false;

    Process *kernel = process_kernel();
    if (!kernel || !ipc_try_send(kernel, connection->send_handle, request)) return false;

    k_memset(reply, 0, sizeof(*reply));
    u64 timeout = service_timeout_ticks();
    if (!timeout || !ipc_receive_blocking_for(kernel, connection->receive_handle, reply, timeout)) {
        service_refresh_terminal(service);
        if (service->state == MANAGED_SERVICE_RUNNING) service->state = MANAGED_SERVICE_FAILED;
        return false;
    }

    /* Standard R6 managed-service reply envelope: word 1 is incarnation. */
    if (reply->word_count < 2U || reply->words[1] != connection->incarnation ||
        !service_wait_ready(service)) {
        service_refresh_terminal(service);
        if (service->state == MANAGED_SERVICE_RUNNING) service->state = MANAGED_SERVICE_FAILED;
        return false;
    }
    return true;
}

ManagedServiceStopResult managed_service_stop_bounded(ManagedService *service) {
    if (!service) return MANAGED_SERVICE_STOP_FAILED;
    service->last_stop_result = MANAGED_SERVICE_STOP_NONE;
    ManagedServiceState state = managed_service_state(service);
    if (state == MANAGED_SERVICE_STOPPED) return MANAGED_SERVICE_STOP_NONE;

    u64 expected_pid = service->program.process_created ? service->program.process.id : 0ULL;
    if (state == MANAGED_SERVICE_RUNNING && service_ready(service)) {
        service->state = MANAGED_SERVICE_CLOSING;
        IpcMessage request;
        k_memset(&request, 0, sizeof(request));
        request.word_count = service->shutdown_request_word_count;
        k_memcpy(request.words, service->shutdown_request_words, sizeof(request.words));

        Process *kernel = process_kernel();
        IpcMessage reply;
        k_memset(&reply, 0, sizeof(reply));
        u64 timeout = service_timeout_ticks();
        bool sent = kernel && ipc_try_send(kernel, service->kernel_send_handle, &request);
        bool replied = sent && timeout && ipc_receive_blocking_for(kernel,
            service->kernel_receive_handle, &reply, timeout);
        bool reply_ok = replied && reply.word_count >= 2U &&
            reply.words[0] == service->shutdown_reply && reply.words[1] == service->incarnation;
        bool dead = reply_ok && service_wait_thread_dead(service, timeout);
        bool event_ok = dead && service_record_pending_exit(service, expected_pid, true) &&
            service->last_exit_valid && service->last_exit.reason == PROCESS_EXIT_NORMAL;
        bool graceful = event_ok && service_endpoints_idle(service);

        if (graceful) {
            service->state = MANAGED_SERVICE_REAP_PENDING;
            if (service_release(service, false)) {
                service->last_stop_result = MANAGED_SERVICE_STOP_GRACEFUL;
                return MANAGED_SERVICE_STOP_GRACEFUL;
            }
            service->last_stop_result = MANAGED_SERVICE_STOP_FAILED;
            return MANAGED_SERVICE_STOP_FAILED;
        }
    }

    service->state = MANAGED_SERVICE_FAILED;
    if (service_release(service, true)) {
        service->last_stop_result = MANAGED_SERVICE_STOP_FORCED;
        return MANAGED_SERVICE_STOP_FORCED;
    }
    service->last_stop_result = MANAGED_SERVICE_STOP_FAILED;
    return MANAGED_SERVICE_STOP_FAILED;
}

bool managed_service_recover(ManagedService *service) {
    if (!service) return false;
    ManagedServiceState state = managed_service_state(service);
    if (state == MANAGED_SERVICE_STOPPED) return true;
    if (state == MANAGED_SERVICE_RUNNING) return false;
    service->state = MANAGED_SERVICE_FAILED;
    return service_release(service, true);
}

bool managed_service_restart_ex(ManagedService *service, const ManagedServiceSpec *spec,
    const ManagedServiceLaunchExtras *extras) {
    if (!service || !service_spec_valid(spec) || !service_extras_valid(extras)) return false;
    ManagedServiceState state = managed_service_state(service);
    if (state == MANAGED_SERVICE_RUNNING) {
        ManagedServiceStopResult stopped = managed_service_stop_bounded(service);
        if (stopped != MANAGED_SERVICE_STOP_GRACEFUL && stopped != MANAGED_SERVICE_STOP_FORCED) return false;
    } else if (state != MANAGED_SERVICE_STOPPED && !managed_service_recover(service)) {
        return false;
    }
    return managed_service_start_ex(service, spec, extras);
}

bool managed_service_restart(ManagedService *service, const ManagedServiceSpec *spec) {
    return managed_service_restart_ex(service, spec, 0);
}

bool managed_service_last_exit_info(const ManagedService *service, ProcessExitInfo *out) {
    if (!service || !out) return false;
    k_memset(out, 0, sizeof(*out));
    if (!service->last_exit_valid) return false;
    k_memcpy(out, &service->last_exit, sizeof(*out));
    return true;
}

ManagedServiceStopResult managed_service_last_stop_result(const ManagedService *service) {
    return service ? service->last_stop_result : MANAGED_SERVICE_STOP_FAILED;
}
