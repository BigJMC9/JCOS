#include "syscall.h"
#include "thread.h"
#include "ipc.h"
#include "lib.h"
#include "scheduler.h"
#include "task.h"
#include "audio.h"
#include "display.h"
#include "framebuffer.h"
#include "timer.h"

_Static_assert(IPC_MESSAGE_MAX_WORDS == JCOS_IPC_MESSAGE_MAX_WORDS, "Kernel/userspace IPC ABI mismatch");

static Process *syscall_current_process(void) {
    Thread *thread = thread_current();

    if (!thread || !thread->id || !thread->process) return 0;
    return thread->process;
}

static void syscall_ipc_clear_receive(InterruptFrame *frame) {
    if (!frame) return;

    frame->rcx = 0;
    frame->rdx = 0;
    frame->rsi = 0;
    frame->rdi = 0;
    frame->r8 = 0;
}

static void syscall_ipc_try_send(InterruptFrame *frame) {
    if (!frame) return;
    if (!frame->rcx || frame->rcx > IPC_MESSAGE_MAX_WORDS) {
        frame->rax = SYSCALL_RESULT_FAILED;
        return;
    }

    Process *process = syscall_current_process();

    if (!process) {
        frame->rax = SYSCALL_RESULT_FAILED;
        return;
    }

    IpcMessage message;

    k_memset(&message, 0, sizeof(message));

    message.word_count = (u32)frame->rcx;
    message.words[0] = frame->rdx;
    message.words[1] = frame->rsi;
    message.words[2] = frame->rdi;
    message.words[3] = frame->r8;

    bool sent = ipc_try_send(process, frame->rbx, &message);

    frame->rax = sent ? SYSCALL_RESULT_OK : SYSCALL_RESULT_FAILED;
}

static void syscall_ipc_try_receive(InterruptFrame *frame) {
    if (!frame) return;

    syscall_ipc_clear_receive(frame);

    Process *process = syscall_current_process();

    if (!process) {
        frame->rax = SYSCALL_RESULT_FAILED;
        return;
    }

    IpcMessage message;

    k_memset(&message, 0, sizeof(message));

    bool received = ipc_try_receive(process, frame->rbx, &message);

    if (!received) {
        frame->rax = SYSCALL_RESULT_FAILED;
        return;
    }

    frame->rax = SYSCALL_RESULT_OK;
    frame->rcx = message.word_count;
    frame->rdx = message.words[0];
    frame->rsi = message.words[1];
    frame->rdi = message.words[2];
    frame->r8 = message.words[3];
}

static void syscall_ipc_receive_blocking(InterruptFrame *frame) {
    if (!frame) return;

    syscall_ipc_clear_receive(frame);

    Process *process = syscall_current_process();

    if (!process) {
        frame->rax = SYSCALL_RESULT_FAILED;
        return;
    }

    IpcMessage message;

    k_memset(&message, 0, sizeof(message));

    bool received = ipc_receive_blocking(process, frame->rbx, &message);

    if (!received) {
        frame->rax = SYSCALL_RESULT_FAILED;
        return;
    }

    frame->rax = SYSCALL_RESULT_OK;
    frame->rcx = message.word_count;
    frame->rdx = message.words[0];
    frame->rsi = message.words[1];
    frame->rdi = message.words[2];
    frame->r8 = message.words[3];
}

static void syscall_ipc_send_blocking(InterruptFrame *frame) {
    if (!frame) return;
    if (!frame->rcx || frame->rcx > IPC_MESSAGE_MAX_WORDS) {
        frame->rax = SYSCALL_RESULT_FAILED;
        return;
    }

    Process *process = syscall_current_process();

    if (!process) {
        frame->rax = SYSCALL_RESULT_FAILED;
        return;
    }

    IpcMessage message;

    k_memset(&message, 0, sizeof(message));

    message.word_count = (u32)frame->rcx;
    message.words[0] = frame->rdx;
    message.words[1] = frame->rsi;
    message.words[2] = frame->rdi;
    message.words[3] = frame->r8;

    bool sent = ipc_send_blocking(process, frame->rbx, &message);

    frame->rax = sent ? SYSCALL_RESULT_OK : SYSCALL_RESULT_FAILED;
}

InterruptFrame *syscall_dispatch(InterruptFrame *frame) {
    if (!frame) return 0;

    /* INT 0x80 is our Ring3 syscall boundary. */
    if (!interrupt_from_user(frame)) {
        frame->rax = SYSCALL_RESULT_INVALID;
        return frame;
    }

    switch (frame->rax) {
        case SYSCALL_THREAD_ID: {
            Thread *thread = thread_current();

            frame->rax = thread ? thread->id : 0;
            return frame;
        }

        case SYSCALL_IPC_TRY_SEND:
            syscall_ipc_try_send(frame);
            return frame;

        case SYSCALL_IPC_TRY_RECEIVE:
            syscall_ipc_try_receive(frame);
            return frame;

        case SYSCALL_IPC_RECEIVE_BLOCKING:
            syscall_ipc_receive_blocking(frame);
            return frame;

        case SYSCALL_IPC_SEND_BLOCKING:
            syscall_ipc_send_blocking(frame);
            return frame;

        case SYSCALL_THREAD_EXIT: {
            Thread *thread = thread_current();

            /* Reject transitional/manual Ring3 probes which are not owned by
             * the scheduler. The scheduler-private idle context is a valid
             * successor when this is the final ordinary runnable Thread. */
            if (!thread || !thread->id || !thread->on_run_queue || thread->state != THREAD_STATE_RUNNING ||
                !thread->process || thread->process->kernel || !scheduler_can_terminate_current()) {
                frame->rax = SYSCALL_RESULT_FAILED;
                return frame;
            }

            /* The task layer preserves siblings on a clean thread exit and
             * records/closes the domain only on its last live thread. It then
             * hands off without freeing this active stack or return frame. */
            return task_exit_current_from_interrupt(frame);
        }

        case JCOS_SYSCALL_CLOCK_TICKS:
            frame->rax = timer_ticks();
            return frame;

        case JCOS_SYSCALL_CLOCK_FREQUENCY:
            frame->rax = timer_frequency();
            return frame;

        case JCOS_SYSCALL_DISPLAY_INFO: {
            Process *process = syscall_current_process();
            void *resource = 0;
            CapabilityTable *caps = process ? process_capabilities(process) : 0;
            if (!caps || !capability_lookup_rights(caps, frame->rbx,
                    CAPABILITY_TYPE_DEVICE_RESOURCE, CAPABILITY_RIGHT_READ, &resource) ||
                resource != display_resource()) {
                frame->rax = SYSCALL_RESULT_FAILED;
                return frame;
            }
            frame->rax = SYSCALL_RESULT_OK;
            frame->rcx = framebuffer_width();
            frame->rdx = framebuffer_height();
            return frame;
        }

        case JCOS_SYSCALL_DISPLAY_PRESENT: {
            Process *process = syscall_current_process();
            frame->rax = display_present_user(process, frame->rbx, frame->rcx,
                (u32)frame->rdx, (u32)frame->rsi) ?
                SYSCALL_RESULT_OK : SYSCALL_RESULT_FAILED;
            return frame;
        }

        case JCOS_SYSCALL_AUDIO_INFO: {
            Process *process = syscall_current_process();
            void *resource = 0;
            CapabilityTable *caps = process ? process_capabilities(process) : 0;
            if (!caps || !capability_lookup_rights(caps, frame->rbx,
                    CAPABILITY_TYPE_DEVICE_RESOURCE, CAPABILITY_RIGHT_READ, &resource) ||
                resource != audio_resource()) {
                frame->rax = SYSCALL_RESULT_FAILED;
                return frame;
            }
            frame->rax = SYSCALL_RESULT_OK;
            frame->rcx = audio_sample_rate();
            frame->rdx = audio_submit_max();
            return frame;
        }

        case JCOS_SYSCALL_AUDIO_WRITE: {
            Process *process = syscall_current_process();
            frame->rax = audio_write_user(process, frame->rbx, frame->rcx,
                (u32)frame->rdx) ? SYSCALL_RESULT_OK : SYSCALL_RESULT_FAILED;
            return frame;
        }

        default:
            frame->rax = SYSCALL_RESULT_INVALID;
            return frame;
    }
}
