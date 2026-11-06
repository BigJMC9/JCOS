#ifndef JA_OS_DEVICE_REGISTER_READER_H
#define JA_OS_DEVICE_REGISTER_READER_H
#include "device_resource.h"
#include "process.h"
#include "polaris_capture.h"
#include "../include/amd_polaris_registers.h"
#include "../include/device_register_abi.h"

#define DEVICE_REGISTER_READER_CAPACITY 8U
#define DEVICE_REGISTER_TEST_VALUES (POLARIS_DISPLAY_PIPE_COUNT * POLARIS_DISPLAY_REGISTER_COUNT)

typedef struct {
    DeviceResource resource; /* First: capability object identity. */
    u64 owner_process_id;
    u64 successful_reads;
    u32 profile;
    PolarisCaptureBudget capture;
    u32 values[DEVICE_REGISTER_TEST_VALUES];
    bool revoked;
    bool initialized;
} DeviceRegisterReader;

/* Kernel diagnostic constructor only. Copies a register bank into protected
 * storage; grants READ without WRITE/MANAGE/TRANSFER. No physical constructor,
 * PCI mapping, read callback or user-created resource exists in this pass. */
bool device_register_reader_create_test_bank(DeviceRegisterReader *reader,
    Process *owner, u32 profile, const u32 *values, u32 count, CapabilityHandle *out);
/* One-shot copied-RAM diagnostic resource, not a physical read grant. The
 * duration is in current timer ticks, nonzero and at most two seconds.
 * Revoke, completion, timeout, wrong sequence or an all-ones value seals it.
 * A new capture requires destruction and a fresh capability/resource ID. */
bool device_register_reader_create_test_capture(DeviceRegisterReader *reader,
    Process *owner, u32 pipe, const u32 *values, u32 count, u64 duration,
    CapabilityHandle *out);
/* Trusted immutable observation producer. Copies fifteen previously captured
 * fields; the capability reads RAM with finite ordering, not live registers. */
bool device_register_reader_create_observation(DeviceRegisterReader *reader,
    Process *owner, u32 pipe, const u32 *values, u64 duration, CapabilityHandle *out);
bool device_register_reader_valid(const DeviceRegisterReader *reader);
bool device_register_reader_revoke(DeviceRegisterReader *reader);
bool device_register_reader_destroy(DeviceRegisterReader *reader);
u32 device_register_reader_object_count(void);
bool device_register_reader_read(Process *owner, CapabilityHandle handle,
    u64 expected_identity, u64 request, u32 *out_value);
_Static_assert(__builtin_offsetof(DeviceRegisterReader, resource) == 0, "Register reader capability object");
#endif
