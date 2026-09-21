#include "lib/syscall.h"
#include "../include/program_startup.h"
#include "../include/worker_service_protocol.h"

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

__attribute__((noreturn))
void jcos_main(const JcosProgramStartup *startup) {
    if (!startup_valid(startup)) jcos_thread_exit();

    JcosCapabilityHandle command_cap = startup->capabilities[0];
    JcosCapabilityHandle reply_cap = startup->capabilities[1];
    JcosU64 incarnation = startup->arguments[0];

    for (;;) {
        JcosIpcMessage request;
        clear_message(&request);
        if (!jcos_ipc_receive_blocking(command_cap, &request) || !request.word_count) jcos_thread_exit();

        if (request.words[0] == WORKER_SERVICE_MESSAGE_PING) {
            if (request.word_count != 2U) jcos_thread_exit();
            JcosIpcMessage reply;
            clear_message(&reply);
            reply.word_count = 3U;
            reply.words[0] = WORKER_SERVICE_REPLY_PONG;
            reply.words[1] = incarnation;
            reply.words[2] = request.words[1];
            if (!jcos_ipc_send_blocking(reply_cap, &reply)) jcos_thread_exit();
            continue;
        }

        if (request.words[0] == WORKER_SERVICE_MESSAGE_SHUTDOWN) {
            if (request.word_count != 1U) jcos_thread_exit();
            JcosIpcMessage reply;
            clear_message(&reply);
            reply.word_count = 2U;
            reply.words[0] = WORKER_SERVICE_REPLY_STOPPED;
            reply.words[1] = incarnation;
            if (!jcos_ipc_send_blocking(reply_cap, &reply)) jcos_thread_exit();
            jcos_thread_exit();
        }

        if (request.words[0] == WORKER_SERVICE_MESSAGE_FAULT) {
            if (request.word_count != 1U) jcos_thread_exit();
            __asm__ volatile ("ud2");
            jcos_thread_exit();
        }

        if (request.words[0] == WORKER_SERVICE_MESSAGE_HANG) {
            if (request.word_count != 1U) jcos_thread_exit();
            for (;;) __asm__ volatile ("pause" : : : "memory");
        }

        jcos_thread_exit();
    }
}