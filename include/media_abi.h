#ifndef JCOS_MEDIA_ABI_H
#define JCOS_MEDIA_ABI_H

#include "user_abi.h"

#define JCOS_MEDIA_MAGIC 0x31414944454D434AULL /* "JCMEDIA1" */
#define JCOS_MEDIA_VERSION 1U
#define JCOS_MEDIA_PIXEL_RGB332 1U
#define JCOS_MEDIA_AUDIO_PCM_U8_MONO 1U

#define JCOS_MEDIA_MAX_WIDTH 320U
#define JCOS_MEDIA_MAX_HEIGHT 240U
#define JCOS_MEDIA_AUDIO_RATE 48000U
#define JCOS_MEDIA_AUDIO_SUBMIT_MAX 4096U

typedef struct {
    JcosU64 magic;
    JcosU32 version;
    JcosU32 header_size;
    JcosU32 width;
    JcosU32 height;
    JcosU32 frames_per_second;
    JcosU32 frame_count;
    JcosU32 sample_rate;
    JcosU32 pixel_format;
    JcosU32 audio_format;
    JcosU32 reserved;
    JcosU64 duration_microseconds;
    JcosU64 payload_size;
} JcosMediaHeader;

typedef struct {
    JcosU32 video_size;
    JcosU32 audio_samples;
} JcosMediaFrameHeader;

_Static_assert(sizeof(JcosMediaHeader) == 64U, "JcosMediaHeader ABI size");
_Static_assert(sizeof(JcosMediaFrameHeader) == 8U, "JcosMediaFrameHeader ABI size");

#endif
