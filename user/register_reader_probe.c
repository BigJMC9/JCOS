/* Diagnostic client: actual INT 0x80 reads, no GPU or framebuffer mapping. */
#include "lib/syscall.h"
#include "drivers/amd_polaris_display.h"
#include "../include/program_startup.h"
#include "../include/register_probe_protocol.h"

static void clear_message(JcosIpcMessage *m) {
    for (JcosU32 i = 0; i < JCOS_IPC_MESSAGE_MAX_WORDS; ++i) m->words[i] = 0ULL;
    m->word_count = 0U;
}
static int refused(JcosCapabilityHandle cap, JcosU64 identity, JcosU64 request) {
    JcosU32 value = ~0U;
    return !jcos_device_register_read(cap, identity, request, &value) && !value;
}
static JcosU64 check_reader(JcosCapabilityHandle cap, JcosU64 identity,
    JcosU64 profile, JcosCapabilityHandle endpoint) {
    if (profile != JCOS_REGISTER_PROFILE_POLARIS_DCE112_TEST &&
        profile != JCOS_REGISTER_PROFILE_POLARIS_DCE112_CAPTURE_TEST) return 0;
    JcosU64 result = 0;
    PolarisDisplaySnapshot snapshot;
    PolarisDisplayObservation decoded;
    if (polaris_display_capture_protected(cap, identity, 3U, 0x40000ULL, &snapshot) ==
            POLARIS_CAPTURE_OK && polaris_display_decode(&snapshot, &decoded) &&
        !decoded.restrictions && decoded.surface_gpu_address == 0x100200000ULL &&
        decoded.surface_width == 1024U && decoded.surface_height == 768U &&
        decoded.linear_span_bytes == 0x300000ULL) result |= 1ULL;
    JcosU64 valid = JCOS_REGISTER_READ_REQUEST(3U, POLARIS_REG_PITCH);
    if (refused(cap, identity + 1ULL, valid)) result |= 2ULL;
    if (refused(0, identity, valid) && refused(endpoint, identity, valid)) result |= 4ULL;
    if (refused(cap, identity, (2ULL << 32) | (valid & 0xFFFFFFFFULL))) result |= 8ULL;
    if (refused(cap, identity, JCOS_REGISTER_READ_REQUEST(6U, POLARIS_REG_PITCH))) result |= 16ULL;
    if (refused(cap, identity, JCOS_REGISTER_READ_REQUEST(0U, POLARIS_DISPLAY_REGISTER_COUNT))) result |= 32ULL;
    if (refused(cap, 0, valid)) result |= 64ULL;
    if (refused(cap, identity, 0) && refused(cap, identity, ~0ULL) &&
        refused(cap, identity, JCOS_REGISTER_READ_REQUEST(65536ULL, POLARIS_REG_PITCH)) &&
        refused(cap, identity, JCOS_REGISTER_READ_REQUEST(0U, 65536ULL))) result |= 128ULL;
    return result;
}
__attribute__((noreturn))
void jcos_main(const JcosProgramStartup *startup) {
    if (!startup || startup->magic != JCOS_PROGRAM_STARTUP_MAGIC ||
        startup->version != JCOS_PROGRAM_STARTUP_VERSION || startup->size != sizeof(*startup) ||
        startup->flags || startup->environment_count || startup->capability_count != 2U ||
        startup->argument_count != 1U || !startup->arguments[0] ||
        !startup->capabilities[0] || !startup->capabilities[1]) jcos_thread_exit();
    for (;;) {
        JcosIpcMessage request;
        clear_message(&request);
        if (!jcos_ipc_receive_blocking(startup->capabilities[0], &request)) jcos_thread_exit();
        JcosIpcMessage reply;
        clear_message(&reply);
        reply.word_count = 4U;
        reply.words[0] = REGISTER_PROBE_REPLY;
        reply.words[1] = startup->arguments[0];
        if (request.word_count == 4U && request.words[0] == REGISTER_PROBE_CHECK) {
            reply.words[2] = check_reader(request.words[1], request.words[2],
                request.words[3], startup->capabilities[0]);
        } else if (request.word_count == 4U && request.words[0] == REGISTER_PROBE_READ) {
            JcosU32 value = ~0U;
            reply.words[2] = jcos_device_register_read(request.words[1], request.words[2],
                request.words[3], &value) ? 1ULL : 0ULL;
            reply.words[3] = value;
        } else if (request.word_count == 1U && request.words[0] == REGISTER_PROBE_SHUTDOWN) {
            reply.words[0] = REGISTER_PROBE_STOPPED;
            if (!jcos_ipc_send_blocking(startup->capabilities[1], &reply)) jcos_thread_exit();
            jcos_thread_exit();
        } else if (request.word_count == 4U && request.words[0] == REGISTER_PROBE_SPIN) {
            JcosU32 value;
            (void)jcos_device_register_read(request.words[1], request.words[2],
                JCOS_REGISTER_READ_REQUEST(request.words[3], POLARIS_REG_CRTC_CONTROL), &value);
            for (;;) __asm__ volatile ("pause");
        } else if (request.word_count == 1U && request.words[0] == REGISTER_PROBE_FAULT) {
            __asm__ volatile ("ud2");
            __builtin_unreachable();
        } else jcos_thread_exit();
        if (!jcos_ipc_send_blocking(startup->capabilities[1], &reply)) jcos_thread_exit();
    }
}
