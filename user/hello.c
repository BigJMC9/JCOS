#include "lib/console.h"
#include "lib/syscall.h"
#include "../include/console_client_protocol.h"
#include "../include/hello_app_abi.h"
#include "../include/program_startup.h"

static int startup_valid(const JcosProgramStartup *startup) {
    return startup && startup->magic == JCOS_PROGRAM_STARTUP_MAGIC &&
        startup->version == JCOS_PROGRAM_STARTUP_VERSION &&
        startup->size == sizeof(JcosProgramStartup) &&
        startup->flags == JCOS_PROGRAM_STARTUP_FLAG_NONE &&
        (startup->capability_count == 1U || startup->capability_count == 2U) &&
        startup->argument_count == 2U &&
        startup->environment_count == 0U &&
        startup->capabilities[0] != JCOS_CAPABILITY_INVALID_HANDLE &&
        (startup->capability_count == 1U || startup->capabilities[1] != JCOS_CAPABILITY_INVALID_HANDLE) &&
        startup->arguments[0] == JCOS_CONSOLE_CLIENT_PROTOCOL_VERSION &&
        startup->arguments[1] != 0ULL;
}

__attribute__((noreturn))
void jcos_main(const JcosProgramStartup *startup) {
    if (!startup_valid(startup)) jcos_thread_exit();

    JcosCapabilityHandle output = startup->capabilities[0];
    JcosU64 session_id = startup->arguments[1];
    if (jcos_console_writeln(output, session_id, JCOS_HELLO_APP_TEXT)) {
        (void)jcos_console_end(output, session_id);
    }
    jcos_thread_exit();
}