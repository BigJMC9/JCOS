#include "syscall.h"
#include "thread.h"

void syscall_dispatch(InterruptFrame *frame) {
    if (!frame) return;
    switch (frame->rax) {
        case SYSCALL_THREAD_ID: {
            Thread *thread = thread_current();
            frame->rax = thread ? thread->id : 0;
            return;
        }
        default:
            frame->rax = SYSCALL_RESULT_INVALID;
            return;
    }
}