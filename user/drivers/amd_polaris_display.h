#ifndef JCOS_AMD_POLARIS_DISPLAY_H
#define JCOS_AMD_POLARIS_DISPLAY_H

#include "../../include/user_abi.h"

/* Decoder for a future qualified Polaris10/DCE11.2 read broker.
 * Offsets and observations grant no hardware or framebuffer authority.
 * Only synthetic readers are installed in the current display service. */
#include "../../include/amd_polaris_registers.h"
#define POLARIS_DISPLAY_SNAPSHOT_VERSION 1U

typedef enum {
    POLARIS_CAPTURE_OK = 0,
    POLARIS_CAPTURE_INVALID,
    POLARIS_CAPTURE_READ_FAILED,
    POLARIS_CAPTURE_CHANGED
} PolarisCaptureResult;

typedef struct {
    JcosU32 version;
    JcosU32 pipe;
    JcosU32 values[POLARIS_DISPLAY_REGISTER_COUNT];
} PolarisDisplaySnapshot;

/* Restrictions on the deliberately small linear 32-bit observation subset.
 * A zero mask is still not a verified output or a permission to access VRAM. */
#define POLARIS_OBSERVATION_INACTIVE       (1U << 0)
#define POLARIS_OBSERVATION_TILED          (1U << 1)
#define POLARIS_OBSERVATION_TRANSLATED     (1U << 2)
#define POLARIS_OBSERVATION_FORMAT         (1U << 3)
#define POLARIS_OBSERVATION_OFFSET         (1U << 4)
#define POLARIS_OBSERVATION_INTERLACED     (1U << 5)
#define POLARIS_OBSERVATION_GEOMETRY       (1U << 6)
#define POLARIS_OBSERVATION_ADDRESS        (1U << 7)
#define POLARIS_OBSERVATION_RESERVED       (1U << 8)
#define POLARIS_OBSERVATION_CONTROL        (1U << 9)

typedef struct {
    /* GPU display address, NEVER a CPU physical address or BAR offset. */
    JcosU64 surface_gpu_address;
    JcosU64 linear_span_bytes; /* Nonzero only for the limited decoded subset. */
    JcosU32 surface_width; /* GRPH extent, not CRTC/monitor mode timing. */
    JcosU32 surface_height;
    JcosU32 pitch_pixels;
    JcosU32 depth;
    JcosU32 format;
    JcosU32 array_mode;
    JcosU32 restrictions;
} PolarisDisplayObservation;

typedef int (*PolarisDisplayRead32)(void *context, JcosU32 byte_offset, JcosU32 *out);

/* Convert a semantic field to its byte offset; no caller-chosen raw register.
 * This checks arithmetic/span only, not PCI provenance, power or read safety. */
int polaris_display_register_offset(JcosU32 pipe, PolarisDisplayRegister field,
    JcosU64 mmio_span, JcosU32 *out_byte_offset);
/* Preflight the entire plan, then take two bounded samples (at most 30 reads).
 * A callback failure/changed sample leaves output zero. Equality is an
 * observation, not an atomic hardware snapshot or a stability guarantee. */
PolarisCaptureResult polaris_display_capture(JcosU32 pipe, JcosU64 mmio_span,
    PolarisDisplayRead32 read32, void *context, PolarisDisplaySnapshot *out);
/* Same bounded reader through the real semantic-read syscall. The supplied
 * span controls planning only; the kernel independently validates every call. */
PolarisCaptureResult polaris_display_capture_protected(JcosCapabilityHandle handle,
    JcosU64 identity, JcosU32 pipe, JcosU64 span, PolarisDisplaySnapshot *out);
int polaris_display_decode(const PolarisDisplaySnapshot *snapshot,
    PolarisDisplayObservation *out);

_Static_assert(sizeof(PolarisDisplaySnapshot) == 68U, "Polaris snapshot layout");
_Static_assert(sizeof(PolarisDisplayObservation) == 48U, "Polaris observation layout");
#endif
