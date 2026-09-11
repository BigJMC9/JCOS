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
#include "user_elf.h"
#include "vfs.h"
#include "vmm.h"

#define SUPERVISOR_PATH "/bin/supervisor.elf"
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
    bool image_loaded;
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

static bool supervisor_release(void) {
    bool result = true;
    AddressSpace *space = g_supervisor.process_created ? process_address_space(&g_supervisor.process) : 0;

    /* Never destroy a live/queued/waiting supervisor thread through this path. */
    if (g_supervisor.thread_created) {
        if (thread_current() == &g_supervisor.thread || g_supervisor.thread.on_run_queue ||
            g_supervisor.thread.state == THREAD_STATE_RUNNING ||
            g_supervisor.thread.state == THREAD_STATE_BLOCKED) {
            return false;
        }

        if (!thread_destroy(&g_supervisor.thread)) result = false;
        else g_supervisor.thread_created = false;
    }

    frame_t ignored = FRAME_INVALID;

    ignored = FRAME_INVALID;

    if (g_supervisor.stack_mapped && space) {
        if (!address_space_unmap_page(space, SUPERVISOR_USER_STACK, &ignored)) result = false;
        else g_supervisor.stack_mapped = false;
    }

    if (g_supervisor.stack_frame_allocated) {
        if (!frame_free(g_supervisor.stack_frame)) result = false;
        else g_supervisor.stack_frame_allocated = false;
    }

    if (g_supervisor.image_loaded) {
        if (!user_elf_unload(&g_supervisor.process, &g_supervisor.image)) result = false;
        else g_supervisor.image_loaded = false;
    }

    Process *kernel_process = process_kernel();
    CapabilityTable *kernel_caps = kernel_process ? process_capabilities(kernel_process) : 0;
    CapabilityTable *user_caps = g_supervisor.process_created ? process_capabilities(&g_supervisor.process) : 0;

    if (g_supervisor.kernel_send_cap) {
        if (!kernel_caps || !capability_revoke(kernel_caps, g_supervisor.kernel_send_handle)) result = false;
        else g_supervisor.kernel_send_cap = false;
    }

    if (g_supervisor.kernel_receive_cap) {
        if (!kernel_caps || !capability_revoke(kernel_caps, g_supervisor.kernel_receive_handle)) result = false;
        else g_supervisor.kernel_receive_cap = false;
    }

    if (g_supervisor.user_receive_cap) {
        if (!user_caps || !capability_revoke(user_caps, g_supervisor.user_receive_handle)) result = false;
        else g_supervisor.user_receive_cap = false;
    }

    if (g_supervisor.user_send_cap) {
        if (!user_caps || !capability_revoke(user_caps, g_supervisor.user_send_handle)) result = false;
        else g_supervisor.user_send_cap = false;
    }

    if (g_supervisor.command_created) {
        if (!endpoint_destroy(&g_supervisor.command_endpoint)) result = false;
        else g_supervisor.command_created = false;
    }

    if (g_supervisor.reply_created) {
        if (!endpoint_destroy(&g_supervisor.reply_endpoint)) result = false;
        else g_supervisor.reply_created = false;
    }

    if (g_supervisor.process_created) {
        if (!process_destroy(&g_supervisor.process)) result = false;
        else g_supervisor.process_created = false;
    }

    if (result) k_memset(&g_supervisor, 0, sizeof(g_supervisor));
    return result;
}

bool supervisor_start(void) {
    if (g_supervisor.active) return false;

    k_memset(&g_supervisor, 0, sizeof(g_supervisor));

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

    g_supervisor.image_loaded = true;
    g_supervisor.stack_frame = frame_alloc();

    if (g_supervisor.stack_frame == FRAME_INVALID) goto fail;

    g_supervisor.stack_frame_allocated = true;

    if (!address_space_map_page(space, SUPERVISOR_USER_STACK, g_supervisor.stack_frame, VM_WRITE)) goto fail;

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

    /* Supervisor runs, replies, loops and blocks on its next RECEIVE. */
    if (!supervisor_schedule_once()) return false;

    IpcMessage reply;

    k_memset(&reply, 0, sizeof(reply));

    if (!ipc_try_receive(kernel_process, g_supervisor.kernel_receive_handle, &reply)) return false;

    bool valid = reply.word_count == 2U && reply.words[0] == SUPERVISOR_REPLY_PONG &&
        reply.words[1] == cookie && g_supervisor.thread.state == THREAD_STATE_BLOCKED &&
        !g_supervisor.thread.on_run_queue && endpoint_receiver_waiting(&g_supervisor.command_endpoint) &&
        scheduler_thread_count() == 1ULL;

    if (!valid) return false;

    *out_cookie = reply.words[1];

    return true;
}

bool supervisor_stop(void) {
    if (!g_supervisor.active) return false;

    Process *kernel_process = process_kernel();

    if (!kernel_process) return false;

    IpcMessage request;

    k_memset(&request, 0, sizeof(request));

    request.word_count = 1U;
    request.words[0] = SUPERVISOR_MESSAGE_SHUTDOWN;

    if (!ipc_try_send(kernel_process, g_supervisor.kernel_send_handle, &request)) return false;

    /* User receives SHUTDOWN, replies STOPPED, then exits through SYSCALL_THREAD_EXIT. */
    if (!supervisor_schedule_once()) return false;

    IpcMessage reply;

    k_memset(&reply, 0, sizeof(reply));

    bool received = ipc_try_receive(kernel_process, g_supervisor.kernel_receive_handle, &reply);
    bool reply_ok = received && reply.word_count == 1U && reply.words[0] == SUPERVISOR_REPLY_STOPPED;

    bool thread_dead = g_supervisor.thread.state == THREAD_STATE_DEAD &&
        !g_supervisor.thread.on_run_queue && !g_supervisor.thread.interrupt_context_ready &&
        !g_supervisor.thread.interrupt_rsp;

    bool endpoints_idle = !endpoint_message_ready(&g_supervisor.command_endpoint) &&
        !endpoint_message_ready(&g_supervisor.reply_endpoint) &&
        !endpoint_receiver_waiting(&g_supervisor.command_endpoint) &&
        !endpoint_sender_waiting(&g_supervisor.command_endpoint) &&
        !endpoint_receiver_waiting(&g_supervisor.reply_endpoint) &&
        !endpoint_sender_waiting(&g_supervisor.reply_endpoint);

    if (!reply_ok || !thread_dead || !endpoints_idle || scheduler_thread_count() != 1ULL) return false;

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