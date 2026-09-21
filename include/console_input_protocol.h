#ifndef JCOS_CONSOLE_INPUT_PROTOCOL_H
#define JCOS_CONSOLE_INPUT_PROTOCOL_H

/* Public foreground-input protocol. The privileged input broker owns SEND;
 * a focused application receives only RECEIVE authority to the input endpoint. */
#define JCOS_CONSOLE_INPUT_PROTOCOL_VERSION 1ULL
#define JCOS_CONSOLE_INPUT_OP_KEY_EVENT     1ULL

/* word 0: bits 0..15 operation, 16..31 version. */
#define JCOS_CONSOLE_INPUT_HEADER(op, version) \
    (((op) & 0xFFFFULL) | (((version) & 0xFFFFULL) << 16))
#define JCOS_CONSOLE_INPUT_HEADER_OP(value) ((value) & 0xFFFFULL)
#define JCOS_CONSOLE_INPUT_HEADER_VERSION(value) (((value) >> 16) & 0xFFFFULL)

/* Public key vocabulary. This is deliberately independent of the kernel's
 * internal KeyCode enum so applications never depend on kernel enum layout. */
#define JCOS_CONSOLE_INPUT_KEY_CHARACTER 1ULL
#define JCOS_CONSOLE_INPUT_KEY_ENTER     2ULL
#define JCOS_CONSOLE_INPUT_KEY_BACKSPACE 3ULL
#define JCOS_CONSOLE_INPUT_KEY_TAB       4ULL
#define JCOS_CONSOLE_INPUT_KEY_ESCAPE    5ULL
#define JCOS_CONSOLE_INPUT_KEY_UP        6ULL
#define JCOS_CONSOLE_INPUT_KEY_DOWN      7ULL
#define JCOS_CONSOLE_INPUT_KEY_LEFT      8ULL
#define JCOS_CONSOLE_INPUT_KEY_RIGHT     9ULL
#define JCOS_CONSOLE_INPUT_KEY_HOME      10ULL
#define JCOS_CONSOLE_INPUT_KEY_END       11ULL
#define JCOS_CONSOLE_INPUT_KEY_DELETE    12ULL
#define JCOS_CONSOLE_INPUT_KEY_PAGE_UP   13ULL
#define JCOS_CONSOLE_INPUT_KEY_PAGE_DOWN 14ULL

#define JCOS_CONSOLE_INPUT_EVENT_CHARACTER_MASK 0xFFULL
#define JCOS_CONSOLE_INPUT_EVENT_SHIFT          (1ULL << 8)
#define JCOS_CONSOLE_INPUT_EVENT_CTRL           (1ULL << 9)
#define JCOS_CONSOLE_INPUT_EVENT_ALT            (1ULL << 10)

#endif
