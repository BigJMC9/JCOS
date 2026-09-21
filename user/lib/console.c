#include "console.h"
#include "syscall.h"
#include "../../include/console_client_protocol.h"
#include "../../include/console_input_protocol.h"

static void message_clear(JcosIpcMessage *message) {
    if (!message) return;
    for (JcosU32 i = 0; i < JCOS_IPC_MESSAGE_MAX_WORDS; ++i) message->words[i] = 0ULL;
    message->word_count = 0U;
}

static int send_chunk(JcosCapabilityHandle output, JcosU64 session_id,
    const char *bytes, JcosU32 count) {
    if (!output || !session_id || !bytes || !count || count > JCOS_CONSOLE_CLIENT_MAX_WRITE_BYTES) return 0;

    JcosIpcMessage message;
    message_clear(&message);
    message.word_count = 4U;
    message.words[0] = JCOS_CONSOLE_CLIENT_HEADER(
        JCOS_CONSOLE_CLIENT_OP_WRITE, JCOS_CONSOLE_CLIENT_PROTOCOL_VERSION, count);
    message.words[1] = session_id;

    for (JcosU32 i = 0; i < count; ++i) {
        JcosU32 word = i < 8U ? 2U : 3U;
        JcosU32 shift = (i & 7U) * 8U;
        message.words[word] |= ((JcosU64)(unsigned char)bytes[i]) << shift;
    }

    return jcos_ipc_send_blocking(output, &message);
}

int jcos_console_write(JcosCapabilityHandle output, JcosU64 session_id, const char *text) {
    if (!output || !session_id || !text) return 0;
    while (*text) {
        char chunk[JCOS_CONSOLE_CLIENT_MAX_WRITE_BYTES];
        JcosU32 count = 0U;
        while (count < JCOS_CONSOLE_CLIENT_MAX_WRITE_BYTES && *text) chunk[count++] = *text++;
        if (!send_chunk(output, session_id, chunk, count)) return 0;
    }
    return 1;
}

int jcos_console_writeln(JcosCapabilityHandle output, JcosU64 session_id, const char *text) {
    return jcos_console_write(output, session_id, text) && send_chunk(output, session_id, "\n", 1U);
}

int jcos_console_end(JcosCapabilityHandle output, JcosU64 session_id) {
    if (!output || !session_id) return 0;
    JcosIpcMessage message;
    message_clear(&message);
    message.word_count = 2U;
    message.words[0] = JCOS_CONSOLE_CLIENT_HEADER(
        JCOS_CONSOLE_CLIENT_OP_END, JCOS_CONSOLE_CLIENT_PROTOCOL_VERSION, 0U);
    message.words[1] = session_id;
    return jcos_ipc_send_blocking(output, &message);
}

int jcos_console_receive_input(JcosCapabilityHandle input, JcosU64 session_id, JcosConsoleInputEvent *event) {
    if (!input || !session_id || !event) return 0;

    for (;;) {
        JcosIpcMessage message;
        message_clear(&message);
        if (!jcos_ipc_receive_blocking(input, &message)) return 0;
        if (message.word_count != 4U) continue;

        JcosU64 header = message.words[0];
        if (JCOS_CONSOLE_INPUT_HEADER_OP(header) != JCOS_CONSOLE_INPUT_OP_KEY_EVENT ||
            JCOS_CONSOLE_INPUT_HEADER_VERSION(header) != JCOS_CONSOLE_INPUT_PROTOCOL_VERSION ||
            message.words[1] != session_id ||
            message.words[2] < JCOS_CONSOLE_INPUT_KEY_CHARACTER ||
            message.words[2] > JCOS_CONSOLE_INPUT_KEY_PAGE_DOWN) continue;

        JcosU64 packed = message.words[3];
        event->key = message.words[2];
        event->character = (char)(packed & JCOS_CONSOLE_INPUT_EVENT_CHARACTER_MASK);
        event->shift = (packed & JCOS_CONSOLE_INPUT_EVENT_SHIFT) != 0ULL;
        event->ctrl = (packed & JCOS_CONSOLE_INPUT_EVENT_CTRL) != 0ULL;
        event->alt = (packed & JCOS_CONSOLE_INPUT_EVENT_ALT) != 0ULL;
        return 1;
    }
}