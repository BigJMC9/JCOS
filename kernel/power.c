#include "power.h"

#include "acpi.h"
#include "address_space.h"
#include "arch.h"
#include "block.h"
#include "capability.h"
#include "endpoint.h"
#include "interrupts.h"
#include "process.h"
#include "scheduler.h"
#include "serial.h"
#include "supervisor.h"
#include "thread.h"

#define POWER_RESET_WAIT_SPINS 10000000ULL
#define POWER_8042_WAIT_SPINS  200000U

static bool g_power_transition;

bool power_storage_safe(void) {
    u32 count = block_device_count();

    for (u32 i = 0; i < count; ++i) {
        BlockDevice *device = block_device(i);
        if (device && !device->read_only) return false;
    }
    return true;
}

static bool power_object_state_clean(bool supervisor_expected) {
    Process *kernel = process_kernel();
    CapabilityTable *kernel_caps = kernel ? process_capabilities(kernel) : 0;
    if (!kernel || !kernel_caps) return false;

    u32 expected_processes = supervisor_expected ? 2U : 1U;
    u32 expected_spaces = supervisor_expected ? 2U : 1U;
    u32 expected_threads = supervisor_expected ? 2U : 1U;
    u32 expected_endpoints = supervisor_expected ? 2U : 0U;
    u32 expected_tables = supervisor_expected ? 2U : 1U;
    /* R5 supervisor: SEND + RECEIVE plus two TRANSFER grant authorities. */
    u32 expected_kernel_caps = supervisor_expected ? 4U : 0U;

    return process_object_count() == expected_processes &&
        address_space_object_count() == expected_spaces &&
        thread_object_count() == expected_threads &&
        endpoint_object_count() == expected_endpoints &&
        capability_table_object_count() == expected_tables &&
        capability_table_count(kernel_caps) == expected_kernel_caps &&
        process_thread_count(kernel) == 1ULL;
}

static PowerResult power_check_common(bool require_poweroff) {
    if (g_power_transition) return POWER_RESULT_BUSY;
    if (require_poweroff && !acpi_poweroff_supported()) return POWER_RESULT_UNSUPPORTED;

    Process *kernel = process_kernel();
    Thread *current = thread_current();
    if (!kernel || !current || current->process != kernel || current->state != THREAD_STATE_RUNNING ||
        !current->on_run_queue) return POWER_RESULT_BAD_CONTEXT;
    if (scheduler_thread_count() != 1ULL) return POWER_RESULT_RUNNABLE_STATE;
    if (!power_object_state_clean(supervisor_running())) return POWER_RESULT_OBJECT_STATE;
    if (!power_storage_safe()) return POWER_RESULT_STORAGE_UNSAFE;
    return POWER_RESULT_OK;
}

PowerResult power_check_shutdown(void) {
    return power_check_common(true);
}

PowerResult power_check_reboot(void) {
    return power_check_common(false);
}

static PowerResult power_quiesce(bool require_poweroff) {
    PowerResult check = power_check_common(require_poweroff);
    if (check != POWER_RESULT_OK) return check;

    g_power_transition = true;
    if (supervisor_running() && !supervisor_stop()) {
        interrupts_disable();
        serial_write("POWER: supervisor shutdown failed after transition began; halting.\n");
        cpu_halt_forever();
    }

    Thread *current = thread_current();
    if (!current || current->process != process_kernel() || current->state != THREAD_STATE_RUNNING ||
        !current->on_run_queue || scheduler_thread_count() != 1ULL || supervisor_running() ||
        !power_object_state_clean(false)) {
        serial_write("POWER: post-quiesce object invariant failed; halting.\n");
        cpu_halt_forever();
    }
    return POWER_RESULT_OK;
}

static void power_wait(void) {
    for (volatile u64 i = 0; i < POWER_RESET_WAIT_SPINS; ++i) arch_pause();
}

PowerResult power_shutdown(void) {
    PowerResult result = power_quiesce(true);
    if (result != POWER_RESULT_OK) return result;

    interrupts_disable();
    serial_write("POWER: supervisor stopped; requesting ACPI S5 shutdown.\n");

    if (!acpi_try_poweroff()) {
        serial_write("POWER: ACPI S5 request failed after quiesce; halting.\n");
        cpu_halt_forever();
    }

    power_wait();
    serial_write("POWER: ACPI S5 request returned; halting instead of resuming a quiesced system.\n");
    cpu_halt_forever();
}

PowerResult power_reboot(void) {
    PowerResult result = power_quiesce(false);
    if (result != POWER_RESULT_OK) return result;

    interrupts_disable();
    serial_write("POWER: supervisor stopped; requesting reboot.\n");

    if (acpi_try_reset()) power_wait();

    const AcpiInfo *info = acpi_get();
    if (!info->i8042_known || info->i8042_present) {
        for (u32 i = 0; i < POWER_8042_WAIT_SPINS; ++i) {
            if (!(arch_in8(0x64U) & 0x02U)) break;
            arch_pause();
        }
        arch_out8(0x64U, 0xFEU);
        power_wait();
    }

    serial_write("POWER: reset methods returned; forcing triple fault.\n");
    arch_triple_fault();
}

const char *power_result_name(PowerResult result) {
    switch (result) {
        case POWER_RESULT_OK: return "OK";
        case POWER_RESULT_BUSY: return "POWER TRANSITION ALREADY IN PROGRESS";
        case POWER_RESULT_UNSUPPORTED: return "ACPI S5 SHUTDOWN IS NOT AVAILABLE";
        case POWER_RESULT_BAD_CONTEXT: return "COMMAND MUST RUN ON THE KERNEL SHELL THREAD";
        case POWER_RESULT_RUNNABLE_STATE: return "ANOTHER THREAD IS RUNNABLE";
        case POWER_RESULT_OBJECT_STATE: return "KERNEL / USERSPACE OBJECT STATE IS NOT QUIESCENT";
        case POWER_RESULT_STORAGE_UNSAFE: return "WRITABLE STORAGE HAS NO FLUSH CONTRACT YET";
        default: return "UNKNOWN POWER ERROR";
    }
}
