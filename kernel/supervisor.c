#include "supervisor.h"

#include "address_space.h"
#include "capability.h"
#include "endpoint.h"
#include "interrupts.h"
#include "ipc.h"
#include "lib.h"
#include "physmap.h"
#include "pmm.h"
#include "process.h"
#include "scheduler.h"
#include "thread.h"
#include "timer.h"
#include "user_elf.h"
#include "vfs.h"
#include "vmm.h"
#include "user_stack.h"

#define SUPERVISOR_PATH "/bin/supervisor.elf"
#define SUPERVISOR_CALL_TIMEOUT_SECONDS 2ULL
#define SUPERVISOR_USER_STACK (ADDRESS_SPACE_USER_BASE + 0x100000ULL)
#define SUPERVISOR_USER_STACK_TOP (SUPERVISOR_USER_STACK + VM_PAGE_SIZE)
#define SUPERVISOR_INITIAL_STACK_SIZE (2ULL * sizeof(u64))
#define SUPERVISOR_RFLAGS_IF (1ULL << 9)

typedef struct {
    Process process;
    Thread thread;
    Endpoint command_endpoint;
    Endpoint reply_endpoint;
    CapabilityHandle kernel_send_handle;
    CapabilityHandle kernel_receive_handle;
    CapabilityHandle user_receive_handle;
    CapabilityHandle user_send_handle;
    UserElfImage image;
    frame_t stack_frame;
    bool process_created;
    bool command_created;
    bool reply_created;
    bool kernel_send_cap;
    bool kernel_receive_cap;
    bool user_receive_cap;
    bool user_send_cap;
    bool thread_created;
    bool stack_frame_allocated;
    bool stack_mapped;
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

static bool supervisor_thread_dead(void) {
    return g_supervisor.thread.state == THREAD_STATE_DEAD &&
        !g_supervisor.thread.on_run_queue && !g_supervisor.thread.interrupt_context_ready &&
        !g_supervisor.thread.interrupt_rsp;
}

static bool supervisor_wait_thread_dead(u64 timeout_ticks) {
    if (!timeout_ticks || !timer_initialized()) return false;

    u64 started = timer_ticks();
    while (!supervisor_thread_dead()) {
        if ((u64)(timer_ticks() - started) >= timeout_ticks) return false;

        /* If the reply woke the caller before the supervisor executed its
         * THREAD_EXIT syscall, keep scheduling boundedly until that terminal
         * transition happens. READY is the only valid non-dead state here. */
        if (g_supervisor.thread.state != THREAD_STATE_READY || !g_supervisor.thread.on_run_queue) return false;
        if (!supervisor_schedule_once()) return false;
    }
    return true;
}

static bool supervisor_release(void) {
    AddressSpace *space = g_supervisor.process_created ? process_address_space(&g_supervisor.process) : 0;

    if (g_supervisor.thread_created) {
        if (thread_current() == &g_supervisor.thread || g_supervisor.thread.on_run_queue ||
            g_supervisor.thread.state == THREAD_STATE_RUNNING ||
            g_supervisor.thread.state == THREAD_STATE_BLOCKED) return false;
        if (!thread_destroy(&g_supervisor.thread)) return false;
        g_supervisor.thread_created = false;
    }

    if (g_supervisor.stack_mapped) {
        frame_t mapped = FRAME_INVALID;
        frame_t old = FRAME_INVALID;
        if (!space || !user_stack_mapping_valid(space, SUPERVISOR_USER_STACK, g_supervisor.stack_frame)) return false;
        if (!address_space_query_page(space, SUPERVISOR_USER_STACK, &mapped, 0) || mapped != g_supervisor.stack_frame) return false;
        if (!address_space_unmap_page(space, SUPERVISOR_USER_STACK, &old)) return false;
        g_supervisor.stack_mapped = false;
        if (old != g_supervisor.stack_frame) return false;
    }
    if (g_supervisor.stack_frame_allocated) {
        if (!frame_free(g_supervisor.stack_frame)) return false;
        g_supervisor.stack_frame_allocated = false;
    }

    /* Failed load rollback can own pages even though load returned false. */
    if (user_elf_needs_cleanup(&g_supervisor.image) && !user_elf_unload(&g_supervisor.process, &g_supervisor.image)) return false;

    Process *kernel_process = process_kernel();
    CapabilityTable *kernel_caps = kernel_process ? process_capabilities(kernel_process) : 0;
    CapabilityTable *user_caps = g_supervisor.process_created ? process_capabilities(&g_supervisor.process) : 0;

    if (g_supervisor.kernel_send_cap) {
        if (!kernel_caps || !capability_revoke(kernel_caps, g_supervisor.kernel_send_handle)) return false;
        g_supervisor.kernel_send_cap = false;
    }
    if (g_supervisor.kernel_receive_cap) {
        if (!kernel_caps || !capability_revoke(kernel_caps, g_supervisor.kernel_receive_handle)) return false;
        g_supervisor.kernel_receive_cap = false;
    }
    if (g_supervisor.user_receive_cap) {
        if (!user_caps || !capability_revoke(user_caps, g_supervisor.user_receive_handle)) return false;
        g_supervisor.user_receive_cap = false;
    }
    if (g_supervisor.user_send_cap) {
        if (!user_caps || !capability_revoke(user_caps, g_supervisor.user_send_handle)) return false;
        g_supervisor.user_send_cap = false;
    }
    if (g_supervisor.command_created) {
        if (!endpoint_destroy(&g_supervisor.command_endpoint)) return false;
        g_supervisor.command_created = false;
    }
    if (g_supervisor.reply_created) {
        if (!endpoint_destroy(&g_supervisor.reply_endpoint)) return false;
        g_supervisor.reply_created = false;
    }
    if (g_supervisor.process_created) {
        if (!process_destroy(&g_supervisor.process)) return false;
        g_supervisor.process_created = false;
    }
    k_memset(&g_supervisor, 0, sizeof(g_supervisor));
    return true;
}

bool supervisor_start(void) {
    if (g_supervisor.active) return false;

    /* Retry retained cleanup instead of erasing a failed prior startup. */
    if (!supervisor_release()) return false;

    Process *kernel_process = process_kernel();
    Thread *current = thread_current();

    if (!kernel_process || !current || current->process != kernel_process ||
        current->state != THREAD_STATE_RUNNING || !current->on_run_queue || scheduler_thread_count() != 1ULL) {
        return false;
    }

    CapabilityTable *kernel_caps = process_capabilities(kernel_process);

    if (!kernel_caps) return false;
    if (!process_create(&g_supervisor.process)) return false;

    g_supervisor.process_created = true;

    AddressSpace *space = process_address_space(&g_supervisor.process);
    CapabilityTable *user_caps = process_capabilities(&g_supervisor.process);

    if (!space || !user_caps) goto fail;
    if (!endpoint_create(&g_supervisor.command_endpoint)) goto fail;

    g_supervisor.command_created = true;

    if (!endpoint_create(&g_supervisor.reply_endpoint)) goto fail;

    g_supervisor.reply_created = true;

    if (!capability_insert(kernel_caps, &g_supervisor.command_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND, &g_supervisor.kernel_send_handle)) {
        goto fail;
    }

    g_supervisor.kernel_send_cap = true;

    if (!capability_insert(kernel_caps, &g_supervisor.reply_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE, &g_supervisor.kernel_receive_handle)) {
        goto fail;
    }

    g_supervisor.kernel_receive_cap = true;

    if (!capability_insert(user_caps, &g_supervisor.command_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE, &g_supervisor.user_receive_handle)) {
        goto fail;
    }

    g_supervisor.user_receive_cap = true;

    if (!capability_insert(user_caps, &g_supervisor.reply_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND, &g_supervisor.user_send_handle)) {
        goto fail;
    }

    g_supervisor.user_send_cap = true;

    if (!thread_create(&g_supervisor.thread, &g_supervisor.process)) goto fail;

    g_supervisor.thread_created = true;

    VfsNode *file = vfs_resolve(vfs_root(), SUPERVISOR_PATH);

    if (!file || file->type != VFS_FILE || !file->data || !file->size) goto fail;
    if (!user_elf_load(&g_supervisor.process, file, &g_supervisor.image)) goto fail;

    g_supervisor.stack_frame = frame_alloc();

    if (g_supervisor.stack_frame == FRAME_INVALID) goto fail;

    g_supervisor.stack_frame_allocated = true;

    if (!user_stack_map_page(space, SUPERVISOR_USER_STACK, g_supervisor.stack_frame)) goto fail;

    g_supervisor.stack_mapped = true;
    u8 *stack = (u8 *)phys_to_virt(frame_to_phys(g_supervisor.stack_frame));

    if (!stack) goto fail;

    k_memset(stack, 0, (usize)VM_PAGE_SIZE);

    u64 initial_rsp = SUPERVISOR_USER_STACK_TOP - SUPERVISOR_INITIAL_STACK_SIZE;
    u64 stack_offset = initial_rsp - SUPERVISOR_USER_STACK;
    u64 *initial_stack = (u64 *)(void *)(stack + stack_offset);

    initial_stack[0] = g_supervisor.user_receive_handle;
    initial_stack[1] = g_supervisor.user_send_handle;

    if (!thread_prepare_user(&g_supervisor.thread, g_supervisor.image.entry, initial_rsp)) goto fail;
    if (!scheduler_add(&g_supervisor.thread)) goto fail;

    /* From this point forward, generic cleanup is no longer safe until the user thread voluntarily exits. */
    g_supervisor.active = true;

    /* Run /bin/supervisor.elf until it reaches its first blocking RECEIVE syscall. */
    if (!supervisor_schedule_once()) return false;

    bool idle = g_supervisor.thread.state == THREAD_STATE_BLOCKED && !g_supervisor.thread.on_run_queue &&
        g_supervisor.thread.interrupt_context_ready && g_supervisor.thread.interrupt_rsp &&
        endpoint_receiver_waiting(&g_supervisor.command_endpoint) && scheduler_thread_count() == 1ULL;

    return idle;

fail:
    (void)supervisor_release();
    return false;
}

bool supervisor_ping(u64 cookie, u64 *out_cookie) {
    if (!g_supervisor.active || !out_cookie) return false;

    Process *kernel_process = process_kernel();

    if (!kernel_process) return false;
    if (g_supervisor.thread.state != THREAD_STATE_BLOCKED) return false;
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
        reply.words[1] == cookie && g_supervisor.thread.state == THREAD_STATE_BLOCKED &&
        !g_supervisor.thread.on_run_queue && endpoint_receiver_waiting(&g_supervisor.command_endpoint);

    if (!valid) return false;

    *out_cookie = reply.words[1];

    return true;
}

bool supervisor_stop(void) {
    if (!g_supervisor.active) {
        return g_supervisor.process_created && supervisor_release();
    }

    Process *kernel_process = process_kernel();

    if (!kernel_process) return false;
    if (g_supervisor.thread.state != THREAD_STATE_BLOCKED ||
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
    return supervisor_release();
}

bool supervisor_running(void) {
    return
        g_supervisor.active &&
        g_supervisor.process_created &&
        g_supervisor.thread_created;
}

u64 supervisor_process_id(void) {
    return
        supervisor_running()
            ? g_supervisor.process.id
            : 0;
}

u64 supervisor_thread_id(void) {
    return
        supervisor_running()
            ? g_supervisor.thread.id
            : 0;
}
bool supervisor_stack_guarded(void) {
    if (!supervisor_running() || !g_supervisor.stack_mapped) return false;
    AddressSpace *space = process_address_space(&g_supervisor.process);
    return space && user_stack_mapping_valid(space, SUPERVISOR_USER_STACK, g_supervisor.stack_frame);
}
