#include "serial.h"
#include "arch.h"

#define COM1 0x3F8

static bool g_ready;
static SerialTxStats g_tx;

/* Current execution profile is UP. Keep the readiness check and one-byte
 * write atomic, but restore the caller's IRQ state between polling attempts. */
static u64 serial_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    arch_cli();
    return flags;
}

static void serial_irq_restore(u64 flags) {
    if (flags & (1ULL << 9U)) arch_sti();
}

void serial_get_tx_stats(SerialTxStats *out) {
    if (!out) return;
    u64 flags = serial_irq_save();
    out->transmitted_bytes = g_tx.transmitted_bytes;
    out->dropped_bytes = g_tx.dropped_bytes;
    out->timeouts = g_tx.timeouts;
    out->stalled = g_tx.stalled;
    serial_irq_restore(flags);
}

bool serial_init(void) {
    g_tx.transmitted_bytes = 0;
    g_tx.dropped_bytes = 0;
    g_tx.timeouts = 0;
    g_tx.stalled = false;
    arch_out8(COM1 + 1, 0x00); /* Disable UART interrupts. */
    arch_out8(COM1 + 3, 0x80); /* Divisor latch access. */
    arch_out8(COM1 + 0, 0x03); /* 38400 baud with a 115200 base clock. */
    arch_out8(COM1 + 1, 0x00);
    arch_out8(COM1 + 3, 0x03); /* 8 data bits, no parity, one stop bit. */
    arch_out8(COM1 + 2, 0xC7); /* FIFO on, clear queues, 14-byte threshold. */
    arch_out8(COM1 + 4, 0x0B); /* IRQs enabled at UART, RTS/DSR asserted. */

    arch_out8(COM1 + 7, 0x5A);
    g_ready = arch_in8(COM1 + 7) == 0x5A;
    return g_ready;
}

bool serial_available(void) {
    return g_ready;
}

static void put_raw(char c) {
    if (!g_ready) return;
    for (u32 attempt = 0; attempt < SERIAL_TX_POLL_LIMIT; ++attempt) {
        u64 flags = serial_irq_save();
        if (arch_in8(COM1 + 5) & 0x20U) {
            arch_out8(COM1, (u8)c);
            ++g_tx.transmitted_bytes;
            g_tx.stalled = false;
            serial_irq_restore(flags);
            return;
        }
        /* After one bounded timeout, avoid repeating a full busy wait for
         * every byte. A future ready observation resumes normal transmission. */
        if (g_tx.stalled || attempt + 1U == SERIAL_TX_POLL_LIMIT) {
            if (!g_tx.stalled) ++g_tx.timeouts;
            g_tx.stalled = true;
            ++g_tx.dropped_bytes;
            serial_irq_restore(flags);
            return;
        }
        serial_irq_restore(flags);
        arch_pause();
    }
}

void serial_putc(char c) {
    if (c == '\n') put_raw('\r');
    put_raw(c);
}

void serial_write(const char *s) {
    if (!s) return;
    while (*s) serial_putc(*s++);
}

void serial_write_u64(u64 value) {
    char digits[21];
    u32 length = 0;
    if (!value) {
        serial_putc('0');
        return;
    }
    while (value) {
        digits[length++] = (char)('0' + value % 10U);
        value /= 10U;
    }
    while (length) serial_putc(digits[--length]);
}

void serial_write_hex(u64 value) {
    static const char digits[] = "0123456789ABCDEF";
    serial_write("0x");
    for (u32 shift = 60U;; shift -= 4U) {
        serial_putc(digits[(value >> shift) & 0x0FU]);
        if (!shift) break;
    }
}

void serial_clear(void) {
    serial_write("\x1B[2J\x1B[H");
}

int serial_read_nonblocking(void) {
    if (!g_ready || !(arch_in8(COM1 + 5) & 0x01)) return -1;
    return (int)arch_in8(COM1);
}
