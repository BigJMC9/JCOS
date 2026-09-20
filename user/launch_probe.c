#include "lib/syscall.h"
#include "../include/program_startup.h"
#include "../include/launch_probe_protocol.h"

static int startup_valid(const JcosProgramStartup *startup) {
    return startup && startup->magic == JCOS_PROGRAM_STARTUP_MAGIC &&
        startup->version == JCOS_PROGRAM_STARTUP_VERSION &&
        startup->size == sizeof(JcosProgramStartup) &&
        startup->flags == JCOS_PROGRAM_STARTUP_FLAG_NONE &&
        startup->capability_count == 1U && startup->argument_count == 1U &&
        startup->environment_count == 0U &&
        startup->capabilities[0] != JCOS_CAPABILITY_INVALID_HANDLE;
}

__attribute__((noreturn))
void jcos_main(const JcosProgramStartup *startup) {
    if (!startup_valid(startup)) jcos_thread_exit();

    JcosIpcMessage message;
    message.words[0] = JCOS_LAUNCH_PROBE_MESSAGE_READY;
    message.words[1] = startup->arguments[0];
    message.words[2] = startup->version;
    message.words[3] = startup->capability_count;
    message.word_count = 4U;

    if (!jcos_ipc_try_send(startup->capabilities[0], &message)) jcos_thread_exit();
    jcos_thread_exit();
}
