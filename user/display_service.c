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
    JcosU32 red_mask;
    JcosU32 green_mask;
    JcosU32 blue_mask;
    JcosU32 reserved_mask;
    unsigned int masks_valid;
} DisplayTarget;

static void clear_message(JcosIpcMessage *message) {
    if (!message) return;
    for (JcosU32 i = 0U; i < JCOS_IPC_MESSAGE_MAX_WORDS; ++i) message->words[i] = 0ULL;
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

static JcosU32 channel(JcosU32 value, JcosU32 mask) {
    if (!mask) return 0U;
    JcosU32 shift = 0U;
    while (shift < 31U && ((mask >> shift) & 1U) == 0U) ++shift;
    JcosU32 max = mask >> shift;
    return (JcosU32)((((JcosU64)(value & 0xFFU) * (JcosU64)max) / 255ULL << shift) & mask);
}

static int target_ready(const DisplayTarget *target) {
    return target && target->framebuffer && target->width && target->height &&
        target->stride >= target->width && (target->pixel_format <= 1U || target->masks_valid);
}

static JcosU32 rgb(const DisplayTarget *target, JcosU32 r, JcosU32 g, JcosU32 b) {
    if (!target) return 0U;
    r &= 0xFFU;
    g &= 0xFFU;
    b &= 0xFFU;
    if (target->pixel_format == 0U) return r | (g << 8) | (b << 16);
    if (target->pixel_format == 1U) return b | (g << 8) | (r << 16);
    if (target->pixel_format != 2U || !target->masks_valid) return 0U;
    return channel(r, target->red_mask) |
        channel(g, target->green_mask) |
        channel(b, target->blue_mask);
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

static int clear_target(const DisplayTarget *target) {
    if (!target_ready(target)) return 0;
    JcosU32 black = rgb(target, 0U, 0U, 0U);
    for (JcosU32 y = 0U; y < target->height; ++y) {
        volatile JcosU32 *row = target->framebuffer + (JcosU64)y * target->stride;
        for (JcosU32 x = 0U; x < target->width; ++x) row[x] = black;
    }
    return 1;
}

static int draw_surface_rect(const DisplayTarget *target, JcosU32 slot,
    JcosU32 x, JcosU32 y, JcosU32 width, JcosU32 height) {
    const volatile unsigned char *source = surface_source(slot);
    if (!target_ready(target) || !source || !width || !height ||
        x >= target->width || y >= target->height ||
        width > target->width - x || height > target->height - y) return 0;

    for (JcosU32 dy = 0U; dy < height; ++dy) {
        JcosU32 source_y = (JcosU32)(((JcosU64)dy * JCOS_DISPLAY_SURFACE_HEIGHT) / height);
        if (source_y >= JCOS_DISPLAY_SURFACE_HEIGHT)
            source_y = JCOS_DISPLAY_SURFACE_HEIGHT - 1U;
        volatile JcosU32 *row = target->framebuffer + (JcosU64)(y + dy) * target->stride + x;
        const volatile unsigned char *source_row = source +
            (JcosU64)source_y * JCOS_DISPLAY_SURFACE_STRIDE;

        for (JcosU32 dx = 0U; dx < width; ++dx) {
            JcosU32 source_x = (JcosU32)(((JcosU64)dx * JCOS_DISPLAY_SURFACE_WIDTH) / width);
            if (source_x >= JCOS_DISPLAY_SURFACE_WIDTH)
                source_x = JCOS_DISPLAY_SURFACE_WIDTH - 1U;
            row[dx] = rgb332(target, source_row[source_x]);
        }
    }
    return 1;
}

static int draw_shared_surface(const DisplayTarget *target) {
    return target_ready(target) && draw_surface_rect(target, 0U, 0U, 0U, target->width, target->height);
}

static JcosU32 sample_center(const DisplayTarget *target) {
    if (!target_ready(target)) return 0U;
    JcosU32 x = target->width / 2U;
    JcosU32 y = target->height / 2U;
    return target->framebuffer[(JcosU64)y * target->stride + x];
}

static void draw_pattern(const DisplayTarget *target) {
    if (!target_ready(target)) return;

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
            JcosU32 color = y < band ?
                (x < third ? red : (x < third * 2U ? green : blue)) : black;
            if (x < 4U || y < 4U || x + 4U >= target->width || y + 4U >= target->height)
                color = white;
            row[x] = color;
        }
    }
}

static int set_pixel_masks(DisplayTarget *target, JcosU64 rg, JcosU64 br) {
    if (!target || target->pixel_format != 2U) return 0;
    JcosU32 red = JCOS_DISPLAY_MASK_LOW(rg);
    JcosU32 green = JCOS_DISPLAY_MASK_HIGH(rg);
    JcosU32 blue = JCOS_DISPLAY_MASK_LOW(br);
    JcosU32 reserved = JCOS_DISPLAY_MASK_HIGH(br);
    if (!(red | green | blue) || (red & green) || (red & blue) || (green & blue)) return 0;

    target->red_mask = red;
    target->green_mask = green;
    target->blue_mask = blue;
    target->reserved_mask = reserved;
    target->masks_valid = 1U;
    return 1;
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
    target.red_mask = 0U;
    target.green_mask = 0U;
    target.blue_mask = 0U;
    target.reserved_mask = 0U;
    target.masks_valid = target.pixel_format <= 1U;

    if (target_ready(&target)) draw_pattern(&target);

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

        if (op == JCOS_DISPLAY_SERVICE_OP_SET_PIXEL_MASKS) {
            if (request.word_count != 3U || !set_pixel_masks(&target, request.words[1], request.words[2])) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op, 4U)) jcos_thread_exit();
                continue;
            }
            if (!send_reply(reply_cap, JCOS_DISPLAY_SERVICE_REPLY_MASKS_SET, incarnation, 0ULL, 3U)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_DISPLAY_SERVICE_OP_REDRAW) {
            if (request.word_count != 2U || !target_ready(&target)) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op, 4U)) jcos_thread_exit();
                continue;
            }
            draw_pattern(&target);
            if (!send_reply(reply_cap, JCOS_DISPLAY_SERVICE_REPLY_DRAWN, incarnation, 0ULL, 3U)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_DISPLAY_SERVICE_OP_CLEAR) {
            if (request.word_count != 2U || !clear_target(&target)) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op, 4U)) jcos_thread_exit();
                continue;
            }
            if (!send_reply(reply_cap, JCOS_DISPLAY_SERVICE_REPLY_CLEARED, incarnation, 0ULL, 3U)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_DISPLAY_SERVICE_OP_PRESENT_SURFACE) {
            if (request.word_count != 3U || !draw_shared_surface(&target)) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op, 4U)) jcos_thread_exit();
                continue;
            }
            if (!send_reply(reply_cap, JCOS_DISPLAY_SERVICE_REPLY_PRESENTED, incarnation, request.words[2], 4U)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_DISPLAY_SERVICE_OP_PRESENT_RECT) {
            if (request.word_count != 4U || request.words[1] >= JCOS_DISPLAY_SURFACE_CAPACITY) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op, 4U)) jcos_thread_exit();
                continue;
            }
            JcosU32 slot = (JcosU32)request.words[1];
            JcosU32 x = JCOS_DISPLAY_SURFACE_POSITION_X(request.words[2]);
            JcosU32 y = JCOS_DISPLAY_SURFACE_POSITION_Y(request.words[2]);
            JcosU32 width = JCOS_DISPLAY_SURFACE_EXTENT_WIDTH(request.words[3]);
            JcosU32 height = JCOS_DISPLAY_SURFACE_EXTENT_HEIGHT(request.words[3]);
            if (!draw_surface_rect(&target, slot, x, y, width, height)) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op, 4U)) jcos_thread_exit();
                continue;
            }
            if (!send_reply(reply_cap, JCOS_DISPLAY_SERVICE_REPLY_RECT_PRESENTED, incarnation, slot, 4U)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_DISPLAY_SERVICE_OP_SAMPLE) {
            if (request.word_count != 2U || !target_ready(&target)) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op, 4U)) jcos_thread_exit();
                continue;
            }
            if (!send_reply(reply_cap, JCOS_DISPLAY_SERVICE_REPLY_SAMPLED, incarnation, sample_center(&target), 4U)) jcos_thread_exit();
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
