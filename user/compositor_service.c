#include "lib/syscall.h"
#include "../include/compositor_service_protocol.h"
#include "../include/display_service_protocol.h"
#include "../include/program_startup.h"
#include "../include/service_protocol.h"

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
    for (JcosU32 i = 0U; i < JCOS_IPC_MESSAGE_MAX_WORDS; ++i) message->words[i] = 0ULL;
    message->word_count = 0U;
}

static int startup_valid(const JcosProgramStartup *startup) {
    return startup && startup->magic == JCOS_PROGRAM_STARTUP_MAGIC &&
        startup->version == JCOS_PROGRAM_STARTUP_VERSION &&
        startup->size == sizeof(JcosProgramStartup) &&
        startup->flags == JCOS_PROGRAM_STARTUP_FLAG_NONE &&
        startup->capability_count == 4U && startup->argument_count == 2U &&
        startup->environment_count == 0U && startup->arguments[0] && startup->arguments[1] &&
        startup->capabilities[0] != JCOS_CAPABILITY_INVALID_HANDLE &&
        startup->capabilities[1] != JCOS_CAPABILITY_INVALID_HANDLE &&
        startup->capabilities[2] != JCOS_CAPABILITY_INVALID_HANDLE &&
        startup->capabilities[3] != JCOS_CAPABILITY_INVALID_HANDLE;
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

static int configure_slot(CompositorSlot *slots, JcosU64 config_word,
    JcosU64 position_word, JcosU64 extent_word) {
    if (!slots) return 0;
    JcosU32 slot = JCOS_DISPLAY_SURFACE_CONFIG_SLOT(config_word);
    JcosU32 width = JCOS_DISPLAY_SURFACE_EXTENT_WIDTH(extent_word);
    JcosU32 height = JCOS_DISPLAY_SURFACE_EXTENT_HEIGHT(extent_word);
    if (slot >= JCOS_DISPLAY_SURFACE_CAPACITY || !width || !height) return 0;

    slots[slot].x = JCOS_DISPLAY_SURFACE_POSITION_X(position_word);
    slots[slot].y = JCOS_DISPLAY_SURFACE_POSITION_Y(position_word);
    slots[slot].width = width;
    slots[slot].height = height;
    slots[slot].z = JCOS_DISPLAY_SURFACE_CONFIG_Z(config_word);
    slots[slot].active = 1U;
    return 1;
}

static int send_reply(JcosCapabilityHandle reply_cap, JcosU64 code,
    JcosU64 incarnation, JcosU64 detail) {
    JcosIpcMessage reply;
    clear_message(&reply);
    reply.word_count = 4U;
    reply.words[0] = code;
    reply.words[1] = incarnation;
    reply.words[2] = JCOS_COMPOSITOR_SERVICE_PROTOCOL_VERSION;
    reply.words[3] = detail;
    return jcos_ipc_send_blocking(reply_cap, &reply);
}

static int display_call(JcosCapabilityHandle command_cap, JcosCapabilityHandle reply_cap,
    JcosU64 display_incarnation, const JcosIpcMessage *request, JcosIpcMessage *reply) {
    if (!request || !reply || !jcos_ipc_send_blocking(command_cap, request)) return 0;
    clear_message(reply);
    return jcos_ipc_receive_blocking(reply_cap, reply) && reply->word_count >= 2U &&
        reply->words[1] == display_incarnation;
}

static int display_clear(JcosCapabilityHandle command_cap, JcosCapabilityHandle reply_cap,
    JcosU64 display_incarnation) {
    JcosIpcMessage request;
    JcosIpcMessage reply;
    clear_message(&request);
    request.word_count = 2U;
    request.words[0] = JCOS_DISPLAY_SERVICE_HEADER(JCOS_DISPLAY_SERVICE_OP_CLEAR, JCOS_DISPLAY_SERVICE_PROTOCOL_VERSION);
    return display_call(command_cap, reply_cap, display_incarnation, &request, &reply) &&
        reply.word_count == 3U && reply.words[0] == JCOS_DISPLAY_SERVICE_REPLY_CLEARED &&
        reply.words[2] == JCOS_DISPLAY_SERVICE_PROTOCOL_VERSION;
}

static int display_present(JcosCapabilityHandle command_cap, JcosCapabilityHandle reply_cap,
    JcosU64 display_incarnation, JcosU32 slot, const CompositorSlot *config) {
    if (!config || !config->active) return 0;
    JcosIpcMessage request;
    JcosIpcMessage reply;
    clear_message(&request);
    request.word_count = 4U;
    request.words[0] = JCOS_DISPLAY_SERVICE_HEADER(JCOS_DISPLAY_SERVICE_OP_PRESENT_RECT, JCOS_DISPLAY_SERVICE_PROTOCOL_VERSION);
    request.words[1] = slot;
    request.words[2] = JCOS_DISPLAY_SURFACE_POSITION(config->x, config->y);
    request.words[3] = JCOS_DISPLAY_SURFACE_EXTENT(config->width, config->height);
    return display_call(command_cap, reply_cap, display_incarnation, &request, &reply) &&
        reply.word_count == 4U && reply.words[0] == JCOS_DISPLAY_SERVICE_REPLY_RECT_PRESENTED &&
        reply.words[2] == JCOS_DISPLAY_SERVICE_PROTOCOL_VERSION && reply.words[3] == slot;
}

static int display_sample(JcosCapabilityHandle command_cap, JcosCapabilityHandle reply_cap,
    JcosU64 display_incarnation, JcosU32 *out_sample) {
    if (!out_sample) return 0;
    *out_sample = 0U;
    JcosIpcMessage request;
    JcosIpcMessage reply;
    clear_message(&request);
    request.word_count = 2U;
    request.words[0] = JCOS_DISPLAY_SERVICE_HEADER(
        JCOS_DISPLAY_SERVICE_OP_SAMPLE, JCOS_DISPLAY_SERVICE_PROTOCOL_VERSION);
    if (!display_call(command_cap, reply_cap, display_incarnation, &request, &reply) ||
        reply.word_count != 4U || reply.words[0] != JCOS_DISPLAY_SERVICE_REPLY_SAMPLED ||
        reply.words[2] != JCOS_DISPLAY_SERVICE_PROTOCOL_VERSION) return 0;
    *out_sample = (JcosU32)reply.words[3];
    return 1;
}

static int compose(JcosCapabilityHandle display_command, JcosCapabilityHandle display_reply,
    JcosU64 display_incarnation, const CompositorSlot *slots, JcosU32 *out_count,
    JcosU32 *out_sample) {
    if (!slots || !out_count || !out_sample ||
        !display_clear(display_command, display_reply, display_incarnation)) return 0;

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
        if (!display_present(display_command, display_reply, display_incarnation, best, &slots[best])) return 0;
        drawn[best] = 1U;
        ++count;
    }

    if (!display_sample(display_command, display_reply, display_incarnation, out_sample)) return 0;
    *out_count = count;
    return 1;
}

__attribute__((noreturn))
void jcos_main(const JcosProgramStartup *startup) {
    if (!startup_valid(startup)) jcos_thread_exit();

    JcosCapabilityHandle command_cap = startup->capabilities[0];
    JcosCapabilityHandle reply_cap = startup->capabilities[1];
    JcosCapabilityHandle display_command = startup->capabilities[2];
    JcosCapabilityHandle display_reply = startup->capabilities[3];
    JcosU64 incarnation = startup->arguments[0];
    JcosU64 display_incarnation = startup->arguments[1];

    CompositorSlot slots[JCOS_DISPLAY_SURFACE_CAPACITY];
    for (JcosU32 i = 0U; i < JCOS_DISPLAY_SURFACE_CAPACITY; ++i) remove_slot(slots, i);
    JcosU32 focused_slot = JCOS_DISPLAY_SURFACE_FOCUS_NONE;

    for (;;) {
        JcosIpcMessage request;
        clear_message(&request);
        if (!jcos_ipc_receive_blocking(command_cap, &request)) jcos_thread_exit();
        if (request.word_count < 2U) {
            if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, request.word_count)) jcos_thread_exit();
            continue;
        }

        JcosU64 header = request.words[0];
        JcosU64 op = JCOS_COMPOSITOR_SERVICE_HEADER_OP(header);
        JcosU64 version = JCOS_COMPOSITOR_SERVICE_HEADER_VERSION(header);
        if (version != JCOS_COMPOSITOR_SERVICE_PROTOCOL_VERSION) {
            if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INCOMPATIBLE_VERSION, incarnation, version)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_COMPOSITOR_SERVICE_OP_PING) {
            if (request.word_count != 3U) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op)) jcos_thread_exit();
                continue;
            }
            if (!send_reply(reply_cap, JCOS_COMPOSITOR_SERVICE_REPLY_PONG, incarnation, request.words[2])) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_COMPOSITOR_SERVICE_OP_SURFACE_CONFIG) {
            if (request.word_count != 4U || !configure_slot(slots,
                    request.words[1], request.words[2], request.words[3])) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op)) jcos_thread_exit();
                continue;
            }
            JcosU32 slot = JCOS_DISPLAY_SURFACE_CONFIG_SLOT(request.words[1]);
            if (!send_reply(reply_cap, JCOS_COMPOSITOR_SERVICE_REPLY_CONFIGURED, incarnation, slot)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_COMPOSITOR_SERVICE_OP_SURFACE_REMOVE) {
            if (request.word_count != 3U || request.words[2] >= JCOS_DISPLAY_SURFACE_CAPACITY) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op)) jcos_thread_exit();
                continue;
            }
            JcosU32 slot = (JcosU32)request.words[2];
            remove_slot(slots, slot);
            if (focused_slot == slot) focused_slot = JCOS_DISPLAY_SURFACE_FOCUS_NONE;
            if (!send_reply(reply_cap, JCOS_COMPOSITOR_SERVICE_REPLY_REMOVED, incarnation, slot)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_COMPOSITOR_SERVICE_OP_FOCUS_SURFACE) {
            if (request.word_count != 3U || request.words[2] > 0xFFFFFFFFULL) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op)) jcos_thread_exit();
                continue;
            }
            JcosU32 requested = (JcosU32)request.words[2];
            if (requested != JCOS_DISPLAY_SURFACE_FOCUS_NONE &&
                (requested >= JCOS_DISPLAY_SURFACE_CAPACITY || !slots[requested].active)) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op)) jcos_thread_exit();
                continue;
            }
            focused_slot = requested;
            if (!send_reply(reply_cap, JCOS_COMPOSITOR_SERVICE_REPLY_FOCUSED, incarnation, focused_slot)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_COMPOSITOR_SERVICE_OP_COMPOSE) {
            JcosU32 count = 0U;
            JcosU32 sample = 0U;
            if (request.word_count != 2U || !compose(display_command, display_reply,
                    display_incarnation, slots, &count, &sample)) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op)) jcos_thread_exit();
                continue;
            }
            if (!send_reply(reply_cap, JCOS_COMPOSITOR_SERVICE_REPLY_COMPOSED, incarnation, JCOS_DISPLAY_COMPOSE_DETAIL(count, sample))) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_COMPOSITOR_SERVICE_OP_DIAG_FAULT) {
            if (request.word_count != 2U) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op)) jcos_thread_exit();
                continue;
            }
            __asm__ volatile ("ud2");
            __builtin_unreachable();
        }

        if (op == JCOS_COMPOSITOR_SERVICE_OP_SHUTDOWN) {
            if (request.word_count != 2U) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST, incarnation, op)) jcos_thread_exit();
                continue;
            }
            if (!send_reply(reply_cap, JCOS_COMPOSITOR_SERVICE_REPLY_STOPPED, incarnation, 0ULL)) jcos_thread_exit();
            jcos_thread_exit();
        }

        if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_UNKNOWN_OPERATION, incarnation, op)) jcos_thread_exit();
    }
}
