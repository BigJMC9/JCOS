#include "serial.h"
#include "arch.h"

#define COM1 0x3F8

static bool g_ready;

bool serial_init(void) {
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
    for (u32 i = 0; i < 1000000; ++i) {
        if (arch_in8(COM1 + 5) & 0x20) {
            arch_out8(COM1, (u8)c);
            return;
        }
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

void serial_clear(void) {
    serial_write("\x1B[2J\x1B[H");
}

int serial_read_nonblocking(void) {
    if (!g_ready || !(arch_in8(COM1 + 5) & 0x01)) return -1;
    return (int)arch_in8(COM1);
}
