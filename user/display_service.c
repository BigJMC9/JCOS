#include "lib/syscall.h"
#include "../include/display_service_protocol.h"
#include "../include/program_startup.h"
#include "../include/service_protocol.h"

typedef struct {
    volatile JcosU32 *framebuffer;
    JcosU32 width;
    JcosU32 height;
    JcosU32 stride;
    JcosU32 pixel_format;
} DisplayTarget;

typedef struct {
    JcosU32 x;
    JcosU32 y;
    JcosU32 width;
    JcosU32 height;
    JcosU32 z;
    unsigned int active;
} CompositorSlot;

static void clear_message(JcosIpcMessage *message) {
    if (!message) return;
    for (JcosU32 i = 0; i < JCOS_IPC_MESSAGE_MAX_WORDS; ++i) message->words[i] = 0ULL;
    message->word_count = 0U;
}

static int startup_valid(const JcosProgramStartup *startup) {
    if (!startup || startup->magic != JCOS_PROGRAM_STARTUP_MAGIC ||
        startup->version != JCOS_PROGRAM_STARTUP_VERSION ||
        startup->size != sizeof(JcosProgramStartup) ||
        startup->flags != JCOS_PROGRAM_STARTUP_FLAG_NONE ||
        startup->capability_count != 2U || startup->argument_count != 4U ||
        startup->environment_count != 0U || !startup->arguments[0] ||
        !startup->arguments[1] ||
        startup->capabilities[0] == JCOS_CAPABILITY_INVALID_HANDLE ||
        startup->capabilities[1] == JCOS_CAPABILITY_INVALID_HANDLE) return 0;

    JcosU32 width = JCOS_DISPLAY_WIDTH(startup->arguments[2]);
    JcosU32 height = JCOS_DISPLAY_HEIGHT(startup->arguments[2]);
    JcosU32 stride = JCOS_DISPLAY_STRIDE(startup->arguments[3]);
    JcosU32 format = JCOS_DISPLAY_PIXEL_FORMAT(startup->arguments[3]);
    return width && height && stride >= width && format <= 2U;
}

static JcosU32 rgb(const DisplayTarget *target, JcosU32 r, JcosU32 g, JcosU32 b) {
    if (!target) return 0U;
    r &= 0xFFU;
    g &= 0xFFU;
    b &= 0xFFU;
    if (target->pixel_format == 0U) return r | (g << 8) | (b << 16);
    if (target->pixel_format == 1U) return b | (g << 8) | (r << 16);

    /* PixelBitMask needs mask metadata for true colour. Keep the R8 fallback
     * deterministic until that metadata is part of the display-service ABI. */
    return (r || g || b) ? 0xFFFFFFFFU : 0U;
}

static JcosU32 rgb332(const DisplayTarget *target, unsigned char pixel) {
    JcosU32 r = (JcosU32)((pixel >> 5) & 0x07U);
    JcosU32 g = (JcosU32)((pixel >> 2) & 0x07U);
    JcosU32 b = (JcosU32)(pixel & 0x03U);
    return rgb(target, (r * 255U) / 7U, (g * 255U) / 7U, (b * 255U) / 3U);
}

static const volatile unsigned char *surface_source(JcosU32 slot) {
    if (slot >= JCOS_DISPLAY_SURFACE_CAPACITY) return 0;
    return (const volatile unsigned char *)(JcosU64)JCOS_DISPLAY_SURFACE_SLOT_VA(slot);
}

static void clear_target(const DisplayTarget *target) {
    if (!target || !target->framebuffer || !target->width || !target->height ||
        target->stride < target->width) return;
    JcosU32 black = rgb(target, 0U, 0U, 0U);
    for (JcosU32 y = 0U; y < target->height; ++y) {
        volatile JcosU32 *row = target->framebuffer + (JcosU64)y * target->stride;
        for (JcosU32 x = 0U; x < target->width; ++x) row[x] = black;
    }
}

static void draw_surface_rect(const DisplayTarget *target, JcosU32 slot,
    const CompositorSlot *config) {
    const volatile unsigned char *source = surface_source(slot);
    if (!target || !source || !config || !config->active || !config->width ||
        !config->height) return;

    for (JcosU32 dy = 0U; dy < config->height; ++dy) {
        JcosU32 source_y = (JcosU32)(((JcosU64)dy * JCOS_DISPLAY_SURFACE_HEIGHT) /
            config->height);
        if (source_y >= JCOS_DISPLAY_SURFACE_HEIGHT)
            source_y = JCOS_DISPLAY_SURFACE_HEIGHT - 1U;
        volatile JcosU32 *row = target->framebuffer +
            (JcosU64)(config->y + dy) * target->stride + config->x;
        const volatile unsigned char *source_row = source +
            (JcosU64)source_y * JCOS_DISPLAY_SURFACE_STRIDE;

        for (JcosU32 dx = 0U; dx < config->width; ++dx) {
            JcosU32 source_x = (JcosU32)(((JcosU64)dx * JCOS_DISPLAY_SURFACE_WIDTH) /
                config->width);
            if (source_x >= JCOS_DISPLAY_SURFACE_WIDTH)
                source_x = JCOS_DISPLAY_SURFACE_WIDTH - 1U;
            row[dx] = rgb332(target, source_row[source_x]);
        }
    }
}

static void draw_shared_surface(const DisplayTarget *target) {
    if (!target) return;
    CompositorSlot full;
    full.x = 0U;
    full.y = 0U;
    full.width = target->width;
    full.height = target->height;
    full.z = 0U;
    full.active = 1U;
    draw_surface_rect(target, 0U, &full);
}

static JcosU32 compose(const DisplayTarget *target, const CompositorSlot *slots,
    JcosU32 *out_sample) {
    if (!target || !slots || !out_sample) return 0U;
    clear_target(target);

    unsigned int drawn[JCOS_DISPLAY_SURFACE_CAPACITY];
    for (JcosU32 i = 0U; i < JCOS_DISPLAY_SURFACE_CAPACITY; ++i) drawn[i] = 0U;

    JcosU32 count = 0U;
    for (;;) {
        JcosU32 best = JCOS_DISPLAY_SURFACE_CAPACITY;
        for (JcosU32 i = 0U; i < JCOS_DISPLAY_SURFACE_CAPACITY; ++i) {
            if (!slots[i].active || drawn[i]) continue;
            if (best == JCOS_DISPLAY_SURFACE_CAPACITY || slots[i].z < slots[best].z ||
                (slots[i].z == slots[best].z && i < best)) best = i;
        }
        if (best == JCOS_DISPLAY_SURFACE_CAPACITY) break;
        draw_surface_rect(target, best, &slots[best]);
        drawn[best] = 1U;
        ++count;
    }

    JcosU32 sample_x = target->width / 2U;
    JcosU32 sample_y = target->height / 2U;
    *out_sample = target->framebuffer[(JcosU64)sample_y * target->stride + sample_x];
    return count;
}

static int configure_slot(const DisplayTarget *target, CompositorSlot *slots,
    JcosU64 config_word, JcosU64 position_word, JcosU64 extent_word) {
    if (!target || !slots) return 0;
    JcosU32 slot = JCOS_DISPLAY_SURFACE_CONFIG_SLOT(config_word);
    JcosU32 z = JCOS_DISPLAY_SURFACE_CONFIG_Z(config_word);
    JcosU32 x = JCOS_DISPLAY_SURFACE_POSITION_X(position_word);
    JcosU32 y = JCOS_DISPLAY_SURFACE_POSITION_Y(position_word);
    JcosU32 width = JCOS_DISPLAY_SURFACE_EXTENT_WIDTH(extent_word);
    JcosU32 height = JCOS_DISPLAY_SURFACE_EXTENT_HEIGHT(extent_word);
    if (slot >= JCOS_DISPLAY_SURFACE_CAPACITY || !width || !height ||
        x >= target->width || y >= target->height ||
        width > target->width - x || height > target->height - y) return 0;

    slots[slot].x = x;
    slots[slot].y = y;
    slots[slot].width = width;
    slots[slot].height = height;
    slots[slot].z = z;
    slots[slot].active = 1U;
    return 1;
}

static void remove_slot(CompositorSlot *slots, JcosU32 slot) {
    if (!slots || slot >= JCOS_DISPLAY_SURFACE_CAPACITY) return;
    slots[slot].x = 0U;
    slots[slot].y = 0U;
    slots[slot].width = 0U;
    slots[slot].height = 0U;
    slots[slot].z = 0U;
    slots[slot].active = 0U;
}

static void draw_pattern(const DisplayTarget *target) {
    if (!target || !target->framebuffer || !target->width || !target->height ||
        target->stride < target->width) return;

    JcosU32 black = rgb(target, 0U, 0U, 0U);
    JcosU32 white = rgb(target, 255U, 255U, 255U);
    JcosU32 red = rgb(target, 220U, 50U, 60U);
    JcosU32 green = rgb(target, 60U, 210U, 110U);
    JcosU32 blue = rgb(target, 70U, 130U, 240U);

    JcosU32 band = target->height / 4U;
    if (!band) band = 1U;
    JcosU32 third = target->width / 3U;
    if (!third) third = 1U;

    for (JcosU32 y = 0U; y < target->height; ++y) {
        volatile JcosU32 *row = target->framebuffer + (JcosU64)y * target->stride;
        for (JcosU32 x = 0U; x < target->width; ++x) {
            JcosU32 color = black;
            if (target->pixel_format <= 1U && y < band) {
                color = x < third ? red : (x < third * 2U ? green : blue);
            } else if (target->pixel_format == 2U && y < band) {
                color = ((x / 32U) & 1U) ? white : black;
            }

            if (x < 4U || y < 4U || x + 4U >= target->width || y + 4U >= target->height)
                color = white;
            row[x] = color;
        }
    }
}

static int send_reply(JcosCapabilityHandle reply_cap, JcosU64 code,
    JcosU64 incarnation, JcosU64 detail, JcosU32 words) {
    JcosIpcMessage reply;
    clear_message(&reply);
    reply.word_count = words;
    reply.words[0] = code;
    reply.words[1] = incarnation;
    reply.words[2] = JCOS_DISPLAY_SERVICE_PROTOCOL_VERSION;
    reply.words[3] = detail;
    return jcos_ipc_send_blocking(reply_cap, &reply);
}

__attribute__((noreturn))
void jcos_main(const JcosProgramStartup *startup) {
    if (!startup_valid(startup)) jcos_thread_exit();

    JcosCapabilityHandle command_cap = startup->capabilities[0];
    JcosCapabilityHandle reply_cap = startup->capabilities[1];
    JcosU64 incarnation = startup->arguments[0];

    DisplayTarget target;
    target.framebuffer = (volatile JcosU32 *)(JcosU64)startup->arguments[1];
    target.width = JCOS_DISPLAY_WIDTH(startup->arguments[2]);
    target.height = JCOS_DISPLAY_HEIGHT(startup->arguments[2]);
    target.stride = JCOS_DISPLAY_STRIDE(startup->arguments[3]);
    target.pixel_format = JCOS_DISPLAY_PIXEL_FORMAT(startup->arguments[3]);

    CompositorSlot slots[JCOS_DISPLAY_SURFACE_CAPACITY];
    for (JcosU32 i = 0U; i < JCOS_DISPLAY_SURFACE_CAPACITY; ++i) remove_slot(slots, i);
    JcosU32 focused_slot = JCOS_DISPLAY_SURFACE_FOCUS_NONE;

    draw_pattern(&target);

    for (;;) {
        JcosIpcMessage request;
        clear_message(&request);
        if (!jcos_ipc_receive_blocking(command_cap, &request)) jcos_thread_exit();
        if (request.word_count < 2U) {
            if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, request.word_count, 4U)) jcos_thread_exit();
            continue;
        }

        JcosU64 header = request.words[0];
        JcosU64 op = JCOS_DISPLAY_SERVICE_HEADER_OP(header);
        JcosU64 version = JCOS_DISPLAY_SERVICE_HEADER_VERSION(header);
        if (version != JCOS_DISPLAY_SERVICE_PROTOCOL_VERSION) {
            if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INCOMPATIBLE_VERSION, incarnation, version, 4U)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_DISPLAY_SERVICE_OP_PING) {
            if (request.word_count != 3U) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op, 4U)) jcos_thread_exit();
                continue;
            }
            if (!send_reply(reply_cap, JCOS_DISPLAY_SERVICE_REPLY_PONG, incarnation, request.words[2], 4U)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_DISPLAY_SERVICE_OP_REDRAW) {
            if (request.word_count != 2U) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op, 4U)) jcos_thread_exit();
                continue;
            }
            draw_pattern(&target);
            if (!send_reply(reply_cap, JCOS_DISPLAY_SERVICE_REPLY_DRAWN, incarnation, 0ULL, 3U)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_DISPLAY_SERVICE_OP_PRESENT_SURFACE) {
            if (request.word_count != 3U) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op, 4U)) jcos_thread_exit();
                continue;
            }
            draw_shared_surface(&target);
            if (!send_reply(reply_cap, JCOS_DISPLAY_SERVICE_REPLY_PRESENTED, incarnation, request.words[2], 4U)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_DISPLAY_SERVICE_OP_SURFACE_CONFIG) {
            if (request.word_count != 4U || !configure_slot(&target, slots,
                    request.words[1], request.words[2], request.words[3])) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op, 4U)) jcos_thread_exit();
                continue;
            }
            JcosU32 slot = JCOS_DISPLAY_SURFACE_CONFIG_SLOT(request.words[1]);
            if (!send_reply(reply_cap, JCOS_DISPLAY_SERVICE_REPLY_CONFIGURED, incarnation, slot, 4U)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_DISPLAY_SERVICE_OP_COMPOSE) {
            if (request.word_count != 2U) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op, 4U)) jcos_thread_exit();
                continue;
            }
            JcosU32 sample = 0U;
            JcosU32 count = compose(&target, slots, &sample);
            if (!send_reply(reply_cap, JCOS_DISPLAY_SERVICE_REPLY_COMPOSED, incarnation, JCOS_DISPLAY_COMPOSE_DETAIL(count, sample), 4U))
                jcos_thread_exit();
            continue;
        }

        if (op == JCOS_DISPLAY_SERVICE_OP_FOCUS_SURFACE) {
            if (request.word_count != 3U || request.words[2] > 0xFFFFFFFFULL) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op, 4U)) jcos_thread_exit();
                continue;
            }
            JcosU32 requested = (JcosU32)request.words[2];
            if (requested != JCOS_DISPLAY_SURFACE_FOCUS_NONE &&
                (requested >= JCOS_DISPLAY_SURFACE_CAPACITY || !slots[requested].active)) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op, 4U)) jcos_thread_exit();
                continue;
            }
            focused_slot = requested;
            if (!send_reply(reply_cap, JCOS_DISPLAY_SERVICE_REPLY_FOCUSED, incarnation, focused_slot, 4U)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_DISPLAY_SERVICE_OP_SURFACE_REMOVE) {
            if (request.word_count != 3U || request.words[2] >= JCOS_DISPLAY_SURFACE_CAPACITY) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op, 4U)) jcos_thread_exit();
                continue;
            }
            JcosU32 slot = (JcosU32)request.words[2];
            remove_slot(slots, slot);
            if (focused_slot == slot) focused_slot = JCOS_DISPLAY_SURFACE_FOCUS_NONE;
            if (!send_reply(reply_cap, JCOS_DISPLAY_SERVICE_REPLY_REMOVED, incarnation, slot, 4U)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_DISPLAY_SERVICE_OP_DIAG_FAULT) {
            if (request.word_count != 2U) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op, 4U)) jcos_thread_exit();
                continue;
            }
            __asm__ volatile ("ud2");
            __builtin_unreachable();
        }

        if (op == JCOS_DISPLAY_SERVICE_OP_SHUTDOWN) {
            if (request.word_count != 2U) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op, 4U)) jcos_thread_exit();
                continue;
            }
            if (!send_reply(reply_cap, JCOS_DISPLAY_SERVICE_REPLY_STOPPED, incarnation, 0ULL, 3U)) jcos_thread_exit();
            jcos_thread_exit();
        }

        if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_UNKNOWN_OPERATION, incarnation, op, 4U)) jcos_thread_exit();
    }
}
