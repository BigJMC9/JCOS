#ifndef JA_OS_DEVICE_RESOURCE_H
#define JA_OS_DEVICE_RESOURCE_H

#include "types.h"

typedef enum {
    DEVICE_RESOURCE_NONE = 0,
    DEVICE_RESOURCE_DISPLAY,
    DEVICE_RESOURCE_AUDIO
} DeviceResourceKind;

typedef struct {
    u64 id;
    u64 capability_refs;
    DeviceResourceKind kind;
    bool initialized;
} DeviceResource;

bool device_resource_create(DeviceResource *resource, DeviceResourceKind kind);
bool device_resource_destroy(DeviceResource *resource);
bool device_resource_valid(const DeviceResource *resource);

#endif
