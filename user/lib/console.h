#ifndef JCOS_USER_CONSOLE_H
#define JCOS_USER_CONSOLE_H

#include "../../include/user_abi.h"
#include "../../include/console_input_protocol.h"

typedef struct {
    JcosU64 key;
    char character;
    int shift;
    int ctrl;
    int alt;
} JcosConsoleInputEvent;

int jcos_console_write(JcosCapabilityHandle output, JcosU64 session_id, const char *text);
int jcos_console_writeln(JcosCapabilityHandle output, JcosU64 session_id, const char *text);
int jcos_console_end(JcosCapabilityHandle output, JcosU64 session_id);
int jcos_console_receive_input(JcosCapabilityHandle input, JcosU64 session_id, JcosConsoleInputEvent *event);

#endif