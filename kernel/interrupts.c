#include "interrupts.h"
#include "arch.h"
#include "lib.h"
#include "terminal.h"
#include "interrupt_controller.h"
#include "ps2.h"
#include "thread.h"
#include "syscall.h"

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
    g_spurious = 0;
    u64 table = (u64)(const void *)isr_stub_offsets;
    u16 selector = arch_read_cs();
    for (u32 vector = 0; vector < 48; ++vector) {

        u64 handler = table + (s64)isr_stub_offsets[vector];
        u8 ist = (tss_ready && vector == 8U) ? 1U : 0U;
        idt_set((u8)vector, handler, selector, ist, 0);
    }
    idt_set(0x80, table + (s64)isr_stub_offsets[48], selector, 0, 3);
    idt_set(0xFF, table + (s64)isr_stub_offsets[49], selector, 0, 0);
    g_idtr.limit = (u16)(sizeof(g_idt) - 1);
    g_idtr.base = (u64)(void *)g_idt;
    arch_load_idt(&g_idtr);
}

static const char *exception_name(u64 vector) {
    switch (vector) {
        case 0: return "DIVIDE ERROR";
        case 1: return "DEBUG";
        case 2: return "NON-MASKABLE INTERRUPT";
        case 3: return "BREAKPOINT";
        case 4: return "OVERFLOW";
        case 5: return "BOUND RANGE EXCEEDED";
        case 6: return "INVALID OPCODE";
        case 7: return "DEVICE NOT AVAILABLE";
        case 8: return "DOUBLE FAULT";
        case 10: return "INVALID TSS";
        case 11: return "SEGMENT NOT PRESENT";
        case 12: return "STACK-SEGMENT FAULT";
        case 13: return "GENERAL PROTECTION FAULT";
        case 14: return "PAGE FAULT";
        case 16: return "X87 FLOATING-POINT EXCEPTION";
        case 17: return "ALIGNMENT CHECK";
        case 18: return "MACHINE CHECK";
        case 19: return "SIMD FLOATING-POINT EXCEPTION";
        case 20: return "VIRTUALIZATION EXCEPTION";
        case 21: return "CONTROL PROTECTION EXCEPTION";
        case 29: return "VMM COMMUNICATION EXCEPTION";
        case 30: return "SECURITY EXCEPTION";
        default: return "RESERVED CPU EXCEPTION";
    }
}

static NORETURN void exception_panic(const InterruptFrame *frame) {
    interrupts_disable();
    terminal_set_color(terminal_error_color());
    terminal_writeln("\nCPU EXCEPTION:");
    terminal_write("VECTOR: "); terminal_write_u64(frame->vector);
    terminal_write(" ("); terminal_write(exception_name(frame->vector));
    terminal_writeln(")");
    terminal_write("ERROR CODE: "); terminal_write_hex(frame->error_code); terminal_putchar('\n');
    terminal_write("RIP: "); terminal_write_hex(frame->rip); terminal_putchar('\n');
    terminal_write("CS: "); terminal_write_hex(frame->cs); terminal_putchar('\n');
    terminal_write("RFLAGS: "); terminal_write_hex(frame->rflags); terminal_putchar('\n');
    bool from_user = interrupt_from_user(frame);
    terminal_write("ORIGIN: ");
    terminal_writeln(from_user ? "USER" : "KERNEL");
    if (from_user) {
        const InterruptStackFrame *user = interrupt_user_stack(frame);
        Thread *current = thread_current();
        terminal_write( "THREAD: ");
        if (current) {
            terminal_write_u64(current->id);
        } 
        else {
            terminal_write("NONE");
        }
        terminal_putchar('\n');
        terminal_write("RAX: ");
        terminal_write_hex(frame->rax);
        terminal_putchar('\n');
        if (user) {
            terminal_write("USER RSP: ");
            terminal_write_hex(user->rsp);
            terminal_putchar('\n');

            terminal_write("USER SS: ");
            terminal_write_hex(user->ss);
            terminal_putchar('\n');
        }
    }
    if (frame->vector == 14) {
        terminal_write("CR2: "); terminal_write_hex(arch_read_cr2()); terminal_putchar('\n');
    }
    terminal_writeln("CPU HALTED.");
    cpu_halt_forever();
}

void interrupt_dispatch(InterruptFrame *frame) {
    if (!frame) cpu_halt_forever();
    u8 vector = (u8)frame->vector;
    ++g_counts[vector];

    if (frame->vector < 32) exception_panic(frame);
    if (vector == SYSCALL_VECTOR) {
        /* INT 0x80 entered through the DPL3 syscall gate and already switched to TSS.RSP0. */
        syscall_dispatch(frame);
        return;
    }   
    if (vector == 0x21) {
        ps2_handle_irq();
        interrupt_controller_eoi(vector);
        return;
    }
    if (vector >= 0x20 && vector <= 0x2F) {
        interrupt_controller_eoi(vector);
        return;
    }
    if (vector == 0xFF) {
        ++g_spurious;
        return; /* A true local-APIC spurious interrupt receives no EOI. */
    }
}

void interrupts_enable(void) { arch_sti(); }
void interrupts_disable(void) { arch_cli(); }
u64 interrupt_count(u8 vector) { return g_counts[vector]; }
u64 interrupt_spurious_count(void) { return g_spurious; }
