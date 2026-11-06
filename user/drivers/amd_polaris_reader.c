#include "amd_polaris_display.h"
#include "../lib/syscall.h"
#include "../../include/device_register_abi.h"

typedef struct {
    JcosCapabilityHandle handle;
    JcosU64 identity;
    JcosU64 span;
    JcosU32 pipe;
} ProtectedReader;

static int read_field(void *context, JcosU32 offset, JcosU32 *out) {
    const ProtectedReader *r = (const ProtectedReader *)context;
    for (JcosU32 field = 0; field < POLARIS_DISPLAY_REGISTER_COUNT; ++field) {
        JcosU32 expected;
        if (polaris_display_register_offset(r->pipe, (PolarisDisplayRegister)field, r->span, &expected) &&
            expected == offset)
            return jcos_device_register_read(r->handle, r->identity,
                JCOS_REGISTER_READ_REQUEST(r->pipe, field), out);
    }
    return 0;
}
PolarisCaptureResult polaris_display_capture_protected(JcosCapabilityHandle handle,
    JcosU64 identity, JcosU32 pipe, JcosU64 span, PolarisDisplaySnapshot *out) {
    ProtectedReader reader = {handle, identity, span, pipe};
    return polaris_display_capture(pipe, span, read_field, &reader, out);
}
