#include "input.h"

#include "interrupts.h"
#include "ps2.h"
#include "serial.h"
#include "usb_xhci.h"

typedef enum {
    SERIAL_PARSE_IDLE = 0,
    SERIAL_PARSE_ESCAPE,
    SERIAL_PARSE_CSI,
    SERIAL_PARSE_CSI_PARAMETER
} SerialParseState;

static SerialParseState g_serial_state;
static u32 g_serial_parameter;

#define INPUT_RFLAGS_IF (1ULL << 9)

static u64 input_irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void input_irq_restore(u64 flags) {
    if (flags & INPUT_RFLAGS_IF) interrupts_enable();
}

static void event_clear(KeyEvent *event) {
    event->key = KEY_NONE;
    event->character = 0;
    event->pressed = true;
    event->shift = false;
    event->ctrl = false;
    event->alt = false;
}

static bool serial_event_from_byte(u8 byte, KeyEvent *event) {
    event_clear(event);

    if (g_serial_state == SERIAL_PARSE_IDLE) {
        if (byte == 0x1BU) {
            g_serial_state = SERIAL_PARSE_ESCAPE;
            return false;
        }

        if (byte == '\r' || byte == '\n') {
            event->key = KEY_ENTER;
            return true;
        }

        if (byte == 0x08U || byte == 0x7FU) {
            event->key = KEY_BACKSPACE;
            return true;
        }

        if (byte == '\t') {
            event->key = KEY_TAB;
            return true;
        }

        if (byte >= 32U && byte <= 126U) {
            event->key = KEY_CHARACTER;
            event->character = (char)byte;
            return true;
        }

        return false;
    }

    if (g_serial_state == SERIAL_PARSE_ESCAPE) {
        if (byte == '[') {
            g_serial_state = SERIAL_PARSE_CSI;
            g_serial_parameter = 0;
            return false;
        }

        g_serial_state = SERIAL_PARSE_IDLE;
        event->key = KEY_ESCAPE;
        return true;
    }

    if (g_serial_state == SERIAL_PARSE_CSI || g_serial_state == SERIAL_PARSE_CSI_PARAMETER) {
        if (byte >= '0' && byte <= '9') {
            g_serial_state = SERIAL_PARSE_CSI_PARAMETER;
            g_serial_parameter = g_serial_parameter * 10U + (u32)(byte - '0');
            return false;
        }

        g_serial_state = SERIAL_PARSE_IDLE;

        switch (byte) {
            case 'A': event->key = KEY_UP; return true;
            case 'B': event->key = KEY_DOWN; return true;
            case 'C': event->key = KEY_RIGHT; return true;
            case 'D': event->key = KEY_LEFT; return true;
            case 'H': event->key = KEY_HOME; return true;
            case 'F': event->key = KEY_END; return true;
            case '~':
                if (g_serial_parameter == 1U || g_serial_parameter == 7U) event->key = KEY_HOME;
                else if (g_serial_parameter == 3U) event->key = KEY_DELETE;
                else if (g_serial_parameter == 4U || g_serial_parameter == 8U) event->key = KEY_END;
                else if (g_serial_parameter == 5U) event->key = KEY_PAGE_UP;
                else if (g_serial_parameter == 6U) event->key = KEY_PAGE_DOWN;
                else return false;
                return true;
            default:
                return false;
        }
    }

    g_serial_state = SERIAL_PARSE_IDLE;
    return false;
}

bool input_poll(KeyEvent *event) {
    if (!event) return false;

    u64 flags = input_irq_save();
    ps2_poll();
    bool have_ps2 = ps2_get_event(event);
    input_irq_restore(flags);

    if (have_ps2) return true;

    xhci_poll();
    if (xhci_get_event(event)) return true;

    for (u32 i = 0; i < 8U; ++i) {
        int value = serial_read_nonblocking();
        if (value < 0) return false;
        if (serial_event_from_byte((u8)value, event)) return true;
    }

    return false;
}
