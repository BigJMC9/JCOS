#include "scheduler.h"
#include "arch.h"
#include "gdt.h"
#include "kernel_stack.h"
#include "lib.h"
#include "physmap.h"
#include "pmm.h"

#define SCHEDULER_RFLAGS_IF (1ULL << 9)
#define SCHEDULER_RFLAGS_FIXED (1ULL << 1)

typedef enum {
    SCHEDULER_TRAP_INVALID = 0,
    SCHEDULER_TRAP_YIELD = 1,
    SCHEDULER_TRAP_BLOCK = 2,
    SCHEDULER_TRAP_EXIT = 3
} SchedulerTrapOperation;

static Thread *g_run_head;
static Thread *g_run_tail;

static u64 g_run_count;
static bool g_initialized;

static u64 g_reschedule_count;
static bool g_preemption_enabled;
static u64 g_preemption_count;
static u64 g_idle_wait_count;

static u64 g_idle_stack_physical;
static u64 g_idle_stack_base;
static u64 g_idle_stack_top;
static bool g_idle_context_ready;
static bool g_idle_active;
static u64 g_idle_entry_count;
static u64 g_idle_resume_count;

static NORETURN void scheduler_idle_loop(void);

static bool scheduler_interrupt_frame_valid(const Thread *thread, const InterruptFrame *frame) {
    if (!thread || 
        !frame || 
        !thread->kernel_stack_base || 
        !thread->kernel_stack_top || 
        thread->kernel_stack_top <= thread->kernel_stack_base
    ) return false;

    const u64 frame_size = sizeof(InterruptFrame) + sizeof(InterruptStackFrame);
    if (thread->kernel_stack_size < frame_size) return false;

    u64 address = (u64)(const void *)frame;
    if (address < thread->kernel_stack_base) return false;
    if (address > thread->kernel_stack_top - frame_size) return false;

    return true;
}

static bool scheduler_idle_runtime_frame_valid(const InterruptFrame *frame) {
    if (!frame || !g_idle_context_ready || !g_idle_stack_base || !g_idle_stack_top ||
        g_idle_stack_top <= g_idle_stack_base) return false;
    u64 address = (u64)(const void *)frame;
    return address >= g_idle_stack_base && address <= g_idle_stack_top - sizeof(InterruptFrame);
}

static bool scheduler_idle_frame_build(u64 stack_base, u64 stack_top, InterruptFrame **out_frame) {
    if (out_frame) *out_frame = 0;
    if (!out_frame || !stack_base || stack_top <= stack_base || (stack_top & 0xFULL)) return false;

    const u64 frame_size = sizeof(InterruptFrame) + sizeof(InterruptStackFrame);
    if (stack_top - stack_base < frame_size + sizeof(u64)) return false;

    u64 anchor = stack_top - sizeof(u64);
    u64 frame_address = anchor - frame_size;
    if (frame_address < stack_base) return false;

    *(u64 *)(u64)anchor = 0;
    InterruptFrame *frame = (InterruptFrame *)(u64)frame_address;
    InterruptStackFrame *stack = (InterruptStackFrame *)((u8 *)frame + sizeof(InterruptFrame));
    k_memset(frame, 0, (usize)frame_size);

    frame->rip = (u64)(void *)scheduler_idle_loop;
    frame->cs = GDT_KERNEL_CODE_SELECTOR;
    frame->rflags = SCHEDULER_RFLAGS_FIXED;
    stack->rsp = anchor;
    stack->ss = GDT_KERNEL_DATA_SELECTOR;
    *out_frame = frame;
    return true;
}

static bool scheduler_idle_stack_init(void) {
    if (g_idle_context_ready || g_idle_stack_physical || g_idle_stack_base || g_idle_stack_top) return false;

    u64 physical = pmm_alloc_pages(THREAD_KERNEL_STACK_PAGES);
    if (!physical) return false;
    frame_t first = phys_to_frame(physical);
    if (first == FRAME_INVALID) return false;

    void *direct = phys_to_virt(physical);
    if (!direct) {
        (void)frame_free_range(first, THREAD_KERNEL_STACK_PAGES);
        return false;
    }
    k_memset(direct, 0, (usize)THREAD_KERNEL_STACK_SIZE);

    u64 base = 0;
    if (!kernel_stack_map_scheduler_idle(physical, &base)) {
        (void)frame_free_range(first, THREAD_KERNEL_STACK_PAGES);
        return false;
    }
    if (!base || base > ~0ULL - THREAD_KERNEL_STACK_SIZE) {
        (void)kernel_stack_discard_unpublished(physical, base);
        return false;
    }

    u64 top = base + THREAD_KERNEL_STACK_SIZE;
    InterruptFrame *probe = 0;
    if ((top & 0xFULL) || !scheduler_idle_frame_build(base, top, &probe) || !probe ||
        !kernel_stack_mapping_valid(physical, base)) {
        (void)kernel_stack_discard_unpublished(physical, base);
        return false;
    }

    g_idle_stack_physical = physical;
    g_idle_stack_base = base;
    g_idle_stack_top = top;
    g_idle_context_ready = true;
    return true;
}

static NORETURN void scheduler_idle_loop(void) {
    for (;;) {
        if (!g_idle_active) cpu_halt_forever();
        __asm__ volatile ("sti; hlt; cli" : : : "memory");
    }
}

bool scheduler_init(void) {
    if (g_initialized) return false;

    Thread *current = thread_current();
    if (!current || !current->id || current->state != THREAD_STATE_RUNNING || current->on_run_queue) return false;
    if (!scheduler_idle_stack_init()) return false;

    /* The bootstrap thread starts as the only runnable thread. */
    current->run_next = current;
    current->on_run_queue = true;

    g_run_head = current;
    g_run_tail = current;
    g_run_count = 1;
    g_initialized = true;
    g_preemption_enabled = false;
    g_preemption_count = 0;
    g_reschedule_count = 0;
    g_idle_wait_count = 0;
    g_idle_active = false;
    g_idle_entry_count = 0;
    g_idle_resume_count = 0;

    return true;
}

bool scheduler_add(Thread *thread) {
    if (!g_initialized || 
        !thread || 
        !thread->id || 
        thread->on_run_queue || 
        thread->state != THREAD_STATE_READY || 
        !thread->interrupt_context_ready || 
        !thread->interrupt_rsp
    ) return false;

    InterruptFrame *frame = (InterruptFrame *)(u64) thread->interrupt_rsp;

    if (!scheduler_interrupt_frame_valid(thread, frame)) return false;
    if (!g_run_count) {
        if (!g_idle_active || g_run_head || g_run_tail) return false;
        thread->run_next = thread;
        thread->on_run_queue = true;
        g_run_head = thread;
        g_run_tail = thread;
        g_run_count = 1;
        return true;
    }
    if (!g_run_head || !g_run_tail) return false;

    thread->run_next = g_run_head;
    g_run_tail->run_next = thread;
    g_run_tail = thread;
    thread->on_run_queue = true;
    ++g_run_count;

    return true;
}

static bool scheduler_unlink(Thread *thread) {
    if (!thread || !thread->on_run_queue || !g_run_head || !g_run_tail || !g_run_count) return false;
    Thread *previous = g_run_head;
    bool found = false;

    for (u64 i = 0; i < g_run_count; ++i) {
        if (!previous || !previous->run_next) return false;
        if (previous->run_next == thread) {

            found = true;
            break;
        }
        previous = previous->run_next;
    }

    if (!found) return false;
    if (g_run_count == 1) {
        if (g_run_head != thread || g_run_tail != thread) return false;
        g_run_head = 0;
        g_run_tail = 0;
    } else {
        previous->run_next = thread->run_next;
        if (g_run_head == thread) g_run_head = thread->run_next;
        if (g_run_tail == thread) g_run_tail = previous;
        g_run_tail->run_next = g_run_head;
    }

    thread->run_next = 0;
    thread->on_run_queue = false;
    --g_run_count;

    return true;
}

static bool scheduler_find_next_interrupt_thread(Thread *current, Thread **out_thread, InterruptFrame **out_frame) {
    if (!current || 
        !out_thread || 
        !out_frame || 
        !current->run_next || 
        g_run_count < 2
    )   return false;

    Thread *candidate = current->run_next;
    for (u64 inspected = 0; inspected + 1ULL < g_run_count; ++inspected) {
        if (!candidate) return false;
        if (candidate != current && candidate->on_run_queue && candidate->state == THREAD_STATE_READY && candidate-> interrupt_context_ready && candidate->interrupt_rsp) {
            InterruptFrame *saved = (InterruptFrame *)(u64) candidate-> interrupt_rsp;
            if (scheduler_interrupt_frame_valid(candidate, saved)) {
                *out_thread = candidate;
                *out_frame = saved;
                return true;
            }
        }
        candidate = candidate->run_next;
    }
    return false;
}

static bool scheduler_find_idle_ready(Thread **out_thread, InterruptFrame **out_frame) {
    if (!out_thread || !out_frame || !g_idle_active || !g_run_head || !g_run_tail || !g_run_count) return false;
    Thread *candidate = g_run_head;
    for (u64 inspected = 0; inspected < g_run_count; ++inspected) {
        if (!candidate || !candidate->run_next) return false;
        if (candidate->on_run_queue && candidate->state == THREAD_STATE_READY &&
            candidate->interrupt_context_ready && candidate->interrupt_rsp) {
            InterruptFrame *saved = (InterruptFrame *)(u64)candidate->interrupt_rsp;
            if (scheduler_interrupt_frame_valid(candidate, saved)) {
                *out_thread = candidate;
                *out_frame = saved;
                return true;
            }
        }
        candidate = candidate->run_next;
    }
    return false;
}

bool scheduler_remove(Thread *thread) {
    if (!g_initialized || !thread || !thread->on_run_queue) return false;
    if (thread == thread_current()) return false;
    return scheduler_unlink(thread);
}

bool scheduler_yield(void) {
    if (!g_initialized || !g_run_head || !g_run_tail || !g_run_count) return false;

    Thread *current = thread_current();
    if (!current || 
        !current->id || 
        !current->on_run_queue || 
        current->state != THREAD_STATE_RUNNING || 
        !current->run_next
    )   return false;

    /* Yielding with nobody else runnable is a successful no-op. */
    if (g_run_count == 1) return true;
    return arch_reschedule_interrupt(SCHEDULER_TRAP_YIELD) != 0;
}

bool scheduler_block_current(void) {
    if (!g_initialized || !g_run_head || !g_run_tail || g_run_count < 2) return false;

    Thread *current = thread_current();
    if (!current || 
        !current->id || 
        !current->on_run_queue || 
        current->state != THREAD_STATE_RUNNING || 
        !current->run_next
    )   return false;

    /* On success this INT does not return until scheduler_wake() makes us READY and the scheduler selects this saved frame again. */
    return arch_reschedule_interrupt(SCHEDULER_TRAP_BLOCK) != 0;
}

bool scheduler_wait_current(void) {
    if (!g_initialized || !g_run_head || !g_run_tail || !g_run_count) return false;

    u64 flags = 0;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    arch_cli();

    Thread *current = thread_current();
    bool valid = current && current->id && current->on_run_queue &&
        current->state == THREAD_STATE_RUNNING && current->run_next;
    bool result = false;

    if (valid && g_run_count > 1ULL) {
        result = scheduler_block_current();
    } else if (valid && g_run_count == 1ULL && g_run_head == current && g_run_tail == current &&
        current->run_next == current) {
        /*
         * No other runnable context exists. Keep the current kernel continuation
         * intact and idle the CPU for one interrupt. STI;HLT is race-free on
         * x86: interrupts are recognized only after the following HLT begins.
         * The caller must re-check its blocking predicate after we return.
         */
        ++g_idle_wait_count;
        __asm__ volatile ("sti; hlt; cli" : : : "memory");
        result = true;
    }

    if (flags & SCHEDULER_RFLAGS_IF) arch_sti();
    return result;
}

u64 scheduler_idle_wait_count(void) {
    return g_initialized ? g_idle_wait_count : 0;
}

bool scheduler_idle_context_ready(void) {
    return g_initialized && g_idle_context_ready && g_idle_stack_physical && g_idle_stack_base &&
        g_idle_stack_top == g_idle_stack_base + THREAD_KERNEL_STACK_SIZE;
}

bool scheduler_idle_active(void) {
    return g_initialized && g_idle_active;
}

bool scheduler_idle_stack_guarded(void) {
    return scheduler_idle_context_ready() &&
        kernel_stack_mapping_valid(g_idle_stack_physical, g_idle_stack_base);
}

u64 scheduler_idle_entry_count(void) {
    return g_initialized ? g_idle_entry_count : 0;
}

u64 scheduler_idle_resume_count(void) {
    return g_initialized ? g_idle_resume_count : 0;
}

bool scheduler_wake(Thread *thread) {
    if (!g_initialized || 
        !thread || 
        !thread->id || 
        thread == thread_current() || 
        thread->on_run_queue || 
        thread->run_next || 
        !thread-> interrupt_context_ready || 
        !thread->interrupt_rsp || 
        thread->state != THREAD_STATE_BLOCKED
    )   return false;

    InterruptFrame *frame = (InterruptFrame *)(u64) thread->interrupt_rsp;
    if (!scheduler_interrupt_frame_valid(thread, frame)) return false;
    thread->state = THREAD_STATE_READY;
    if (!scheduler_add(thread)) {
        thread->state = THREAD_STATE_BLOCKED;
        return false;
    }
    return true;
}

u64 scheduler_thread_count(void) {
    return g_initialized ? g_run_count : 0;
}

bool scheduler_can_terminate_current(void) {
    if (!g_initialized || g_idle_active || !g_run_count || !g_run_head || !g_run_tail) return false;
    Thread *current = thread_current();
    if (!current || !current->id || !current->on_run_queue || current->state != THREAD_STATE_RUNNING ||
        !current->run_next) return false;

    if (g_run_count == 1ULL) {
        return scheduler_idle_context_ready() && scheduler_idle_stack_guarded() &&
            g_run_head == current && g_run_tail == current && current->run_next == current;
    }

    Thread *chosen = 0;
    InterruptFrame *chosen_frame = 0;
    return scheduler_find_next_interrupt_thread(current, &chosen, &chosen_frame);
}

bool scheduler_preemption_enable(void) {
    if (!g_initialized ||
        g_preemption_enabled ||
        g_idle_active ||
        !g_run_head ||
        !g_run_tail ||
        !g_run_count
    )    return false;

    Thread *current = thread_current();
    if (!current || 
        !current->id || 
        !current->on_run_queue || 
        current->state != THREAD_STATE_RUNNING
    )   return false;

    bool saw_current = false;
    Thread *thread = g_run_head;
    /*
     * Every non-running thread must already
     * have a valid interrupt-return context.
     *
     * The running thread does not need one:
     * its first PIT interrupt creates it.
     */
    for (u64 i = 0; i < g_run_count; ++i) {
        if (!thread || !thread->id || !thread->on_run_queue || !thread->run_next) return false;
        if (thread == current) {
            if (saw_current || thread->state != THREAD_STATE_RUNNING) return false;

            saw_current = true;
        } 
        else {
            if (thread->state != THREAD_STATE_READY || !thread-> interrupt_context_ready || !thread->interrupt_rsp) return false;
            InterruptFrame *frame = (InterruptFrame *)(u64) thread->interrupt_rsp;
            if (!scheduler_interrupt_frame_valid(thread, frame)) return false;
        }
        thread = thread->run_next;
    }
    if (!saw_current) return false;
    g_preemption_enabled = true;
    return true;
}

bool scheduler_preemption_disable(void) {
    if (!g_initialized || !g_preemption_enabled) return false;
    g_preemption_enabled = false;
    return true;
}

bool scheduler_preemption_enabled(void) {
    return g_initialized && g_preemption_enabled;
}

u64 scheduler_preemption_count(void) {
    return g_initialized ? g_preemption_count : 0;
}

static InterruptFrame *scheduler_switch_ready_from_interrupt(InterruptFrame *frame, bool timer_preemption) {
    if (!frame || 
        !g_initialized || 
        !g_run_head || 
        !g_run_tail || 
        g_run_count < 2
    )   return frame;

    Thread *current = thread_current();
    if (!current || 
        !current->id || 
        !current->on_run_queue || 
        current->state != THREAD_STATE_RUNNING || 
        !current->run_next
    )    return frame;
    if (!scheduler_interrupt_frame_valid(current, frame)) return frame;

    Thread *chosen = 0;
    InterruptFrame *chosen_frame = 0;

    if (!scheduler_find_next_interrupt_thread(current, &chosen, &chosen_frame)) {
        /* A software yield with nobody eligible is still a successful no-op. */
        if (!timer_preemption) frame->rax = 1;
        return frame;
    }

    if (!timer_preemption) frame->rax = 1;
    current->interrupt_rsp = (u64)(void *)frame;
    current->interrupt_context_ready = true;

    if (!thread_activate(chosen)) {
        current->interrupt_rsp = 0;
        current->interrupt_context_ready = false;
        if (!timer_preemption) frame->rax = 0;
        return frame;
    }

    /* chosen_frame is now being consumed. */
    chosen->interrupt_rsp = 0;
    chosen->interrupt_context_ready = false;

    if (timer_preemption) ++g_preemption_count;
    else ++g_reschedule_count;

    return chosen_frame;
}

static InterruptFrame *scheduler_block_from_interrupt(InterruptFrame *frame) {
    if (!frame) return frame;
    frame->rax = 0;

    if (!g_initialized || !g_run_head || !g_run_tail || g_run_count < 2) return frame;

    Thread *blocked = thread_current();
    if (!blocked || !blocked->id || !blocked->on_run_queue || blocked->state != THREAD_STATE_RUNNING || !blocked->run_next || !scheduler_interrupt_frame_valid(blocked, frame)) return frame;

    Thread *chosen = 0;
    InterruptFrame *chosen_frame = 0;
    if (!scheduler_find_next_interrupt_thread(blocked, &chosen, &chosen_frame)) return frame;

    frame->rax = 1;
    blocked->interrupt_rsp = (u64)(void *)frame;
    blocked->interrupt_context_ready = true;

    /* thread_activate temporarily changes blocked RUNNING -> READY. */
    if (!thread_activate(chosen)) {
        blocked->interrupt_rsp = 0;
        blocked->interrupt_context_ready = false;
        frame->rax = 0;
        return frame;
    }

    /* We have already changed architectural current to chosen. Failure here is an internal scheduler invariant failure. */
    if (!scheduler_unlink(blocked)) cpu_halt_forever();

    blocked->state = THREAD_STATE_BLOCKED;
    chosen->interrupt_rsp = 0;
    chosen->interrupt_context_ready = false;
    ++g_reschedule_count;
    return chosen_frame;
}

static InterruptFrame *scheduler_resume_from_idle(InterruptFrame *frame) {
    if (!frame || !g_idle_active) return frame;
    if (!scheduler_idle_runtime_frame_valid(frame)) cpu_halt_forever();
    if (!g_run_count) return frame;

    Thread *chosen = 0;
    InterruptFrame *chosen_frame = 0;
    if (!scheduler_find_idle_ready(&chosen, &chosen_frame)) cpu_halt_forever();
    if (!thread_scheduler_activate_from_idle(chosen)) cpu_halt_forever();

    chosen->interrupt_rsp = 0;
    chosen->interrupt_context_ready = false;
    g_idle_active = false;
    ++g_idle_resume_count;
    ++g_reschedule_count;
    return chosen_frame;
}

static InterruptFrame *scheduler_exit_from_interrupt(InterruptFrame *frame, bool count_reschedule) {
    if (!frame || 
        !g_initialized || 
        !g_run_head || 
        !g_run_tail || 
        !g_run_count
    )   cpu_halt_forever();

    Thread *dying = thread_current();
    if (!dying || 
        !dying->id || 
        !dying->on_run_queue || 
        dying->state != THREAD_STATE_RUNNING || 
        !dying->run_next || 
        !scheduler_interrupt_frame_valid(dying, frame)
    )    cpu_halt_forever();

    if (g_run_count == 1ULL) {
        if (!scheduler_can_terminate_current()) cpu_halt_forever();

        InterruptFrame *idle_frame = 0;
        if (!scheduler_idle_frame_build(g_idle_stack_base, g_idle_stack_top, &idle_frame) || !idle_frame) {
            cpu_halt_forever();
        }
        if (!scheduler_unlink(dying)) cpu_halt_forever();

        dying->state = THREAD_STATE_DEAD;
        dying->interrupt_rsp = 0;
        dying->interrupt_context_ready = false;

        if (!thread_scheduler_enter_idle(g_idle_stack_top)) cpu_halt_forever();
        g_idle_active = true;
        ++g_idle_entry_count;
        if (count_reschedule) ++g_reschedule_count;
        return idle_frame;
    }

    Thread *chosen = 0;
    InterruptFrame *chosen_frame = 0;

    if (!scheduler_find_next_interrupt_thread(dying, &chosen, &chosen_frame)) cpu_halt_forever();

    /* The dying frame is deliberately NOT retained as a resumable context. */
    if (!thread_activate(chosen)) cpu_halt_forever();
    if (!scheduler_unlink(dying)) cpu_halt_forever();

    dying->state = THREAD_STATE_DEAD;
    dying->interrupt_rsp = 0;
    dying->interrupt_context_ready = false;

    chosen->interrupt_rsp = 0;
    chosen->interrupt_context_ready = false;

    if (count_reschedule) ++g_reschedule_count;
    
    return chosen_frame;
}

InterruptFrame *scheduler_reschedule(InterruptFrame *frame) {
    if (!frame) return 0;
    SchedulerTrapOperation operation = (SchedulerTrapOperation) frame->rax;

    switch (operation) {
        case SCHEDULER_TRAP_YIELD:
            return scheduler_switch_ready_from_interrupt(frame, false);
        case SCHEDULER_TRAP_BLOCK:
            return scheduler_block_from_interrupt(frame);
        case SCHEDULER_TRAP_EXIT:
            return scheduler_exit_from_interrupt(frame, true);
        case SCHEDULER_TRAP_INVALID:
        default:
            frame->rax = 0;
            return frame;
    }
}

InterruptFrame *scheduler_preempt(InterruptFrame *frame) {
    if (g_idle_active) return scheduler_resume_from_idle(frame);
    if (!frame || !g_preemption_enabled) return frame;

    return scheduler_switch_ready_from_interrupt(frame, true);
}

u64 scheduler_reschedule_count(void) {
    return g_initialized ? g_reschedule_count : 0;
}

bool scheduler_can_terminate_thread(const Thread *thread) {
    if (!g_initialized || !thread || thread == thread_current() || !thread->id) {
        return false;
    }

    if (thread->state == THREAD_STATE_READY) {
        if (thread->on_run_queue) {
            if (!thread->run_next || !thread->interrupt_context_ready || !thread->interrupt_rsp) {
                return false;
            }
            return true;
        }
        return thread->run_next == 0;
    }

    if (thread->state == THREAD_STATE_BLOCKED) {
        return (
            !thread->on_run_queue &&
            !thread->run_next &&
            thread->interrupt_context_ready &&
            thread->interrupt_rsp
        );
    }
    return false;
}

bool scheduler_terminate_thread(Thread *thread) {
    if (!scheduler_can_terminate_thread(thread)) return false;

    /* Another kernel object must not retain a reference to this Thread. */
    if (thread_wait_active(thread)) return false;
    if (thread->state == THREAD_STATE_READY && thread->on_run_queue) {
        if (!scheduler_unlink(thread)) return false;
    }

    thread->state = THREAD_STATE_DEAD;
    thread->interrupt_rsp = 0;
    thread->interrupt_context_ready = false;
    thread->run_next = 0;
    thread->on_run_queue = false;
    return true;
}

InterruptFrame *scheduler_terminate_current_from_interrupt(InterruptFrame *frame) {
    return scheduler_exit_from_interrupt(frame, false);
}

NORETURN void scheduler_exit_current(void) {
    if (!scheduler_can_terminate_current()) cpu_halt_forever();

    Thread *current = thread_current();

    if (!current || 
        !current->id || 
        !current->on_run_queue || 
        current->state != THREAD_STATE_RUNNING
    )    cpu_halt_forever();

    /* Successful EXIT never restores this frame, so this call never returns. */
    (void)arch_reschedule_interrupt(SCHEDULER_TRAP_EXIT);
    cpu_halt_forever();
}