/* Hardware-free tests link the actual UART implementation with port doubles. */
#include <stdio.h>
#include <string.h>
#include "../../kernel/serial.h"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "UART line %d: %s\n", __LINE__, #x); return 1; } } while (0)
static u8 output[1024];
static u32 output_size, polls, pause_count, cli_count, sti_count;
static u32 busy_reads;
static u8 scratch;
static bool absent, stuck, irq_disabled;

void arch_cli(void) { ++cli_count; irq_disabled = true; }
void arch_sti(void) { ++sti_count; irq_disabled = false; }
void arch_pause(void) { ++pause_count; }

u8 arch_in8(u16 port) {
    if (port == 0x3FFU) return absent ? 0xFFU : scratch;
    if (port == 0x3FDU) {
        ++polls;
        if (!irq_disabled) { fputs("UART polling not atomic\n", stderr); return 0; }
        if (stuck) return 0;
        if (busy_reads) { --busy_reads; return 0; }
        return 0x20U;
    }
    return 0;
}
void arch_out8(u16 port, u8 value) {
    if (port == 0x3FFU) scratch = value;
    /* Divisor initialization writes 3 to data register; do not record it. */
    if (port == 0x3F8U && value != 3U && output_size < sizeof(output))
        output[output_size++] = value;
}
static int reset(void) {
    output_size = polls = pause_count = cli_count = sti_count = busy_reads = 0;
    absent = stuck = irq_disabled = false;
    return serial_init();
}
int main(void) {
    SerialTxStats stats;
    CHECK(reset());
    serial_write("abc\n");
    CHECK(output_size == 5U && !memcmp(output, "abc\r\n", 5U));
    serial_get_tx_stats(&stats);
    CHECK(stats.transmitted_bytes == 5U && !stats.dropped_bytes && !stats.timeouts && !stats.stalled);
    CHECK(cli_count == sti_count && !irq_disabled);

    CHECK(reset());
    busy_reads = 17U;
    serial_putc('Z');
    CHECK(output_size == 1U && output[0] == 'Z' && polls == 18U && pause_count == 17U);
    serial_get_tx_stats(&stats);
    CHECK(stats.transmitted_bytes == 1U && !stats.dropped_bytes);
    CHECK(cli_count == sti_count && !irq_disabled);

    CHECK(reset());
    stuck = true;
    serial_write("abc");
    CHECK(polls == SERIAL_TX_POLL_LIMIT + 2U && pause_count == SERIAL_TX_POLL_LIMIT - 1U);
    serial_get_tx_stats(&stats);
    CHECK(!stats.transmitted_bytes && stats.dropped_bytes == 3U && stats.timeouts == 1U && stats.stalled);
    stuck = false;
    serial_putc('R');
    serial_get_tx_stats(&stats);
    CHECK(output_size == 1U && output[0] == 'R' && stats.transmitted_bytes == 1U &&
        stats.dropped_bytes == 3U && stats.timeouts == 1U && !stats.stalled);
    CHECK(cli_count == sti_count && !irq_disabled);

    CHECK(reset());
    busy_reads = SERIAL_TX_POLL_LIMIT - 1U;
    serial_putc('B');
    serial_get_tx_stats(&stats);
    CHECK(output_size == 1U && output[0] == 'B' && !stats.timeouts && !stats.dropped_bytes);

    absent = true;
    CHECK(!serial_init() && !serial_available());
    polls = 0;
    serial_write("not present");
    CHECK(!polls);
    serial_get_tx_stats(&stats);
    CHECK(!stats.transmitted_bytes && !stats.timeouts && !stats.dropped_bytes);
    puts("SERIAL TX: PASS (CRLF, delayed ready, bounded stall, fast drops, recovery, last attempt, absent UART)");
    return 0;
}
