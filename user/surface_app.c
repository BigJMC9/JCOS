#include "lib/syscall.h"
#include "../include/display_service_protocol.h"
#include "../include/program_startup.h"

#define SURFACE_APP_MODE_INVALID  0
#define SURFACE_APP_MODE_PRESENT  1
#define SURFACE_APP_MODE_PRODUCER 2
#define SURFACE_APP_MODE_INPUT    3
#define SURFACE_APP_MODE_SESSION  4

#define SESSION_COLUMNS 10U
#define SESSION_ROWS 6U
#define SESSION_TEXT_Y 8U
#define SESSION_CELL_WIDTH 6U
#define SESSION_CELL_HEIGHT 8U
#define SESSION_STATUS_Y 56U

#define GLYPH(a,b,c,d,e,f,g) \
    ((JcosU64)(a) | ((JcosU64)(b) << 5) | ((JcosU64)(c) << 10) | \
     ((JcosU64)(d) << 15) | ((JcosU64)(e) << 20) | ((JcosU64)(f) << 25) | \
     ((JcosU64)(g) << 30))

static void clear_message(JcosIpcMessage *message) {
    if (!message) return;
    for (JcosU32 i = 0U; i < JCOS_IPC_MESSAGE_MAX_WORDS; ++i) message->words[i] = 0ULL;
    message->word_count = 0U;
}

static int startup_mode(const JcosProgramStartup *startup) {
    if (!startup || startup->magic != JCOS_PROGRAM_STARTUP_MAGIC ||
        startup->version != JCOS_PROGRAM_STARTUP_VERSION ||
        startup->size != sizeof(JcosProgramStartup) ||
        startup->flags != JCOS_PROGRAM_STARTUP_FLAG_NONE ||
        startup->environment_count != 0U) return SURFACE_APP_MODE_INVALID;

    if (startup->capability_count == 2U && startup->argument_count == 1U &&
        startup->arguments[0] != 0ULL &&
        startup->capabilities[0] != JCOS_CAPABILITY_INVALID_HANDLE &&
        startup->capabilities[1] != JCOS_CAPABILITY_INVALID_HANDLE)
        return SURFACE_APP_MODE_PRESENT;

    if (startup->capability_count == 0U && startup->argument_count == 2U &&
        startup->arguments[0] == JCOS_DISPLAY_SURFACE_PRODUCER_COOKIE &&
        startup->arguments[1] <= 0xFFULL)
        return SURFACE_APP_MODE_PRODUCER;

    if (startup->capability_count == 1U && startup->argument_count == 2U &&
        startup->arguments[0] == JCOS_DISPLAY_SURFACE_INPUT_CLIENT_COOKIE &&
        startup->arguments[1] != 0ULL &&
        startup->capabilities[0] != JCOS_CAPABILITY_INVALID_HANDLE)
        return SURFACE_APP_MODE_INPUT;

    if (startup->capability_count == 1U && startup->argument_count == 2U &&
        startup->arguments[0] == JCOS_DISPLAY_SURFACE_SESSION_CLIENT_COOKIE &&
        startup->arguments[1] != 0ULL &&
        startup->capabilities[0] != JCOS_CAPABILITY_INVALID_HANDLE)
        return SURFACE_APP_MODE_SESSION;

    return SURFACE_APP_MODE_INVALID;
}

static unsigned char rgb332(unsigned int r, unsigned int g, unsigned int b) {
    return (unsigned char)(((r & 0x07U) << 5) | ((g & 0x07U) << 2) | (b & 0x03U));
}

static void draw_surface(volatile unsigned char *surface) {
    if (!surface) return;
    for (JcosU32 y = 0U; y < JCOS_DISPLAY_SURFACE_HEIGHT; ++y) {
        for (JcosU32 x = 0U; x < JCOS_DISPLAY_SURFACE_WIDTH; ++x) {
            unsigned char pixel;
            if (x == y || x + y + 1U == JCOS_DISPLAY_SURFACE_WIDTH) {
                pixel = rgb332(7U, 7U, 3U);
            } else if (x < JCOS_DISPLAY_SURFACE_WIDTH / 2U &&
                       y < JCOS_DISPLAY_SURFACE_HEIGHT / 2U) {
                pixel = rgb332(7U, 1U, 1U);
            } else if (x >= JCOS_DISPLAY_SURFACE_WIDTH / 2U &&
                       y < JCOS_DISPLAY_SURFACE_HEIGHT / 2U) {
                pixel = rgb332(1U, 7U, 1U);
            } else if (x < JCOS_DISPLAY_SURFACE_WIDTH / 2U) {
                pixel = rgb332(1U, 2U, 3U);
            } else {
                pixel = rgb332(6U, 2U, 3U);
            }
            surface[(JcosU64)y * JCOS_DISPLAY_SURFACE_STRIDE + x] = pixel;
        }
    }
}

static void fill_surface(volatile unsigned char *surface, unsigned char pixel) {
    if (!surface) return;
    for (JcosU32 i = 0U; i < JCOS_DISPLAY_SURFACE_BYTES; ++i) surface[i] = pixel;
}

static int receive_client_event(JcosCapabilityHandle input_cap, JcosU64 client_id,
    JcosIpcMessage *event) {
    if (!event) return 0;
    clear_message(event);
    if (!jcos_ipc_receive_blocking(input_cap, event) || event->word_count < 2U ||
        JCOS_DISPLAY_CLIENT_HEADER_VERSION(event->words[0]) != JCOS_DISPLAY_CLIENT_PROTOCOL_VERSION ||
        event->words[1] != client_id) return 0;

    JcosU64 operation = JCOS_DISPLAY_CLIENT_HEADER_OP(event->words[0]);
    if (operation == JCOS_DISPLAY_CLIENT_OP_DIAG_FAULT && event->word_count == 2U) {
        __asm__ volatile ("ud2");
        jcos_thread_exit();
    }

    return operation == JCOS_DISPLAY_CLIENT_OP_KEY_EVENT && event->word_count == 4U;
}

static void record_event(volatile unsigned char *surface, const JcosIpcMessage *event) {
    if (!surface || !event) return;
    surface[JCOS_DISPLAY_SURFACE_INPUT_MARKER_OFFSET] = JCOS_DISPLAY_SURFACE_INPUT_MARKER;
    surface[JCOS_DISPLAY_SURFACE_INPUT_KEY_OFFSET] =
        (unsigned char)JCOS_DISPLAY_INPUT_EVENT_KEY(event->words[2]);
    surface[JCOS_DISPLAY_SURFACE_INPUT_CHAR_OFFSET] =
        JCOS_DISPLAY_INPUT_EVENT_CHARACTER(event->words[2]);
    surface[JCOS_DISPLAY_SURFACE_INPUT_FLAGS_OFFSET] = (unsigned char)event->words[3];
}

static JcosU64 glyph_bits(char value) {
    char c = value;
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    switch (c) {
        case 'A': return GLYPH(0x0E,0x11,0x11,0x1F,0x11,0x11,0x11);
        case 'B': return GLYPH(0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E);
        case 'C': return GLYPH(0x0E,0x11,0x10,0x10,0x10,0x11,0x0E);
        case 'D': return GLYPH(0x1E,0x11,0x11,0x11,0x11,0x11,0x1E);
        case 'E': return GLYPH(0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F);
        case 'F': return GLYPH(0x1F,0x10,0x10,0x1E,0x10,0x10,0x10);
        case 'G': return GLYPH(0x0E,0x11,0x10,0x17,0x11,0x11,0x0F);
        case 'H': return GLYPH(0x11,0x11,0x11,0x1F,0x11,0x11,0x11);
        case 'I': return GLYPH(0x0E,0x04,0x04,0x04,0x04,0x04,0x0E);
        case 'J': return GLYPH(0x07,0x02,0x02,0x02,0x12,0x12,0x0C);
        case 'K': return GLYPH(0x11,0x12,0x14,0x18,0x14,0x12,0x11);
        case 'L': return GLYPH(0x10,0x10,0x10,0x10,0x10,0x10,0x1F);
        case 'M': return GLYPH(0x11,0x1B,0x15,0x15,0x11,0x11,0x11);
        case 'N': return GLYPH(0x11,0x19,0x15,0x13,0x11,0x11,0x11);
        case 'O': return GLYPH(0x0E,0x11,0x11,0x11,0x11,0x11,0x0E);
        case 'P': return GLYPH(0x1E,0x11,0x11,0x1E,0x10,0x10,0x10);
        case 'Q': return GLYPH(0x0E,0x11,0x11,0x11,0x15,0x12,0x0D);
        case 'R': return GLYPH(0x1E,0x11,0x11,0x1E,0x14,0x12,0x11);
        case 'S': return GLYPH(0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E);
        case 'T': return GLYPH(0x1F,0x04,0x04,0x04,0x04,0x04,0x04);
        case 'U': return GLYPH(0x11,0x11,0x11,0x11,0x11,0x11,0x0E);
        case 'V': return GLYPH(0x11,0x11,0x11,0x11,0x11,0x0A,0x04);
        case 'W': return GLYPH(0x11,0x11,0x11,0x15,0x15,0x15,0x0A);
        case 'X': return GLYPH(0x11,0x11,0x0A,0x04,0x0A,0x11,0x11);
        case 'Y': return GLYPH(0x11,0x11,0x0A,0x04,0x04,0x04,0x04);
        case 'Z': return GLYPH(0x1F,0x01,0x02,0x04,0x08,0x10,0x1F);
        case '0': return GLYPH(0x0E,0x11,0x13,0x15,0x19,0x11,0x0E);
        case '1': return GLYPH(0x04,0x0C,0x04,0x04,0x04,0x04,0x0E);
        case '2': return GLYPH(0x0E,0x11,0x01,0x02,0x04,0x08,0x1F);
        case '3': return GLYPH(0x1E,0x01,0x01,0x0E,0x01,0x01,0x1E);
        case '4': return GLYPH(0x02,0x06,0x0A,0x12,0x1F,0x02,0x02);
        case '5': return GLYPH(0x1F,0x10,0x10,0x1E,0x01,0x01,0x1E);
        case '6': return GLYPH(0x0E,0x10,0x10,0x1E,0x11,0x11,0x0E);
        case '7': return GLYPH(0x1F,0x01,0x02,0x04,0x08,0x08,0x08);
        case '8': return GLYPH(0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E);
        case '9': return GLYPH(0x0E,0x11,0x11,0x0F,0x01,0x01,0x0E);
        case '.': return GLYPH(0x00,0x00,0x00,0x00,0x00,0x0C,0x0C);
        case ',': return GLYPH(0x00,0x00,0x00,0x00,0x0C,0x0C,0x08);
        case '!': return GLYPH(0x04,0x04,0x04,0x04,0x04,0x00,0x04);
        case '?': return GLYPH(0x0E,0x11,0x01,0x02,0x04,0x00,0x04);
        case '-': return GLYPH(0x00,0x00,0x00,0x1F,0x00,0x00,0x00);
        case '_': return GLYPH(0x00,0x00,0x00,0x00,0x00,0x00,0x1F);
        case '/': return GLYPH(0x01,0x02,0x02,0x04,0x08,0x08,0x10);
        case ':': return GLYPH(0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00);
        case ' ': return 0ULL;
        default: return GLYPH(0x1F,0x11,0x02,0x04,0x04,0x00,0x04);
    }
}

static void session_fill_rect(volatile unsigned char *surface, JcosU32 x, JcosU32 y,
    JcosU32 width, JcosU32 height, unsigned char pixel) {
    if (!surface || x >= JCOS_DISPLAY_SURFACE_WIDTH || y >= JCOS_DISPLAY_SURFACE_HEIGHT) return;
    if (width > JCOS_DISPLAY_SURFACE_WIDTH - x) width = JCOS_DISPLAY_SURFACE_WIDTH - x;
    if (height > JCOS_DISPLAY_SURFACE_HEIGHT - y) height = JCOS_DISPLAY_SURFACE_HEIGHT - y;
    for (JcosU32 dy = 0U; dy < height; ++dy)
        for (JcosU32 dx = 0U; dx < width; ++dx)
            surface[(JcosU64)(y + dy) * JCOS_DISPLAY_SURFACE_STRIDE + x + dx] = pixel;
}

static void session_draw_glyph(volatile unsigned char *surface, JcosU32 x, JcosU32 y,
    char c, unsigned char fg, unsigned char bg) {
    session_fill_rect(surface, x, y, SESSION_CELL_WIDTH, SESSION_CELL_HEIGHT, bg);
    JcosU64 bits = glyph_bits(c);
    for (JcosU32 row = 0U; row < 7U; ++row) {
        JcosU32 pattern = (JcosU32)((bits >> (row * 5U)) & 0x1FULL);
        for (JcosU32 col = 0U; col < 5U; ++col) {
            if (pattern & (1U << (4U - col)))
                surface[(JcosU64)(y + row) * JCOS_DISPLAY_SURFACE_STRIDE + x + col] = fg;
        }
    }
}

static void session_draw_string(volatile unsigned char *surface, JcosU32 col, JcosU32 row,
    const char *text, unsigned char fg, unsigned char bg) {
    if (!text) return;
    while (*text && col < SESSION_COLUMNS) {
        session_draw_glyph(surface, col * SESSION_CELL_WIDTH, row * SESSION_CELL_HEIGHT,
            *text++, fg, bg);
        ++col;
    }
}

static void session_scroll(volatile unsigned char *surface, unsigned char bg) {
    for (JcosU32 y = SESSION_TEXT_Y; y + SESSION_CELL_HEIGHT < SESSION_STATUS_Y; ++y) {
        for (JcosU32 x = 0U; x < JCOS_DISPLAY_SURFACE_WIDTH; ++x) {
            surface[(JcosU64)y * JCOS_DISPLAY_SURFACE_STRIDE + x] =
                surface[(JcosU64)(y + SESSION_CELL_HEIGHT) * JCOS_DISPLAY_SURFACE_STRIDE + x];
        }
    }
    session_fill_rect(surface, 0U, SESSION_STATUS_Y - SESSION_CELL_HEIGHT,
        JCOS_DISPLAY_SURFACE_WIDTH, SESSION_CELL_HEIGHT, bg);
}

static void session_newline(volatile unsigned char *surface, JcosU32 *col, JcosU32 *row,
    unsigned char bg) {
    *col = 0U;
    if (*row + 1U < SESSION_ROWS) {
        ++*row;
    } else {
        session_scroll(surface, bg);
    }
}

static void session_status(volatile unsigned char *surface, const JcosIpcMessage *event,
    unsigned char fg, unsigned char bg) {
    unsigned char status = rgb332(0U, 2U, 1U);
    if (event->words[3] & JCOS_DISPLAY_INPUT_FLAG_SHIFT) status = rgb332(1U, 4U, 1U);
    if (event->words[3] & JCOS_DISPLAY_INPUT_FLAG_CTRL) status = rgb332(1U, 2U, 3U);
    if (event->words[3] & JCOS_DISPLAY_INPUT_FLAG_ALT) status = rgb332(4U, 1U, 2U);
    session_fill_rect(surface, 0U, SESSION_STATUS_Y, JCOS_DISPLAY_SURFACE_WIDTH, 8U, status);
    unsigned char c = JCOS_DISPLAY_INPUT_EVENT_CHARACTER(event->words[2]);
    if (c >= 32U && c <= 126U) session_draw_glyph(surface, 1U, SESSION_STATUS_Y, (char)c, fg, status);
    (void)bg;
}

static void session_init(volatile unsigned char *surface) {
    unsigned char bg = rgb332(0U, 0U, 1U);
    unsigned char fg = rgb332(7U, 7U, 3U);
    unsigned char accent = rgb332(1U, 7U, 1U);
    fill_surface(surface, bg);
    session_fill_rect(surface, 0U, 0U, JCOS_DISPLAY_SURFACE_WIDTH, 8U, rgb332(0U, 2U, 2U));
    session_draw_string(surface, 0U, 0U, "JA OS GUI", accent, rgb332(0U, 2U, 2U));
    session_fill_rect(surface, 0U, SESSION_STATUS_Y, JCOS_DISPLAY_SURFACE_WIDTH, 8U, rgb332(0U, 2U, 1U));
    session_draw_glyph(surface, 1U, SESSION_STATUS_Y, '>', fg, rgb332(0U, 2U, 1U));
}

static void session_handle_event(volatile unsigned char *surface, const JcosIpcMessage *event,
    JcosU32 *col, JcosU32 *row) {
    unsigned char bg = rgb332(0U, 0U, 1U);
    unsigned char fg = rgb332(7U, 7U, 3U);
    JcosU32 key = JCOS_DISPLAY_INPUT_EVENT_KEY(event->words[2]);
    unsigned char character = JCOS_DISPLAY_INPUT_EVENT_CHARACTER(event->words[2]);
    int pressed = (event->words[3] & JCOS_DISPLAY_INPUT_FLAG_PRESSED) != 0ULL;
    session_status(surface, event, fg, bg);
    record_event(surface, event);
    if (!pressed) return;

    if (key == JCOS_DISPLAY_INPUT_KEY_CHARACTER && character >= 32U && character <= 126U) {
        session_draw_glyph(surface, *col * SESSION_CELL_WIDTH,
            SESSION_TEXT_Y + *row * SESSION_CELL_HEIGHT, (char)character, fg, bg);
        if (*col + 1U < SESSION_COLUMNS) ++*col;
        else session_newline(surface, col, row, bg);
        return;
    }
    if (key == JCOS_DISPLAY_INPUT_KEY_ENTER) {
        session_newline(surface, col, row, bg);
        return;
    }
    if (key == JCOS_DISPLAY_INPUT_KEY_BACKSPACE) {
        if (*col) --*col;
        else if (*row) { --*row; *col = SESSION_COLUMNS - 1U; }
        session_draw_glyph(surface, *col * SESSION_CELL_WIDTH,
            SESSION_TEXT_Y + *row * SESSION_CELL_HEIGHT, ' ', fg, bg);
        return;
    }
    if (key == JCOS_DISPLAY_INPUT_KEY_TAB) {
        JcosU32 next = (*col + 4U) & ~3U;
        *col = next < SESSION_COLUMNS ? next : SESSION_COLUMNS - 1U;
        return;
    }
    if (key == JCOS_DISPLAY_INPUT_KEY_LEFT && *col) --*col;
    else if (key == JCOS_DISPLAY_INPUT_KEY_RIGHT && *col + 1U < SESSION_COLUMNS) ++*col;
    else if (key == JCOS_DISPLAY_INPUT_KEY_UP && *row) --*row;
    else if (key == JCOS_DISPLAY_INPUT_KEY_DOWN && *row + 1U < SESSION_ROWS) ++*row;
    else if (key == JCOS_DISPLAY_INPUT_KEY_HOME) *col = 0U;
    else if (key == JCOS_DISPLAY_INPUT_KEY_END) *col = SESSION_COLUMNS - 1U;
    else if (key == JCOS_DISPLAY_INPUT_KEY_DELETE)
        session_draw_glyph(surface, *col * SESSION_CELL_WIDTH,
            SESSION_TEXT_Y + *row * SESSION_CELL_HEIGHT, ' ', fg, bg);
}

__attribute__((noreturn))
void jcos_main(const JcosProgramStartup *startup) {
    int mode = startup_mode(startup);
    if (mode == SURFACE_APP_MODE_INVALID) jcos_thread_exit();

    volatile unsigned char *surface =
        (volatile unsigned char *)(JcosU64)JCOS_DISPLAY_SURFACE_VIRTUAL_BASE;

    if (mode == SURFACE_APP_MODE_PRODUCER) {
        fill_surface(surface, (unsigned char)startup->arguments[1]);
        jcos_thread_exit();
    }

    if (mode == SURFACE_APP_MODE_INPUT || mode == SURFACE_APP_MODE_SESSION) {
        JcosCapabilityHandle input_cap = startup->capabilities[0];
        JcosU64 client_id = startup->arguments[1];
        if (mode == SURFACE_APP_MODE_SESSION) {
            JcosU32 col = 0U;
            JcosU32 row = 0U;
            session_init(surface);
            for (;;) {
                JcosIpcMessage event;
                if (!receive_client_event(input_cap, client_id, &event)) jcos_thread_exit();
                session_handle_event(surface, &event, &col, &row);
            }
        }

        JcosIpcMessage event;
        if (!receive_client_event(input_cap, client_id, &event)) jcos_thread_exit();
        record_event(surface, &event);
        jcos_thread_exit();
    }

    JcosCapabilityHandle command_cap = startup->capabilities[0];
    JcosCapabilityHandle reply_cap = startup->capabilities[1];
    JcosU64 expected_incarnation = startup->arguments[0];

    draw_surface(surface);

    JcosIpcMessage request;
    clear_message(&request);
    request.word_count = 3U;
    request.words[0] = JCOS_DISPLAY_SERVICE_HEADER(
        JCOS_DISPLAY_SERVICE_OP_PRESENT_SURFACE, JCOS_DISPLAY_SERVICE_PROTOCOL_VERSION);
    request.words[1] = 0ULL;
    request.words[2] = JCOS_DISPLAY_SURFACE_APP_COOKIE;
    if (!jcos_ipc_send_blocking(command_cap, &request)) jcos_thread_exit();

    JcosIpcMessage reply;
    clear_message(&reply);
    if (!jcos_ipc_receive_blocking(reply_cap, &reply)) jcos_thread_exit();
    if (reply.word_count != 4U ||
        reply.words[0] != JCOS_DISPLAY_SERVICE_REPLY_PRESENTED ||
        reply.words[1] != expected_incarnation ||
        reply.words[2] != JCOS_DISPLAY_SERVICE_PROTOCOL_VERSION ||
        reply.words[3] != JCOS_DISPLAY_SURFACE_APP_COOKIE) jcos_thread_exit();

    surface[JCOS_DISPLAY_SURFACE_BYTES - 1U] = JCOS_DISPLAY_SURFACE_APP_MARKER;
    jcos_thread_exit();
}