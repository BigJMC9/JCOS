#include "interrupts.h"
#include "arch.h"
#include "lib.h"
#include "terminal.h"
#include "interrupt_controller.h"
#include "ps2.h"
#include "thread.h"
#include "syscall.h"
#include "scheduler.h"
#include "timer.h"
#include "gdt.h"
#include "task.h"

typedef struct PACKED {
    u16 offset_low;
    u16 selector;
    u8 ist;
    u8 type_attributes;
    u16 offset_middle;
    u32 offset_high;
    u32 zero;
} IdtEntry;

typedef struct PACKED {
    u16 limit;
    u64 base;
} IdtDescriptor;

static IdtEntry g_idt[256];
static IdtDescriptor g_idtr;
static u64 g_counts[256];
static u64 g_spurious;
static UserFaultInfo g_last_user_fault;

void interrupt_clear_user_fault(void) {
    k_memset(&g_last_user_fault, 0, sizeof(g_last_user_fault));
}

bool interrupt_last_user_fault(UserFaultInfo *info) {
    if (!info || !g_last_user_fault.valid) return false;
    *info = g_last_user_fault;

    return true;
}

bool interrupt_from_user(const InterruptFrame *frame) {
    if (!frame) return false;
    return(frame->cs & 3ULL) == 3ULL;
}

const InterruptStackFrame * interrupt_user_stack( const InterruptFrame *frame) {
    if (!interrupt_from_user(frame)) return 0;
    return (const InterruptStackFrame *) ((const u8 *)frame + sizeof(InterruptFrame));
}

static void idt_set(u8 vector, u64 handler, u16 selector, u8 ist, u8 dpl) {
    IdtEntry *entry = &g_idt[vector];
    entry->offset_low = (u16)handler;
    entry->selector = selector;
    entry->ist = ist & 7U;
    entry->type_attributes = (u8)(0x8EU | ((dpl & 3U) << 5));
    entry->offset_middle = (u16)(handler >> 16);
    entry->offset_high = (u32)(handler >> 32);
    entry->zero = 0;
}

void idt_init(bool tss_ready) {
    k_memset(g_idt, 0, sizeof(g_idt)); k_memset(g_counts, 0, sizeof(g_counts));
    k_memset(&g_last_user_fault, 0, sizeof(g_last_user_fault));
    g_spurious = 0;
    u64 table = (u64)(const void *)isr_stub_offsets;
    u16 selector = arch_read_cs();
    for (u32 vector = 0; vector < 48; ++vector) {

        u64 handler = table + (s64)isr_stub_offsets[vector];
        u8 ist = (tss_ready && vector == 8U) ? 1U : 0U;
        idt_set((u8)vector, handler, selector, ist, 0);
    }
    idt_set(0x80, table + (s64)isr_stub_offsets[48], selector, 0, 3); 
    idt_set(RESCHEDULE_VECTOR, table + (s64)isr_stub_offsets[49], selector, 0, 0);
    idt_set(0xFF, table + (s64)isr_stub_offsets[50], selector, 0, 0);
    g_idtr.limit = (u16)(sizeof(g_idt) - 1);
    g_idtr.base = (u64)(void *)g_idt;
    arch_load_idt(&g_idtr);
}

static __attribute__((optnone))
void exception_write_name(u64 vector) {
    switch (vector) {
        case 0: terminal_write("DIVIDE ERROR"); return;
        case 1: terminal_write("DEBUG"); return;
        case 2: terminal_write("NON-MASKABLE INTERRUPT"); return;
        case 3: terminal_write("BREAKPOINT"); return;
        case 4: terminal_write("OVERFLOW"); return;
        case 5: terminal_write("BOUND RANGE EXCEEDED"); return;
        case 6: terminal_write("INVALID OPCODE"); return;
        case 7: terminal_write("DEVICE NOT AVAILABLE"); return;
        case 8: terminal_write("DOUBLE FAULT"); return;
        case 10: terminal_write("INVALID TSS"); return;
        case 11: terminal_write("SEGMENT NOT PRESENT"); return;
        case 12: terminal_write("STACK-SEGMENT FAULT"); return;
        case 13: terminal_write("GENERAL PROTECTION FAULT"); return;
        case 14: terminal_write("PAGE FAULT"); return;
        case 16: terminal_write("X87 FLOATING-POINT EXCEPTION"); return;
        case 17: terminal_write("ALIGNMENT CHECK"); return;
        case 18: terminal_write("MACHINE CHECK"); return;
        case 19: terminal_write("SIMD FLOATING-POINT EXCEPTION"); return;
        case 20: terminal_write("VIRTUALIZATION EXCEPTION"); return;
        case 21: terminal_write("CONTROL PROTECTION EXCEPTION"); return;
        case 29: terminal_write("VMM COMMUNICATION EXCEPTION"); return;
        case 30: terminal_write("SECURITY EXCEPTION"); return;
        default: terminal_write("RESERVED CPU EXCEPTION"); return;
    }
}

static InterruptFrame *user_exception_terminate(InterruptFrame *frame) {
    interrupts_disable();

    Thread *thread = thread_current();

    if (!frame || !thread || !thread->id || !thread->on_run_queue ||
        thread->state != THREAD_STATE_RUNNING || !scheduler_can_terminate_current()) cpu_halt_forever();

    const InterruptStackFrame *user = interrupt_user_stack(frame);

    k_memset(&g_last_user_fault, 0, sizeof(g_last_user_fault));

    g_last_user_fault.valid = true;
    g_last_user_fault.thread_id = thread->id;
    g_last_user_fault.vector = frame->vector;
    g_last_user_fault.error_code = frame->error_code;
    g_last_user_fault.rip = frame->rip;
    g_last_user_fault.rax = frame->rax;

    if (user) {
        g_last_user_fault.user_rsp = user->rsp;
        g_last_user_fault.user_ss = user->ss;
    }

    if (frame->vector == 14ULL) g_last_user_fault.cr2 = arch_read_cr2();

    terminal_set_color(terminal_error_color());
    terminal_writeln("\nUSER FAULT:");
    terminal_write("VECTOR: "); 
    terminal_write_u64(frame->vector); 
    terminal_write(" (");
    exception_write_name(frame->vector);
    terminal_writeln(")");
    terminal_write("THREAD: "); terminal_write_u64(thread->id); terminal_putchar('\n'); 
    terminal_write("RIP: "); terminal_write_hex(frame->rip); terminal_putchar('\n');
    terminal_write("ERROR CODE: "); terminal_write_hex(frame->error_code); terminal_putchar('\n');

    if (user) { 
        terminal_write("USER RSP: "); terminal_write_hex(user->rsp); terminal_putchar('\n'); 
    }
    if (frame->vector == 14ULL) { 
        terminal_write("CR2: "); terminal_write_hex(g_last_user_fault.cr2); terminal_putchar('\n'); 
    }

    terminal_writeln("ACTION: PROCESS TERMINATED (RESOURCES RETAINED FOR REAP)");
    terminal_set_color(terminal_default_color());

    return task_fault_current_from_interrupt(frame, g_last_user_fault.cr2);
}

static NORETURN void exception_panic(const InterruptFrame *frame) {
    interrupts_disable();
    terminal_set_color(terminal_error_color());
    terminal_writeln("\nCPU EXCEPTION:");
    terminal_write("VECTOR: "); terminal_write_u64(frame->vector); terminal_write(" (");
    exception_write_name(frame->vector);
    terminal_writeln(")");
    terminal_write("ERROR CODE: "); terminal_write_hex(frame->error_code); terminal_putchar('\n');
    terminal_write("RIP: "); terminal_write_hex(frame->rip); terminal_putchar('\n');
    terminal_write("CS: "); terminal_write_hex(frame->cs); terminal_putchar('\n');
    terminal_write("RFLAGS: "); terminal_write_hex(frame->rflags); terminal_putchar('\n');
    Thread *fault_thread = thread_current();
    terminal_write("FRAME PTR: "); terminal_write_hex((u64)(const void *)frame); terminal_putchar('\n');
    terminal_write("CURRENT THREAD: ");
    if (fault_thread) terminal_write_u64(fault_thread->id);
    else terminal_write("NONE");
    terminal_putchar('\n');
    terminal_write("PREEMPTION: ");
    terminal_writeln(scheduler_preemption_enabled() ? "ENABLED" : "DISABLED");
    terminal_write("PREEMPTIONS: "); terminal_write_u64(scheduler_preemption_count()); terminal_putchar('\n');

    if (fault_thread) {
        terminal_write("THREAD STACK BASE: "); terminal_write_hex(fault_thread-> kernel_stack_base); terminal_putchar('\n');
        terminal_write("THREAD STACK TOP: "); terminal_write_hex(fault_thread-> kernel_stack_top); terminal_putchar('\n');
        terminal_write("THREAD INT RSP: "); terminal_write_hex(fault_thread-> interrupt_rsp); terminal_putchar('\n');
        terminal_write("THREAD INT READY: ");
        terminal_writeln(fault_thread-> interrupt_context_ready ? "YES" : "NO");
    }
    if (frame->vector == 8ULL) {
        /*
        * #DF uses IST1.
        *
        * An IST switch saves the previous RSP/SS
        * even though CS.RPL is still 0.
        */
        const InterruptStackFrame *old_stack = (const InterruptStackFrame *) ((const u8 *)frame + sizeof(InterruptFrame));

        terminal_write("DF IST1 NOW: "); terminal_write_hex(gdt_ist1()); terminal_putchar('\n');
        terminal_write("DF OLD RSP: "); terminal_write_hex(old_stack->rsp); terminal_putchar('\n');
        terminal_write("DF OLD SS: "); terminal_write_hex(old_stack->ss); terminal_putchar('\n');

        /* CR2 is only authoritative for #PF, but during this controlled test its value is useful for detecting whether the first exception was caused by a page access. */
        terminal_write("DF CR2 SNAPSHOT: "); terminal_write_hex(arch_read_cr2()); terminal_putchar('\n');

        if (fault_thread) {
            bool old_rsp_in_thread_stack = old_stack->rsp >= fault_thread->kernel_stack_base && old_stack->rsp <= fault_thread->kernel_stack_top;
            terminal_write("DF OLD RSP IN THREAD STACK: ");
            terminal_writeln(old_rsp_in_thread_stack ? "YES" : "NO");
        }
    }
    bool from_user = interrupt_from_user(frame);
    terminal_write("ORIGIN: ");
    terminal_writeln(from_user ? "USER" : "KERNEL");
    if (from_user) {
        const InterruptStackFrame *user = interrupt_user_stack(frame);
        terminal_write( "THREAD: ");
        if (fault_thread) terminal_write_u64(fault_thread->id);
        else terminal_write("NONE");
        terminal_putchar('\n'); terminal_write("RAX: "); terminal_write_hex(frame->rax); terminal_putchar('\n');
        if (user) {
            terminal_write("USER RSP: "); terminal_write_hex(user->rsp); terminal_putchar('\n');
            terminal_write("USER SS: "); terminal_write_hex(user->ss); terminal_putchar('\n');
        }
    }
    if (frame->vector == 14) {
        terminal_write("CR2: "); terminal_write_hex(arch_read_cr2()); terminal_putchar('\n');
    }
    terminal_writeln("CPU HALTED.");
    cpu_halt_forever();
}

InterruptFrame *interrupt_dispatch(InterruptFrame *frame) {
    if (!frame) cpu_halt_forever();
    u8 vector = (u8)frame->vector;
    ++g_counts[vector];

    if (frame->vector < 32) {
        if (interrupt_from_user(frame) && task_user_fault_supported(frame->vector)) {
            Thread *current = thread_current();

            /* Scheduler-managed user threads can be isolated even when the
             * scheduler-private idle context is their only successor. */
            if (current && current->process && !current->process->kernel && scheduler_can_terminate_current()) {
                return user_exception_terminate(frame);
            }
        }
        exception_panic(frame);
    }
    if (vector == SYSCALL_VECTOR) {
        return syscall_dispatch(frame);
    }
    if (vector == RESCHEDULE_VECTOR) {
        return scheduler_reschedule(frame);
    }
    if (vector == 0x20) {
        timer_handle_irq();
        interrupt_controller_eoi(vector);
        return scheduler_preempt(frame);
    }
    if (vector == 0x21) {
        ps2_handle_irq();
        interrupt_controller_eoi(vector);
        return frame;
    }
    if (vector >= 0x20 && vector <= 0x2F) {
        interrupt_controller_eoi(vector);
        return frame;
    }
    if (vector == 0xFF) {
        ++g_spurious;
        return frame; /* A true local-APIC spurious interrupt receives no EOI. */
    }
    return frame; /* Unhandled interrupt. The CPU will resume execution at the interrupted instruction. */
}

void interrupts_enable(void) { arch_sti(); }
void interrupts_disable(void) { arch_cli(); }
u64 interrupt_count(u8 vector) { return g_counts[vector]; }
u64 interrupt_spurious_count(void) { return g_spurious; }
