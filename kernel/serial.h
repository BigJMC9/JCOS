#ifndef JA_OS_SERIAL_H
#define JA_OS_SERIAL_H

#include "types.h"

#define SERIAL_TX_POLL_LIMIT 65536U

typedef struct {
    u64 transmitted_bytes;
    u64 dropped_bytes;
    u64 timeouts;
    bool stalled;
} SerialTxStats;

bool serial_init(void);
void serial_get_tx_stats(SerialTxStats *out);
bool serial_available(void);
void serial_putc(char c);
void serial_write(const char *s);
void serial_write_u64(u64 value);
void serial_write_hex(u64 value);
void serial_clear(void);
int serial_read_nonblocking(void);

#endif
