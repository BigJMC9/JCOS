#include "lib/syscall.h"
#include "../include/program_startup.h"
#include "../include/supervisor_protocol.h"

static void clear_message(JcosIpcMessage *message) {
    if (!message) return;

    message->words[0] = 0;
    message->words[1] = 0;
    message->words[2] = 0;
    message->words[3] = 0;
    message->word_count = 0;
}

static int startup_valid(const JcosProgramStartup *startup) {
    return startup && startup->magic == JCOS_PROGRAM_STARTUP_MAGIC &&
        startup->version == JCOS_PROGRAM_STARTUP_VERSION &&
        startup->size == sizeof(JcosProgramStartup) &&
        startup->flags == JCOS_PROGRAM_STARTUP_FLAG_NONE &&
        startup->capability_count == 2U && startup->argument_count == 0U &&
        startup->environment_count == 0U &&
        startup->capabilities[0] != JCOS_CAPABILITY_INVALID_HANDLE &&
        startup->capabilities[1] != JCOS_CAPABILITY_INVALID_HANDLE;
}

__attribute__((noreturn))
void jcos_main(const JcosProgramStartup *startup) {
    if (!startup_valid(startup)) jcos_thread_exit();

    JcosCapabilityHandle command_cap = startup->capabilities[0];
    JcosCapabilityHandle reply_cap = startup->capabilities[1];

    for (;;) {
        JcosIpcMessage request;

        clear_message(&request);

        if (!jcos_ipc_receive_blocking(command_cap, &request)) {
            jcos_thread_exit();
        }

        if (!request.word_count) jcos_thread_exit();
        if (request.words[0] == SUPERVISOR_MESSAGE_PING) {
            /*
             * PING requires:
             *
             * word0 = command
             * word1 = cookie
             */
            if (request.word_count < 2U) jcos_thread_exit();

            JcosIpcMessage reply;
            clear_message(&reply);
            reply.word_count = 2U;
            reply.words[0] = SUPERVISOR_REPLY_PONG;
            reply.words[1] = request.words[1];

            if (!jcos_ipc_send_blocking(reply_cap, &reply)) {
                jcos_thread_exit();
            }
            continue;
        }

        if (request.words[0] == SUPERVISOR_MESSAGE_SHUTDOWN) {
            JcosIpcMessage reply;
            clear_message(&reply);
            reply.word_count = 1U;
            reply.words[0] = SUPERVISOR_REPLY_STOPPED;

            if (!jcos_ipc_send_blocking(reply_cap, &reply)) {
                jcos_thread_exit();
            }
            jcos_thread_exit();
        }

        /* Unknown supervisor protocol message. */
        jcos_thread_exit();
    }
}