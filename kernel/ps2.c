#include "ps2.h"
#include "arch.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define STATUS_OUTPUT_FULL 0x01
#define STATUS_INPUT_FULL  0x02
#define STATUS_AUX_DATA    0x20

#define QUEUE_SIZE 256U

static volatile u8 g_queue[QUEUE_SIZE];
static volatile u32 g_head;
static volatile u32 g_tail;
static bool g_present;
static bool g_initialized;
static bool g_left_shift;
static bool g_right_shift;
static bool g_caps_lock;
static bool g_extended;
static u64 g_irq_count;
static u64 g_scancode_count;
static u64 g_dropped_count;

static const char unshifted[128] = {
    [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',[0x07]='6',[0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',
    [0x0C]='-',[0x0D]='=',[0x0E]='\b',[0x0F]='\t',
    [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',[0x15]='y',[0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',[0x1A]='[',[0x1B]=']',[0x1C]='\n',
    [0x1E]='a',[0x1F]='s',[0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',[0x24]='j',[0x25]='k',[0x26]='l',[0x27]=';',[0x28]='\'',[0x29]='`',
    [0x2B]='\\',[0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',[0x31]='n',[0x32]='m',[0x33]=',',[0x34]='.',[0x35]='/',
    [0x39]=' '
};

static const char shifted[128] = {
    [0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',[0x07]='^',[0x08]='&',[0x09]='*',[0x0A]='(',[0x0B]=')',
    [0x0C]='_',[0x0D]='+',[0x0E]='\b',[0x0F]='\t',
    [0x10]='Q',[0x11]='W',[0x12]='E',[0x13]='R',[0x14]='T',[0x15]='Y',[0x16]='U',[0x17]='I',[0x18]='O',[0x19]='P',[0x1A]='{',[0x1B]='}',[0x1C]='\n',
    [0x1E]='A',[0x1F]='S',[0x20]='D',[0x21]='F',[0x22]='G',[0x23]='H',[0x24]='J',[0x25]='K',[0x26]='L',[0x27]=':',[0x28]='"',[0x29]='~',
    [0x2B]='|',[0x2C]='Z',[0x2D]='X',[0x2E]='C',[0x2F]='V',[0x30]='B',[0x31]='N',[0x32]='M',[0x33]='<',[0x34]='>',[0x35]='?',
    [0x39]=' '
};

static bool wait_input_empty(void) {
    for (u32 i = 0; i < 200000; ++i) {
        if (!(arch_in8(PS2_STATUS) & STATUS_INPUT_FULL)) return true;
        arch_pause();
    }
    return false;
}

static bool wait_output_full(void) {
    for (u32 i = 0; i < 200000; ++i) {
        if (arch_in8(PS2_STATUS) & STATUS_OUTPUT_FULL) return true;
        arch_pause();
    }
    return false;
}

static bool controller_command(u8 command) {
    if (!wait_input_empty()) return false;
    arch_out8(PS2_CMD, command);
    return true;
}

static bool controller_write_data(u8 value) {
    if (!wait_input_empty()) return false;
    arch_out8(PS2_DATA, value);
    return true;
}

static bool controller_read_data(u8 *value) {
    if (!wait_output_full()) return false;
    *value = arch_in8(PS2_DATA);
    return true;
}

static bool keyboard_command(u8 command) {
    if (!controller_write_data(command)) return false;
    u8 response = 0;
    if (!controller_read_data(&response)) return false;
    if (response == 0xFE) { /* Resend once. */
        if (!controller_write_data(command) || !controller_read_data(&response)) return false;
    }
    return response == 0xFA;
}

static void enqueue(char c) {
    if (!c) return;
    u32 next = (g_head + 1U) & (QUEUE_SIZE - 1U);
    if (next == g_tail) {
        ++g_dropped_count;
        return;
    }
    g_queue[g_head] = (u8)c;
    g_head = next;
}

static void decode_scancode(u8 byte) {
    ++g_scancode_count;
    if (byte == 0xE0) { g_extended = true; return; }
    if (byte == 0xE1) { g_extended = false; return; }

    bool released = (byte & 0x80U) != 0;
    u8 code = byte & 0x7FU;
    if (!g_extended) {
        if (code == 0x2A) { g_left_shift = !released; return; }
        if (code == 0x36) { g_right_shift = !released; return; }
        if (code == 0x3A && !released) { g_caps_lock = !g_caps_lock; return; }
    }
    if (released) { g_extended = false; return; }

    if (g_extended) {
        if (code == 0x1C) enqueue('\n');
        else if (code == 0x35) enqueue('/');
        g_extended = false;
        return;
    }

    bool shift = g_left_shift || g_right_shift;
    char c = shift ? shifted[code] : unshifted[code];
    if (c >= 'a' && c <= 'z' && g_caps_lock) c = (char)(c - 'a' + 'A');
    else if (c >= 'A' && c <= 'Z' && g_caps_lock) c = (char)(c - 'A' + 'a');
    enqueue(c);
}

static void drain_controller(void) {
    for (u32 i = 0; i < 32; ++i) {
        u8 status = arch_in8(PS2_STATUS);
        if (!(status & STATUS_OUTPUT_FULL)) break;
        u8 value = arch_in8(PS2_DATA);
        if (!(status & STATUS_AUX_DATA)) decode_scancode(value);
    }
}

bool ps2_init(void) {
    g_head = g_tail = 0;
    g_left_shift = g_right_shift = g_caps_lock = g_extended = false;
    g_irq_count = g_scancode_count = g_dropped_count = 0;
    g_present = false;
    g_initialized = false;

    if (!controller_command(0xAD)) return false; /* Disable first port. */
    (void)controller_command(0xA7);              /* Disable second port if present. */
    for (u32 i = 0; i < 32 && (arch_in8(PS2_STATUS) & STATUS_OUTPUT_FULL); ++i)
        (void)arch_in8(PS2_DATA);

    if (!controller_command(0x20)) return false;
    u8 config = 0;
    if (!controller_read_data(&config)) return false;
    config &= (u8)~0x03U; /* No controller IRQs while configuring. */
    config &= (u8)~0x10U; /* Enable first-port clock. */
    config |= 0x40U;      /* Translate keyboard set 2 into set 1. */
    if (!controller_command(0x60) || !controller_write_data(config)) return false;
    if (!controller_command(0xAE)) return false;

    /* Enable keyboard scanning. Some USB legacy emulation does not ACK this,
       so lack of ACK does not prevent the polling fallback from operating. */
    bool acknowledged = keyboard_command(0xF4);

    config |= 0x01U; /* Enable first-port IRQ delivery. */
    if (controller_command(0x60)) (void)controller_write_data(config);
    g_present = acknowledged || arch_in8(PS2_STATUS) != 0xFF;
    g_initialized = g_present;
    return g_present;
}

void ps2_handle_irq(void) {
    ++g_irq_count;
    if (g_initialized) drain_controller();
}

void ps2_poll(void) {
    if (g_initialized) drain_controller();
}

int ps2_getchar(void) {
    if (g_tail == g_head) return -1;
    u8 value = g_queue[g_tail];
    g_tail = (g_tail + 1U) & (QUEUE_SIZE - 1U);
    return value;
}

bool ps2_present(void) { return g_present; }
u64 ps2_irq_count(void) { return g_irq_count; }
u64 ps2_scancode_count(void) { return g_scancode_count; }
u64 ps2_dropped_count(void) { return g_dropped_count; }
