#ifndef JA_OS_DEVICE_RESOURCE_H
#define JA_OS_DEVICE_RESOURCE_H

#include "types.h"

typedef enum {
    DEVICE_RESOURCE_NONE = 0,
    DEVICE_RESOURCE_DISPLAY,
    DEVICE_RESOURCE_AUDIO,
    DEVICE_RESOURCE_PCI_FUNCTION,
    DEVICE_RESOURCE_PCI_MMIO_BAR,
    DEVICE_RESOURCE_PCI_IO_BAR,
    DEVICE_RESOURCE_PCI_IRQ,
    DEVICE_RESOURCE_PCI_DMA_BUFFER,
    DEVICE_RESOURCE_REGISTER_READER
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
u32 device_resource_object_count(void);

#endif
