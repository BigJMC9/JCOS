#include "lib/syscall.h"
#include "../include/ordinary_service_protocol.h"
#include "../include/program_startup.h"
#include "../include/service_protocol.h"

#ifndef JCOS_ORDINARY_SERVICE_IMAGE_ID
#define JCOS_ORDINARY_SERVICE_IMAGE_ID JCOS_ORDINARY_SERVICE_IMAGE_PRIMARY
#endif

static void clear_message(JcosIpcMessage *message) {
    if (!message) return;
    for (JcosU32 i = 0; i < JCOS_IPC_MESSAGE_MAX_WORDS; ++i) message->words[i] = 0ULL;
    message->word_count = 0U;
}

static int startup_valid(const JcosProgramStartup *startup) {
    return startup && startup->magic == JCOS_PROGRAM_STARTUP_MAGIC &&
        startup->version == JCOS_PROGRAM_STARTUP_VERSION &&
        startup->size == sizeof(JcosProgramStartup) &&
        startup->flags == JCOS_PROGRAM_STARTUP_FLAG_NONE &&
        startup->capability_count == 2U && startup->argument_count == 1U &&
        startup->environment_count == 0U && startup->arguments[0] != 0ULL &&
        startup->capabilities[0] != JCOS_CAPABILITY_INVALID_HANDLE &&
        startup->capabilities[1] != JCOS_CAPABILITY_INVALID_HANDLE;
}

static int send_reply(JcosCapabilityHandle reply_cap, JcosU64 code,
    JcosU64 incarnation, JcosU64 detail, JcosU32 words) {
    JcosIpcMessage reply;
    clear_message(&reply);
    reply.word_count = words;
    reply.words[0] = code;
    reply.words[1] = incarnation;
    reply.words[2] = JCOS_ORDINARY_SERVICE_PROTOCOL_VERSION;
    reply.words[3] = detail;
    return jcos_ipc_send_blocking(reply_cap, &reply);
}

__attribute__((noreturn))
void jcos_main(const JcosProgramStartup *startup) {
    if (!startup_valid(startup)) jcos_thread_exit();

    JcosCapabilityHandle command_cap = startup->capabilities[0];
    JcosCapabilityHandle reply_cap = startup->capabilities[1];
    JcosU64 incarnation = startup->arguments[0];

    for (;;) {
        JcosIpcMessage request;
        clear_message(&request);
        if (!jcos_ipc_receive_blocking(command_cap, &request)) jcos_thread_exit();
        if (request.word_count < 2U) {
            if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST,
                    incarnation, request.word_count, 4U)) jcos_thread_exit();
            continue;
        }

        JcosU64 header = request.words[0];
        JcosU64 op = JCOS_ORDINARY_SERVICE_HEADER_OP(header);
        JcosU64 version = JCOS_ORDINARY_SERVICE_HEADER_VERSION(header);
        if (version != JCOS_ORDINARY_SERVICE_PROTOCOL_VERSION) {
            if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INCOMPATIBLE_VERSION,
                    incarnation, version, 4U)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_ORDINARY_SERVICE_OP_PING) {
            if (request.word_count != 3U) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST,
                        incarnation, op, 4U)) jcos_thread_exit();
                continue;
            }
            if (!send_reply(reply_cap, JCOS_ORDINARY_SERVICE_REPLY_PONG,
                    incarnation, request.words[2], 4U)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_ORDINARY_SERVICE_OP_SHUTDOWN) {
            if (request.word_count != 2U) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST,
                        incarnation, op, 4U)) jcos_thread_exit();
                continue;
            }
            if (!send_reply(reply_cap, JCOS_ORDINARY_SERVICE_REPLY_STOPPED,
                    incarnation, 0ULL, 3U)) jcos_thread_exit();
            jcos_thread_exit();
        }

        if (op == JCOS_ORDINARY_SERVICE_OP_IDENTIFY) {
            if (request.word_count != 2U) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST,
                        incarnation, op, 4U)) jcos_thread_exit();
                continue;
            }
            if (!send_reply(reply_cap, JCOS_ORDINARY_SERVICE_REPLY_IDENTITY,
                    incarnation, JCOS_ORDINARY_SERVICE_IMAGE_ID, 4U)) jcos_thread_exit();
            continue;
        }

        if (op == JCOS_ORDINARY_SERVICE_OP_DIAG_FAULT) {
            if (request.word_count != 2U) {
                if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_INVALID_REQUEST,
                        incarnation, op, 4U)) jcos_thread_exit();
                continue;
            }
            __asm__ volatile ("ud2");
            jcos_thread_exit();
        }

        if (!send_reply(reply_cap, JCOS_SERVICE_REPLY_UNKNOWN_OPERATION,
                incarnation, op, 4U)) jcos_thread_exit();
    }
}