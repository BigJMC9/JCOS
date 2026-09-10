#include "process.h"
#include "lib.h"

static Process g_kernel_process;

static u64 g_next_process_id;
static bool g_initialized;

bool process_system_init(AddressSpace *kernel_space) {
    if (g_initialized || !kernel_space || !kernel_space->kernel || !address_space_cr3(kernel_space)) return false;

    k_memset(&g_kernel_process, 0, sizeof(g_kernel_process));

    if (!capability_table_init(&g_kernel_process.capabilities)) return false;

    g_kernel_process.id = 1ULL;
    g_kernel_process.address_space = kernel_space;
    g_kernel_process.kernel = true;
    g_kernel_process.owns_address_space = false;
    g_kernel_process.initialized = true;
    g_next_process_id = 2ULL;
    g_initialized = true;
    g_kernel_process.thread_count = 0;

    return true;
}

Process *process_kernel(void) {
    if (!g_initialized || !g_kernel_process.initialized) return 0;
    return &g_kernel_process;
}

bool process_create(Process *process) {
    if (!g_initialized || !process || process == &g_kernel_process || !g_next_process_id) return false;
    k_memset(process, 0, sizeof(*process));

    if (!address_space_create(&process->owned_address_space)) return false;
    if (!capability_table_init(&process->capabilities)) {
        address_space_destroy(&process->owned_address_space);
        k_memset(process, 0, sizeof(*process));
        return false;
    }

    process->id = g_next_process_id++;
    process->address_space = &process->owned_address_space;
    process->kernel = false;
    process->owns_address_space = true;
    process->initialized = true;
    process->thread_count = 0;

    return true;
}

bool process_destroy(Process *process) {
    if (!g_initialized ||
        !process ||
        process == &g_kernel_process ||
        !process->initialized ||
        !process->id ||
        process->kernel ||
        !process->owns_address_space ||
        process->address_space != &process->owned_address_space) {
        return false;
    }

    /* The address space must remain alive until every thread belonging to this process has been destroyed/reaped. */
    if (process->thread_count != 0ULL) return false;
    /* Capabilities represent authority owned by the process. Require explicit revocation before destroying the process. */
    if (capability_table_count(&process->capabilities) != 0U) return false;
    address_space_destroy(&process->owned_address_space);
    k_memset(process, 0, sizeof(*process));
    return true;
}

AddressSpace *process_address_space(Process *process) {
    if (!g_initialized || !process || !process->initialized) return 0;
    return process->address_space;
}

CapabilityTable *process_capabilities(Process *process) {
    if (!g_initialized || !process || !process->initialized) return 0;
    return &process->capabilities;
}

u64 process_thread_count(const Process *process) {
    if (!g_initialized || !process || !process->initialized) return 0;
    return process->thread_count;
}

bool process_thread_attach(Process *process) {
    if (!g_initialized ||
        !process ||
        !process->initialized ||
        !process->id ||
        !process->address_space ||
        !address_space_cr3(process->address_space) ||
        process->thread_count == ~0ULL) {

        return false;
    }

    ++process->thread_count;

    return true;
}

bool process_thread_detach(Process *process) {
    if (!g_initialized || !process || !process->initialized || !process->id || !process->thread_count) {
        return false;
    }
    --process->thread_count;

    return true;
}