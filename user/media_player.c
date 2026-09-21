#include "lib/console.h"
#include "lib/syscall.h"
#include "../include/console_client_protocol.h"
#include "../include/media_abi.h"
#include "../include/program_startup.h"

static unsigned char g_frame[JCOS_MEDIA_MAX_WIDTH * JCOS_MEDIA_MAX_HEIGHT];
static short g_audio[JCOS_MEDIA_AUDIO_SUBMIT_MAX / sizeof(short)];

static int startup_valid(const JcosProgramStartup *startup) {
    return startup && startup->magic == JCOS_PROGRAM_STARTUP_MAGIC &&
        startup->version == JCOS_PROGRAM_STARTUP_VERSION &&
        startup->size == sizeof(JcosProgramStartup) &&
        startup->flags == JCOS_PROGRAM_STARTUP_FLAG_NONE &&
        startup->capability_count == 4U && startup->argument_count == 4U &&
        startup->environment_count == 0U &&
        startup->capabilities[0] && startup->capabilities[1] &&
        startup->capabilities[2] && startup->capabilities[3] &&
        startup->arguments[0] == JCOS_CONSOLE_CLIENT_PROTOCOL_VERSION &&
        startup->arguments[1] && startup->arguments[2] && startup->arguments[3];
}

static int range_valid(JcosU64 offset, JcosU64 size, JcosU64 total) {
    return offset <= total && size <= total - offset;
}

static int decode_frame(const unsigned char *encoded, JcosU32 encoded_size,
    JcosU32 pixel_count) {
    JcosU32 input = 0U;
    JcosU32 output = 0U;
    while (input < encoded_size && output < pixel_count) {
        unsigned char command = encoded[input++];
        if (command < 0x80U) {
            JcosU32 count = (JcosU32)command + 1U;
            if (count > encoded_size - input || count > pixel_count - output) return 0;
            for (JcosU32 i = 0; i < count; ++i) g_frame[output++] = encoded[input++];
        } else if (command < 0xC0U) {
            JcosU32 count = ((JcosU32)command & 0x3FU) + 1U;
            if (count > pixel_count - output) return 0;
            output += count;
        } else {
            JcosU32 count = ((JcosU32)command & 0x3FU) + 1U;
            if (input >= encoded_size || count > pixel_count - output) return 0;
            unsigned char value = encoded[input++];
            for (JcosU32 i = 0; i < count; ++i) g_frame[output++] = value;
        }
    }
    return input == encoded_size && output == pixel_count;
}

static int submit_audio(JcosCapabilityHandle audio, const unsigned char *samples,
    JcosU32 count, JcosU64 tick_frequency) {
    JcosU32 consumed = 0U;
    while (consumed < count) {
        JcosU32 frames = count - consumed;
        JcosU32 capacity = JCOS_MEDIA_AUDIO_SUBMIT_MAX / 4U;
        if (frames > capacity) frames = capacity;
        for (JcosU32 i = 0; i < frames; ++i) {
            short sample = (short)(((int)samples[consumed + i] - 128) << 8);
            g_audio[i * 2U] = sample;
            g_audio[i * 2U + 1U] = sample;
        }
        JcosU32 bytes = frames * 4U;
        JcosU64 started = jcos_clock_ticks();
        while (!jcos_audio_write(audio, g_audio, bytes)) {
            if ((JcosU64)(jcos_clock_ticks() - started) > tick_frequency * 2ULL) return 0;
            __asm__ volatile ("pause");
        }
        consumed += frames;
    }
    return 1;
}

static void finish(JcosCapabilityHandle output, JcosU64 session_id, const char *error) {
    if (error) (void)jcos_console_writeln(output, session_id, error);
    (void)jcos_console_end(output, session_id);
}

__attribute__((noreturn))
void jcos_main(const JcosProgramStartup *startup) {
    if (!startup_valid(startup)) jcos_thread_exit();

    JcosCapabilityHandle output = startup->capabilities[0];
    JcosCapabilityHandle display = startup->capabilities[2];
    JcosCapabilityHandle audio = startup->capabilities[3];
    JcosU64 session_id = startup->arguments[1];
    const unsigned char *media = (const unsigned char *)(JcosU64)startup->arguments[2];
    JcosU64 media_size = startup->arguments[3];

    if (media_size < sizeof(JcosMediaHeader)) {
        finish(output, session_id, "media: truncated header");
        jcos_thread_exit();
    }
    const JcosMediaHeader *header = (const JcosMediaHeader *)(const void *)media;
    JcosU64 pixel_count = (JcosU64)header->width * header->height;
    if (header->magic != JCOS_MEDIA_MAGIC || header->version != JCOS_MEDIA_VERSION ||
        header->header_size != sizeof(JcosMediaHeader) ||
        !header->width || header->width > JCOS_MEDIA_MAX_WIDTH ||
        !header->height || header->height > JCOS_MEDIA_MAX_HEIGHT ||
        !header->frames_per_second || !header->frame_count ||
        header->sample_rate != JCOS_MEDIA_AUDIO_RATE ||
        header->pixel_format != JCOS_MEDIA_PIXEL_RGB332 ||
        header->audio_format != JCOS_MEDIA_AUDIO_PCM_U8_MONO ||
        header->payload_size != media_size - sizeof(JcosMediaHeader) ||
        pixel_count > sizeof(g_frame)) {
        finish(output, session_id, "media: unsupported stream");
        jcos_thread_exit();
    }

    JcosU32 screen_width = 0U;
    JcosU32 screen_height = 0U;
    JcosU32 sample_rate = 0U;
    JcosU32 submit_max = 0U;
    JcosU64 frequency = jcos_clock_frequency();
    if (!frequency || !jcos_display_info(display, &screen_width, &screen_height) ||
        !screen_width || !screen_height ||
        !jcos_audio_info(audio, &sample_rate, &submit_max) ||
        sample_rate != header->sample_rate || submit_max < 4U) {
        finish(output, session_id, "media: display or audio unavailable");
        jcos_thread_exit();
    }

    JcosU64 offset = sizeof(JcosMediaHeader);
    JcosU64 started = jcos_clock_ticks();
    for (JcosU32 frame = 0U; frame < header->frame_count; ++frame) {
        if (!range_valid(offset, sizeof(JcosMediaFrameHeader), media_size)) {
            finish(output, session_id, "media: truncated frame table");
            jcos_thread_exit();
        }
        const JcosMediaFrameHeader *chunk =
            (const JcosMediaFrameHeader *)(const void *)(media + offset);
        offset += sizeof(*chunk);
        if (!range_valid(offset, chunk->video_size, media_size) ||
            !decode_frame(media + offset, chunk->video_size, (JcosU32)pixel_count)) {
            finish(output, session_id, "media: invalid video frame");
            jcos_thread_exit();
        }
        offset += chunk->video_size;
        if (!range_valid(offset, chunk->audio_samples, media_size) ||
            !submit_audio(audio, media + offset, chunk->audio_samples, frequency)) {
            finish(output, session_id, "media: audio device stalled");
            jcos_thread_exit();
        }
        offset += chunk->audio_samples;

        if (!jcos_display_present(display, g_frame, header->width, header->height)) {
            finish(output, session_id, "media: display rejected frame");
            jcos_thread_exit();
        }

        JcosU64 target = started +
            ((JcosU64)(frame + 1U) * frequency) / header->frames_per_second;
        while ((JcosU64)(jcos_clock_ticks() - target) > (1ULL << 63))
            __asm__ volatile ("pause");
    }

    finish(output, session_id, 0);
    jcos_thread_exit();
}