#include "device_register_reader.h"
#include "capability.h"
#include "interrupts.h"
#include "lib.h"
#include "object_storage.h"
#include "timer.h"

static void *g_readers[DEVICE_REGISTER_READER_CAPACITY];
static u64 irq_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq; popq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}
static void irq_restore(u64 flags) { if (flags & (1ULL << 9)) interrupts_enable(); }

static bool valid_locked(const DeviceRegisterReader *r) {
    /* Membership precedes dereference, including objects obtained from a cap. */
    return r && object_storage_find(g_readers, DEVICE_REGISTER_READER_CAPACITY, r) <
        DEVICE_REGISTER_READER_CAPACITY && r->initialized && r->owner_process_id &&
        (r->profile == JCOS_REGISTER_PROFILE_POLARIS_DCE112_OBSERVATION ||
         r->profile == JCOS_REGISTER_PROFILE_POLARIS_DCE112_TEST ||
         r->profile == JCOS_REGISTER_PROFILE_POLARIS_DCE112_CAPTURE_TEST) &&
        device_resource_valid(&r->resource) && r->resource.kind == DEVICE_RESOURCE_REGISTER_READER;
}
static bool owner_live(Process *owner) {
    ProcessExitInfo exit;
    return owner && process_storage_in_use(owner) && owner->initialized &&
        !owner->kernel && owner->id && !process_exit_info(owner, &exit);
}
bool device_register_reader_valid(const DeviceRegisterReader *r) {
    u64 flags = irq_save();
    bool valid = valid_locked(r);
    irq_restore(flags);
    return valid;
}
u32 device_register_reader_object_count(void) {
    u64 flags = irq_save();
    u32 count = object_storage_count(g_readers, DEVICE_REGISTER_READER_CAPACITY);
    irq_restore(flags);
    return count;
}
static bool create_bank(DeviceRegisterReader *r, Process *owner,
    u32 profile, const u32 *values, u32 count, const PolarisCaptureBudget *capture,
    CapabilityHandle *out) {
    if (!out) return false;
    *out = CAPABILITY_INVALID_HANDLE;
    if (!r || !values || count != DEVICE_REGISTER_TEST_VALUES ||
        (profile != JCOS_REGISTER_PROFILE_POLARIS_DCE112_OBSERVATION &&
         profile != JCOS_REGISTER_PROFILE_POLARIS_DCE112_TEST &&
         profile != JCOS_REGISTER_PROFILE_POLARIS_DCE112_CAPTURE_TEST)) return false;
    u64 flags = irq_save();
    bool created = false;
    u32 slot = object_storage_empty(g_readers, DEVICE_REGISTER_READER_CAPACITY);
    if (slot >= DEVICE_REGISTER_READER_CAPACITY || !owner_live(owner) ||
        object_storage_find(g_readers, DEVICE_REGISTER_READER_CAPACITY, r) <
            DEVICE_REGISTER_READER_CAPACITY) goto done;
    CapabilityTable *caps = process_capabilities(owner);
    if (!caps) goto done;
    k_memset(r, 0, sizeof(*r));
    if (!device_resource_create(&r->resource, DEVICE_RESOURCE_REGISTER_READER)) goto done;
    r->owner_process_id = owner->id;
    r->profile = profile;
    if (capture) k_memcpy(&r->capture, capture, sizeof(r->capture));
    k_memcpy(r->values, values, sizeof(r->values));
    r->initialized = true;
    g_readers[slot] = r;
    if (!capability_insert(caps, &r->resource, CAPABILITY_TYPE_DEVICE_RESOURCE,
            CAPABILITY_RIGHT_READ, out)) {
        r->revoked = true;
        /* Keep a cleanup record if object destruction cannot complete. */
        if (device_resource_destroy(&r->resource)) {
            g_readers[slot] = 0;
            k_memset(r, 0, sizeof(*r));
        }
        goto done;
    }
    created = true;
done:
    irq_restore(flags);
    return created;
}
bool device_register_reader_create_test_bank(DeviceRegisterReader *r, Process *owner,
    u32 profile, const u32 *values, u32 count, CapabilityHandle *out) {
    if (profile != JCOS_REGISTER_PROFILE_POLARIS_DCE112_TEST) {
        if (out) *out = CAPABILITY_INVALID_HANDLE;
        return false;
    }
    return create_bank(r, owner, profile, values, count, 0, out);
}
bool device_register_reader_create_test_capture(DeviceRegisterReader *r, Process *owner,
    u32 pipe, const u32 *values, u32 count, u64 duration, CapabilityHandle *out) {
    if (out) *out = CAPABILITY_INVALID_HANDLE;
    u64 flags = irq_save();
    PolarisCaptureBudget budget;
    bool valid = timer_initialized() && polaris_capture_budget_init(&budget, pipe,
        timer_frequency(), timer_ticks(), duration);
    bool created = valid && create_bank(r, owner, JCOS_REGISTER_PROFILE_POLARIS_DCE112_CAPTURE_TEST,
        values, count, &budget, out);
    irq_restore(flags);
    return created;
}
bool device_register_reader_create_observation(DeviceRegisterReader *r, Process *owner,
    u32 pipe, const u32 *values, u64 duration, CapabilityHandle *out) {
    if (out) *out = CAPABILITY_INVALID_HANDLE;
    if (!values || pipe >= POLARIS_DISPLAY_PIPE_COUNT) return false;
    u32 bank[DEVICE_REGISTER_TEST_VALUES];
    k_memset(bank, 0, sizeof(bank));
    k_memcpy(bank + pipe * POLARIS_DISPLAY_REGISTER_COUNT, values,
        POLARIS_DISPLAY_REGISTER_COUNT * sizeof(u32));
    u64 flags = irq_save();
    PolarisCaptureBudget budget;
    bool valid = timer_initialized() && polaris_capture_budget_init(&budget, pipe,
        timer_frequency(), timer_ticks(), duration);
    bool created = valid && create_bank(r, owner, JCOS_REGISTER_PROFILE_POLARIS_DCE112_OBSERVATION,
        bank, DEVICE_REGISTER_TEST_VALUES, &budget, out);
    irq_restore(flags);
    return created;
}
bool device_register_reader_revoke(DeviceRegisterReader *r) {
    u64 flags = irq_save();
    bool revoked = valid_locked(r);
    if (revoked) {
        r->revoked = true;
        polaris_capture_budget_close(&r->capture);
    }
    irq_restore(flags);
    return revoked;
}
bool device_register_reader_destroy(DeviceRegisterReader *r) {
    u64 flags = irq_save();
    bool destroyed = false;
    if (valid_locked(r) && !r->resource.capability_refs &&
        device_resource_destroy(&r->resource)) {
        u32 slot = object_storage_find(g_readers, DEVICE_REGISTER_READER_CAPACITY, r);
        g_readers[slot] = 0;
        k_memset(r, 0, sizeof(*r));
        destroyed = true;
    }
    irq_restore(flags);
    return destroyed;
}
bool device_register_reader_read(Process *owner, CapabilityHandle handle,
    u64 expected_identity, u64 request, u32 *out_value) {
    if (!out_value) return false;
    *out_value = 0U;
    if (!handle || !expected_identity || (request >> 32) != JCOS_REGISTER_READ_VERSION) return false;
    u32 pipe = (u32)((request >> 16) & 0xFFFFULL);
    u32 field = (u32)(request & 0xFFFFULL);
    u32 offset = 0U;
    if (!jcos_polaris_register_offset(pipe, (PolarisDisplayRegister)field, 0x40000ULL, &offset))
        return false;
    u64 flags = irq_save();
    bool read = false;
    void *object = 0;
    CapabilityTable *caps = owner_live(owner) ? process_capabilities(owner) : 0;
    if (!caps || !capability_lookup_rights(caps, handle, CAPABILITY_TYPE_DEVICE_RESOURCE,
            CAPABILITY_RIGHT_READ, &object)) goto done;
    DeviceRegisterReader *r = (DeviceRegisterReader *)object;
    if (!valid_locked(r) || r->revoked || r->owner_process_id != owner->id ||
        r->resource.id != expected_identity) goto done;
    bool finite = r->profile == JCOS_REGISTER_PROFILE_POLARIS_DCE112_CAPTURE_TEST ||
        r->profile == JCOS_REGISTER_PROFILE_POLARIS_DCE112_OBSERVATION;
    if (finite && (!timer_initialized() || !polaris_capture_budget_admit(&r->capture,
            pipe, field, timer_frequency(), timer_ticks()))) {
        r->revoked = true;
        polaris_capture_budget_close(&r->capture);
        goto done;
    }
    /* All constructible profiles read protected copied RAM only.
     * Admission bounds future operations; it does not qualify MMIO access. */
    u32 value = r->values[pipe * POLARIS_DISPLAY_REGISTER_COUNT + field];
    if (finite && value == ~0U) {
        r->revoked = true;
        polaris_capture_budget_close(&r->capture);
        goto done;
    }
    *out_value = value;
    if (finite && r->capture.closed) r->revoked = true;
    if (r->successful_reads != ~0ULL) ++r->successful_reads;
    read = true;
done:
    irq_restore(flags);
    return read;
}
