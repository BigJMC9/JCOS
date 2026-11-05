#include "pci_device_recovery_test.h"

#include "../address_space.h"
#include "../capability.h"
#include "../device_resource.h"
#include "../endpoint.h"
#include "../ipc.h"
#include "../lib.h"
#include "../pci.h"
#include "../pci_assignment.h"
#include "../pci_device_recovery.h"
#include "../pci_dma_buffer.h"
#include "../pci_dma_submission.h"
#include "../pci_flr.h"
#include "../pci_irq_resource.h"
#include "../physmap.h"
#include "../pmm.h"
#include "../process_exit_queue.h"
#include "../program.h"
#include "../scheduler.h"
#include "../process.h"
#include "../task.h"
#include "../terminal.h"
#include "../thread.h"
#include "../timer.h"
#include "../vfs.h"
#include "../../include/pci_driver_probe_protocol.h"

#define PCI_RECOVERY_TEST_DMA_PAGES 1U
#define PCI_RECOVERY_COMMAND_OFFSET 0x04U
#define PCI_DRIVER_PROBE_PATH "/bin/pcidriverprobe.elf"

static PciAssignment g_assignment;
static PciIrqResource g_irq;
static PciDmaBuffer g_dma_buffer;
static PciDmaSubmission g_dma_submission;
static PciDeviceRecovery g_recovery;
static Process g_driver;
static CapabilityHandle g_assignment_cap;
static CapabilityHandle g_irq_cap;
static CapabilityHandle g_irq_event_cap;
static CapabilityHandle g_dma_buffer_cap;

static ProgramInstance g_probe_program;
static ProcessExitQueue g_probe_exit_queue;
static Endpoint g_probe_command;
static Endpoint g_probe_reply;
static PciAssignment g_probe_assignment;
static PciIrqResource g_probe_irq;
static PciDmaBuffer g_probe_dma_buffer;
static PciDmaSubmission g_probe_dma_submission;
static CapabilityHandle g_probe_kernel_send;
static CapabilityHandle g_probe_kernel_receive;
static CapabilityHandle g_probe_command_grant;
static CapabilityHandle g_probe_reply_grant;
static CapabilityHandle g_probe_assignment_cap;
static CapabilityHandle g_probe_irq_cap;
static CapabilityHandle g_probe_irq_event_cap;
static CapabilityHandle g_probe_dma_buffer_cap;
static bool g_probe_exit_queue_created;
static bool g_probe_command_created;
static bool g_probe_reply_created;
static bool g_probe_kernel_send_cap;
static bool g_probe_kernel_receive_cap;
static bool g_probe_command_grant_cap;
static bool g_probe_reply_grant_cap;

typedef struct {
    u64 free_pages;
    u32 processes;
    u32 spaces;
    u32 tables;
    u32 threads;
    u32 endpoints;
    u32 exit_queues;
    u32 kernel_caps;
    u64 kernel_threads;
    u64 scheduler_threads;
    u32 device_resources;
    u32 assignments;
    u32 irq_resources;
    u32 dma_buffers;
    u32 dma_submissions;
    u32 recoveries;
} PciDeviceRecoveryBaseline;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static bool capture_baseline(PciDeviceRecoveryBaseline *out) {
    if (!out) return false;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!kernel || !caps) return false;
    out->free_pages = pmm_stats().free_pages;
    out->processes = process_object_count();
    out->spaces = address_space_object_count();
    out->tables = capability_table_object_count();
    out->threads = thread_object_count();
    out->endpoints = endpoint_object_count();
    out->exit_queues = process_exit_queue_object_count();
    out->kernel_caps = capability_table_count(caps);
    out->kernel_threads = process_thread_count(kernel);
    out->scheduler_threads = scheduler_thread_count();
    out->device_resources = device_resource_object_count();
    out->assignments = pci_assignment_object_count();
    out->irq_resources = pci_irq_resource_object_count();
    out->dma_buffers = pci_dma_buffer_object_count();
    out->dma_submissions = pci_dma_submission_object_count();
    out->recoveries = pci_device_recovery_object_count();
    return true;
}

static bool baseline_matches(const PciDeviceRecoveryBaseline *baseline) {
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    return baseline && kernel && caps &&
        pmm_stats().free_pages == baseline->free_pages &&
        process_object_count() == baseline->processes &&
        address_space_object_count() == baseline->spaces &&
        capability_table_object_count() == baseline->tables &&
        thread_object_count() == baseline->threads &&
        endpoint_object_count() == baseline->endpoints &&
        process_exit_queue_object_count() == baseline->exit_queues &&
        capability_table_count(caps) == baseline->kernel_caps &&
        process_thread_count(kernel) == baseline->kernel_threads &&
        scheduler_thread_count() == baseline->scheduler_threads &&
        device_resource_object_count() == baseline->device_resources &&
        pci_assignment_object_count() == baseline->assignments &&
        pci_irq_resource_object_count() == baseline->irq_resources &&
        pci_dma_buffer_object_count() == baseline->dma_buffers &&
        pci_dma_submission_object_count() == baseline->dma_submissions &&
        pci_device_recovery_object_count() == baseline->recoveries;
}

static bool flr_discovery_contract(void) {
    bool inspected = pci_device_count() != 0U;
    for (u32 i = 0U; i < pci_device_count(); ++i) {
        const PciDevice *device = pci_device(i);
        if (!device || !pci_device_is_enumerated(device)) return false;

        u16 vendor_before = pci_config_read16(device->address, 0x00U);
        u16 device_before = pci_config_read16(device->address, 0x02U);
        u16 command_before = pci_config_read16(device->address,
            PCI_RECOVERY_COMMAND_OFFSET);
        if (vendor_before == 0xFFFFU || device_before == 0xFFFFU ||
            command_before == 0xFFFFU) return false;

        u8 express = 0U;
        u8 flr = 0U;
        bool has_express = pci_find_capability(device,
            PCI_CAPABILITY_ID_EXPRESS, &express);
        bool has_flr = pci_flr_supported(device, &flr);
        if (has_flr && (!has_express || !express || express != flr)) return false;

        u16 unsafe = PCI_COMMAND_IO_SPACE | PCI_COMMAND_MEMORY_SPACE |
            PCI_COMMAND_BUS_MASTER;
        if (has_flr && ((command_before & unsafe) ||
                !(command_before & PCI_COMMAND_INT_DISABLE))) {
            u16 ignored = 0U;
            if (pci_flr_execute_quiesced(device, &ignored) !=
                    PCI_FLR_RESULT_NOT_ATTEMPTED || ignored) return false;
        }

        if (pci_config_read16(device->address, 0x00U) != vendor_before ||
            pci_config_read16(device->address, 0x02U) != device_before ||
            pci_config_read16(device->address, PCI_RECOVERY_COMMAND_OFFSET) !=
                command_before) return false;
    }
    return inspected;
}

static u64 probe_timeout_ticks(void) {
    if (!timer_initialized()) return 0ULL;
    u32 frequency = timer_frequency();
    return frequency ? (u64)frequency * 2ULL : 0ULL;
}

static bool probe_thread_terminal(void) {
    Thread *thread = &g_probe_program.thread;
    return g_probe_program.thread_created && thread->state == THREAD_STATE_DEAD &&
        !thread->on_run_queue && !thread->interrupt_context_ready &&
        !thread->interrupt_rsp && !thread_wait_active(thread);
}

static bool probe_send(u64 a, u64 b, u64 c, u64 d, u32 words) {
    if (!g_probe_kernel_send_cap || words > IPC_MESSAGE_MAX_WORDS) return false;
    IpcMessage message;
    k_memset(&message, 0, sizeof(message));
    message.word_count = words;
    message.words[0] = a;
    message.words[1] = b;
    message.words[2] = c;
    message.words[3] = d;
    u64 timeout = probe_timeout_ticks();
    Process *kernel = process_kernel();
    return timeout && kernel && ipc_send_blocking_for(kernel,
        g_probe_kernel_send, &message, timeout);
}

static bool probe_receive(IpcMessage *out) {
    if (!out || !g_probe_kernel_receive_cap) return false;
    k_memset(out, 0, sizeof(*out));
    u64 timeout = probe_timeout_ticks();
    Process *kernel = process_kernel();
    return timeout && kernel && ipc_receive_blocking_for(kernel,
        g_probe_kernel_receive, out, timeout);
}

static bool probe_portal_create(void) {
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!kernel || !caps) return false;

    if (!endpoint_create(&g_probe_command)) return false;
    g_probe_command_created = true;
    if (!endpoint_create(&g_probe_reply)) return false;
    g_probe_reply_created = true;
    if (!capability_insert(caps, &g_probe_command, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND, &g_probe_kernel_send)) return false;
    g_probe_kernel_send_cap = true;
    if (!capability_insert(caps, &g_probe_reply, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE, &g_probe_kernel_receive)) return false;
    g_probe_kernel_receive_cap = true;
    if (!capability_insert(caps, &g_probe_command, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE | CAPABILITY_RIGHT_TRANSFER,
            &g_probe_command_grant)) return false;
    g_probe_command_grant_cap = true;
    if (!capability_insert(caps, &g_probe_reply, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND | CAPABILITY_RIGHT_TRANSFER,
            &g_probe_reply_grant)) return false;
    g_probe_reply_grant_cap = true;
    if (!process_exit_queue_create(&g_probe_exit_queue)) return false;
    g_probe_exit_queue_created = true;
    return true;
}

static bool probe_launch(void) {
    if (!probe_portal_create()) return false;
    VfsNode *file = vfs_resolve(vfs_root(), PCI_DRIVER_PROBE_PATH);
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!file || file->type != VFS_FILE || !file->data || !file->size || !caps)
        return false;

    ProgramLaunchSpec spec;
    k_memset(&spec, 0, sizeof(spec));
    spec.file = file;
    spec.exit_queue = &g_probe_exit_queue;
    spec.startup_grant_count = 2U;
    spec.startup_grants[0].authority_table = caps;
    spec.startup_grants[0].authority_handle = g_probe_command_grant;
    spec.startup_grants[0].object = &g_probe_command;
    spec.startup_grants[0].type = CAPABILITY_TYPE_ENDPOINT;
    spec.startup_grants[0].rights = CAPABILITY_RIGHT_RECEIVE;
    spec.startup_grants[1].authority_table = caps;
    spec.startup_grants[1].authority_handle = g_probe_reply_grant;
    spec.startup_grants[1].object = &g_probe_reply;
    spec.startup_grants[1].type = CAPABILITY_TYPE_ENDPOINT;
    spec.startup_grants[1].rights = CAPABILITY_RIGHT_SEND;
    spec.startup_argument_count = 1U;
    spec.startup_arguments[0] = JCOS_PCI_DRIVER_PROBE_VERSION;
    if (!program_launch(&g_probe_program, &spec)) return false;

    IpcMessage ready;
    return probe_receive(&ready) && ready.word_count == 2U &&
        ready.words[0] == JCOS_PCI_DRIVER_PROBE_READY &&
        ready.words[1] == JCOS_PCI_DRIVER_PROBE_VERSION;
}

static bool probe_wait_fault(ProcessExitInfo *out) {
    if (!out || !g_probe_exit_queue_created || !g_probe_program.thread_created)
        return false;
    u64 timeout = probe_timeout_ticks();
    if (!timeout) return false;
    u64 started = timer_ticks();

    for (;;) {
        u32 pending = process_exit_queue_pending_count(&g_probe_exit_queue);
        if (pending > 1U) return false;
        if (pending == 1U && probe_thread_terminal()) break;
        if ((u64)(timer_ticks() - started) >= timeout) return false;
        if (!scheduler_yield()) return false;
    }

    k_memset(out, 0, sizeof(*out));
    return process_exit_queue_try_receive(&g_probe_exit_queue, out) &&
        process_exit_queue_pending_count(&g_probe_exit_queue) == 0U &&
        process_exit_queue_watch_count(&g_probe_exit_queue) == 0U;
}

static bool probe_virtual_dma_write(PciDmaSubmission *submission,
    u64 offset, u64 size, u8 value) {
    u64 physical = 0ULL;
    u64 span = 0ULL;
    if (!submission || !size ||
        !pci_dma_submission_device_span(submission, &physical, &span) ||
        offset >= span || size > span - offset || physical > ~0ULL - offset)
        return false;
    u8 *bytes = (u8 *)phys_to_virt(physical + offset);
    if (!bytes) return false;
    for (u64 i = 0ULL; i < size; ++i) bytes[i] = value;
    return true;
}

static bool probe_close_exit_queue(void) {
    if (!g_probe_exit_queue_created) return true;
    while (process_exit_queue_pending_count(&g_probe_exit_queue)) {
        ProcessExitInfo discard;
        k_memset(&discard, 0, sizeof(discard));
        if (!process_exit_queue_try_receive(&g_probe_exit_queue, &discard)) return false;
    }
    if (process_exit_queue_watch_count(&g_probe_exit_queue)) return false;
    if (!g_probe_exit_queue.closed && !process_exit_queue_close(&g_probe_exit_queue))
        return false;
    if (!process_exit_queue_destroy(&g_probe_exit_queue)) return false;
    g_probe_exit_queue_created = false;
    k_memset(&g_probe_exit_queue, 0, sizeof(g_probe_exit_queue));
    return true;
}

static bool probe_revoke_kernel_cap(CapabilityHandle *handle, bool *active) {
    if (!handle || !active || !*active) return true;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps || !capability_revoke(caps, *handle)) return false;
    *handle = CAPABILITY_INVALID_HANDLE;
    *active = false;
    return true;
}

static bool probe_cleanup_all(void) {
    bool ok = true;
    u64 owner = pci_assignment_valid(&g_probe_assignment) ?
        pci_assignment_owner_process_id(&g_probe_assignment) : 0ULL;

    if (process_storage_in_use(&g_probe_program.process) &&
        (process_thread_count(&g_probe_program.process) ||
         g_probe_program.process.thread_head || g_probe_program.process.thread_tail)) {
        if (!task_quiesce_process(&g_probe_program.process)) ok = false;
    }

    if (pci_dma_buffer_valid(&g_probe_dma_buffer) && owner &&
        !pci_dma_buffer_force_unmap(&g_probe_dma_buffer, owner)) ok = false;

    if (pci_assignment_valid(&g_probe_assignment) && owner &&
        pci_assignment_state(&g_probe_assignment) == PCI_ASSIGNMENT_ASSIGNED &&
        (pci_dma_submission_valid(&g_probe_dma_submission) ||
         pci_irq_resource_valid(&g_probe_irq))) {
        if (!pci_assignment_force_quarantine(&g_probe_assignment, owner)) ok = false;
    }

    if (pci_irq_resource_valid(&g_probe_irq) && owner &&
        !pci_irq_resource_force_disarm(&g_probe_irq, owner)) ok = false;

    if (pci_dma_submission_valid(&g_probe_dma_submission)) {
        if (pci_dma_submission_state(&g_probe_dma_submission) == PCI_DMA_SUBMISSION_ACTIVE &&
            !pci_dma_submission_force_quarantine(&g_probe_dma_submission)) ok = false;
        if (pci_dma_submission_state(&g_probe_dma_submission) == PCI_DMA_SUBMISSION_QUARANTINED &&
            !pci_dma_submission_reset_complete(&g_probe_dma_submission)) ok = false;
    }

    if (pci_dma_buffer_valid(&g_probe_dma_buffer)) {
        if (pci_dma_buffer_state(&g_probe_dma_buffer) == PCI_DMA_BUFFER_DEVICE_ACTIVE && owner &&
            !pci_dma_buffer_force_quarantine(&g_probe_dma_buffer, owner)) ok = false;
        if (pci_dma_buffer_state(&g_probe_dma_buffer) == PCI_DMA_BUFFER_QUARANTINED && owner &&
            !pci_dma_buffer_reset_complete(&g_probe_dma_buffer, owner)) ok = false;
    }

    if (process_storage_in_use(&g_probe_program.process)) {
        CapabilityTable *caps = process_capabilities(&g_probe_program.process);
        if (!caps || !capability_revoke_all(caps)) ok = false;
    }
    g_probe_assignment_cap = CAPABILITY_INVALID_HANDLE;
    g_probe_irq_cap = CAPABILITY_INVALID_HANDLE;
    g_probe_irq_event_cap = CAPABILITY_INVALID_HANDLE;
    g_probe_dma_buffer_cap = CAPABILITY_INVALID_HANDLE;

    if (pci_irq_resource_valid(&g_probe_irq) && !pci_irq_resource_destroy(&g_probe_irq))
        ok = false;
    if (pci_dma_buffer_valid(&g_probe_dma_buffer) &&
        !pci_dma_buffer_destroy(&g_probe_dma_buffer)) ok = false;

    if (pci_assignment_valid(&g_probe_assignment)) {
        owner = pci_assignment_owner_process_id(&g_probe_assignment);
        PciAssignmentState state = pci_assignment_state(&g_probe_assignment);
        if (state == PCI_ASSIGNMENT_ASSIGNED) {
            if (!pci_assignment_begin_quiesce(&g_probe_assignment, owner) ||
                !pci_assignment_complete_release(&g_probe_assignment, owner)) ok = false;
        } else if (state == PCI_ASSIGNMENT_QUARANTINED) {
            if (!pci_assignment_complete_quarantine(&g_probe_assignment, owner)) ok = false;
        }
        if (pci_assignment_state(&g_probe_assignment) == PCI_ASSIGNMENT_AVAILABLE &&
            !pci_assignment_destroy(&g_probe_assignment)) ok = false;
    }

    if (program_instance_needs_cleanup(&g_probe_program) &&
        !program_terminate(&g_probe_program)) ok = false;
    if (!probe_close_exit_queue()) ok = false;

    if (!probe_revoke_kernel_cap(&g_probe_kernel_send, &g_probe_kernel_send_cap)) ok = false;
    if (!probe_revoke_kernel_cap(&g_probe_kernel_receive, &g_probe_kernel_receive_cap)) ok = false;
    if (!probe_revoke_kernel_cap(&g_probe_command_grant, &g_probe_command_grant_cap)) ok = false;
    if (!probe_revoke_kernel_cap(&g_probe_reply_grant, &g_probe_reply_grant_cap)) ok = false;

    if (g_probe_command_created) {
        if (endpoint_message_ready(&g_probe_command)) {
            IpcMessage discard;
            k_memset(&discard, 0, sizeof(discard));
            if (!endpoint_try_receive(&g_probe_command, &discard)) ok = false;
        }
        if (endpoint_receiver_waiting(&g_probe_command) || endpoint_sender_waiting(&g_probe_command) ||
            !endpoint_destroy(&g_probe_command)) ok = false;
        else g_probe_command_created = false;
    }
    if (g_probe_reply_created) {
        if (endpoint_message_ready(&g_probe_reply)) {
            IpcMessage discard;
            k_memset(&discard, 0, sizeof(discard));
            if (!endpoint_try_receive(&g_probe_reply, &discard)) ok = false;
        }
        if (endpoint_receiver_waiting(&g_probe_reply) || endpoint_sender_waiting(&g_probe_reply) ||
            !endpoint_destroy(&g_probe_reply)) ok = false;
        else g_probe_reply_created = false;
    }

    return ok;
}

static bool ring3_virtual_driver_contract(const PciDeviceRecoveryBaseline *baseline) {
    /* The PCI function supplies assignment identity only. This proof never
     * programs its BARs, interrupt route, or command register. */
    if (!baseline || !probe_cleanup_all()) return false;
    const PciDevice *device = pci_device_count() ? pci_device(0U) : 0;
    bool launched = device && pci_device_is_enumerated(device) && probe_launch();
    report("RING3 DRIVER LAUNCH / MEDIATOR PORTAL", launched);
    if (!launched) goto fail;

    bool setup = pci_assignment_create(&g_probe_assignment, device) &&
        pci_assignment_claim(&g_probe_assignment, &g_probe_program.process,
            &g_probe_assignment_cap) &&
        pci_dma_buffer_create(&g_probe_dma_buffer, &g_probe_assignment,
            &g_probe_program.process, g_probe_assignment_cap, 2U,
            &g_probe_dma_buffer_cap) &&
        pci_irq_resource_create(&g_probe_irq, &g_probe_assignment,
            &g_probe_program.process, g_probe_assignment_cap,
            &g_probe_irq_cap, &g_probe_irq_event_cap) &&
        pci_dma_buffer_map(&g_probe_dma_buffer, &g_probe_program.process,
            g_probe_dma_buffer_cap, JCOS_PCI_DRIVER_PROBE_DMA_VIRTUAL, true);
    report("RING3 DRIVER / ASSIGNMENT + DMA + IRQ AUTHORITY", setup);
    if (!setup) goto fail;

    u64 physical = 0ULL;
    u64 buffer_size = 0ULL;
    bool span = pci_dma_buffer_device_span(&g_probe_dma_buffer, &physical, &buffer_size) &&
        buffer_size >= 256ULL;
    u8 *direct = span ? (u8 *)phys_to_virt(physical) : 0;
    if (direct) {
        direct[0] = 0x11U;
        direct[buffer_size - 1ULL] = 0x22U;
    }
    bool configured = direct && probe_send(JCOS_PCI_DRIVER_PROBE_CONFIG,
        JCOS_PCI_DRIVER_PROBE_DMA_VIRTUAL, buffer_size, g_probe_irq_event_cap, 4U);
    IpcMessage reply;
    configured = configured && probe_receive(&reply) && reply.word_count == 2U &&
        reply.words[0] == JCOS_PCI_DRIVER_PROBE_CONFIGURED &&
        reply.words[1] == buffer_size;
    report("RING3 DRIVER / VIRTUAL DMA + IRQ CONFIG", configured);
    if (!configured) goto fail;

    bool invalid_dma = probe_send(JCOS_PCI_DRIVER_PROBE_REQUEST_INVALID_DMA,
            0ULL, 0ULL, 0ULL, 1U) &&
        probe_receive(&reply) && reply.word_count == 4U &&
        reply.words[0] == JCOS_PCI_DRIVER_PROBE_DMA_REQUEST &&
        reply.words[1] == buffer_size && reply.words[2] == 1ULL &&
        !(reply.words[1] < buffer_size && reply.words[2] <= buffer_size - reply.words[1]) &&
        direct[0] == 0x11U && direct[buffer_size - 1ULL] == 0x22U &&
        probe_send(JCOS_PCI_DRIVER_PROBE_DMA_RESULT,
            JCOS_PCI_DRIVER_PROBE_DMA_REJECTED, 0ULL, 0ULL, 2U) &&
        probe_receive(&reply) && reply.word_count == 1U &&
        reply.words[0] == JCOS_PCI_DRIVER_PROBE_INVALID_REJECTED;
    report("RING3 DRIVER INVALID DMA RANGE REJECTED", invalid_dma);
    if (!invalid_dma) goto fail;

    const u64 dma_offset = 64ULL;
    const u64 dma_size = 32ULL;
    const u8 dma_value = 0xA7U;
    bool dma_request = probe_send(JCOS_PCI_DRIVER_PROBE_REQUEST_DMA,
            dma_offset, dma_size, dma_value, 4U) &&
        probe_receive(&reply) && reply.word_count == 4U &&
        reply.words[0] == JCOS_PCI_DRIVER_PROBE_DMA_REQUEST &&
        reply.words[1] == dma_offset && reply.words[2] == dma_size &&
        reply.words[3] == dma_value;
    bool dma_executed = dma_request &&
        pci_dma_buffer_unmap(&g_probe_dma_buffer, &g_probe_program.process,
            g_probe_dma_buffer_cap) &&
        pci_dma_submission_begin(&g_probe_dma_submission, &g_probe_assignment,
            &g_probe_program.process, g_probe_assignment_cap, &g_probe_dma_buffer,
            g_probe_dma_buffer_cap, dma_offset, dma_size, PCI_DMA_BIDIRECTIONAL) &&
        probe_virtual_dma_write(&g_probe_dma_submission, 0ULL, dma_size, dma_value) &&
        pci_dma_submission_complete(&g_probe_dma_submission,
            &g_probe_program.process, g_probe_dma_buffer_cap) &&
        pci_dma_buffer_map(&g_probe_dma_buffer, &g_probe_program.process,
            g_probe_dma_buffer_cap, JCOS_PCI_DRIVER_PROBE_DMA_VIRTUAL, true) &&
        probe_send(JCOS_PCI_DRIVER_PROBE_DMA_RESULT,
            JCOS_PCI_DRIVER_PROBE_DMA_COMPLETE, 0ULL, 0ULL, 2U) &&
        probe_receive(&reply) && reply.word_count == 4U &&
        reply.words[0] == JCOS_PCI_DRIVER_PROBE_DMA_VERIFIED &&
        reply.words[1] == dma_offset && reply.words[2] == dma_size &&
        reply.words[3] == dma_value && direct[0] == 0x11U &&
        direct[buffer_size - 1ULL] == 0x22U;
    report("RING3 DRIVER BOUNDED MEDIATED DMA ROUND TRIP", dma_executed);
    if (!dma_executed) goto fail;

    bool irq_round_trip = pci_irq_resource_arm(&g_probe_irq, &g_probe_program.process,
            g_probe_irq_cap) &&
        probe_send(JCOS_PCI_DRIVER_PROBE_WAIT_IRQ, 0ULL, 0ULL, 0ULL, 1U) &&
        pci_irq_resource_signal(&g_probe_irq) && probe_receive(&reply) &&
        reply.word_count == 4U && reply.words[0] == JCOS_PCI_DRIVER_PROBE_IRQ_ACK &&
        reply.words[1] && reply.words[2] == 0ULL &&
        reply.words[3] == g_probe_irq.resource.id &&
        pci_irq_resource_acknowledge(&g_probe_irq, &g_probe_program.process,
            g_probe_irq_cap, reply.words[1]) &&
        pci_irq_resource_state(&g_probe_irq) == PCI_IRQ_STATE_ARMED;
    report("RING3 DRIVER IRQ DELIVERY / EXACT ACK", irq_round_trip);
    if (!irq_round_trip) goto fail;

    const u64 crash_offset = 128ULL;
    const u64 crash_size = 16ULL;
    bool crash_submission = probe_send(JCOS_PCI_DRIVER_PROBE_REQUEST_DMA_THEN_FAULT,
            crash_offset, crash_size, 0xCCULL, 4U) &&
        probe_receive(&reply) && reply.word_count == 4U &&
        reply.words[0] == JCOS_PCI_DRIVER_PROBE_DMA_REQUEST &&
        reply.words[1] == crash_offset && reply.words[2] == crash_size &&
        pci_dma_buffer_unmap(&g_probe_dma_buffer, &g_probe_program.process,
            g_probe_dma_buffer_cap) &&
        pci_dma_submission_begin(&g_probe_dma_submission, &g_probe_assignment,
            &g_probe_program.process, g_probe_assignment_cap, &g_probe_dma_buffer,
            g_probe_dma_buffer_cap, crash_offset, crash_size, PCI_DMA_BIDIRECTIONAL) &&
        probe_send(JCOS_PCI_DRIVER_PROBE_DIAG_FAULT, 0ULL, 0ULL, 0ULL, 1U);
    report("RING3 DRIVER FAULT WITH DMA SUBMISSION ACTIVE", crash_submission);
    if (!crash_submission) goto fail;

    ProcessExitInfo exit_info;
    bool fault = probe_wait_fault(&exit_info) &&
        exit_info.reason == PROCESS_EXIT_FAULT && exit_info.vector == 6ULL &&
        exit_info.process_id == g_probe_program.process.id &&
        exit_info.thread_id == g_probe_program.thread.id &&
        task_quiesce_process(&g_probe_program.process);
    report("RING3 DRIVER #UD / AUTHORITY REVOKED", fault);
    if (!fault) goto fail;

    u64 owner = g_probe_program.process.id;
    bool contained = pci_assignment_force_quarantine(&g_probe_assignment, owner) &&
        pci_irq_resource_force_disarm(&g_probe_irq, owner) &&
        pci_dma_submission_force_quarantine(&g_probe_dma_submission) &&
        pci_dma_submission_state(&g_probe_dma_submission) == PCI_DMA_SUBMISSION_QUARANTINED &&
        pci_dma_buffer_state(&g_probe_dma_buffer) == PCI_DMA_BUFFER_QUARANTINED &&
        pci_assignment_dma_gate_active(&g_probe_assignment) &&
        pci_assignment_active_access_count(&g_probe_assignment) == 1U &&
        !pci_dma_buffer_destroy(&g_probe_dma_buffer);
    report("DRIVER DEATH RETAINS DMA QUARANTINE", contained);
    if (!contained) goto fail;

    bool reset = pci_dma_submission_reset_complete(&g_probe_dma_submission) &&
        !pci_assignment_dma_gate_active(&g_probe_assignment) &&
        pci_assignment_active_access_count(&g_probe_assignment) == 0U &&
        pci_dma_buffer_state(&g_probe_dma_buffer) == PCI_DMA_BUFFER_READY;
    report("VIRTUAL DEVICE RESET RELEASES DMA QUARANTINE", reset);
    if (!reset) goto fail;

    bool device_cleanup = pci_irq_resource_destroy(&g_probe_irq) &&
        pci_dma_buffer_destroy(&g_probe_dma_buffer) &&
        pci_assignment_child_count(&g_probe_assignment) == 0U &&
        pci_assignment_complete_quarantine(&g_probe_assignment, owner) &&
        pci_assignment_destroy(&g_probe_assignment) &&
        program_terminate(&g_probe_program) && probe_close_exit_queue();
    if (device_cleanup) {
        device_cleanup = probe_revoke_kernel_cap(&g_probe_kernel_send,
                &g_probe_kernel_send_cap) &&
            probe_revoke_kernel_cap(&g_probe_kernel_receive,
                &g_probe_kernel_receive_cap) &&
            probe_revoke_kernel_cap(&g_probe_command_grant,
                &g_probe_command_grant_cap) &&
            probe_revoke_kernel_cap(&g_probe_reply_grant,
                &g_probe_reply_grant_cap);
    }
    if (device_cleanup && endpoint_destroy(&g_probe_command)) {
        g_probe_command_created = false;
    } else if (device_cleanup) {
        device_cleanup = false;
    }
    if (device_cleanup && endpoint_destroy(&g_probe_reply)) {
        g_probe_reply_created = false;
    } else if (device_cleanup) {
        device_cleanup = false;
    }
    report("RING3 DRIVER / MEDIATED DEVICE RESOURCE RECLAIM", device_cleanup);
    if (!device_cleanup) goto fail;

    bool baseline_restored = baseline_matches(baseline);
    report("RING3 DRIVER FULL RESOURCE BASELINE", baseline_restored);
    return baseline_restored;

fail:
    (void)probe_cleanup_all();
    return false;
}

static bool ring3_hostile_driver_contract(const PciDeviceRecoveryBaseline *baseline) {
    /* This is a virtual-device proof. The PCI function is assignment identity
     * only; the test never enables decode, routes a physical IRQ, or sets BME. */
    if (!baseline || !probe_cleanup_all()) return false;
    const PciDevice *device = pci_device_count() ? pci_device(0U) : 0;
    bool launched = device && pci_device_is_enumerated(device) && probe_launch();
    report("HOSTILE DRIVER LAUNCH / LIVE PREEMPTION", launched &&
        scheduler_preemption_enabled());
    if (!launched || !scheduler_preemption_enabled()) goto fail;

    bool setup = pci_assignment_create(&g_probe_assignment, device) &&
        pci_assignment_claim(&g_probe_assignment, &g_probe_program.process,
            &g_probe_assignment_cap) &&
        pci_dma_buffer_create(&g_probe_dma_buffer, &g_probe_assignment,
            &g_probe_program.process, g_probe_assignment_cap, 2U,
            &g_probe_dma_buffer_cap) &&
        pci_irq_resource_create(&g_probe_irq, &g_probe_assignment,
            &g_probe_program.process, g_probe_assignment_cap,
            &g_probe_irq_cap, &g_probe_irq_event_cap) &&
        pci_dma_buffer_map(&g_probe_dma_buffer, &g_probe_program.process,
            g_probe_dma_buffer_cap, JCOS_PCI_DRIVER_PROBE_DMA_VIRTUAL, true);
    report("HOSTILE DRIVER / ASSIGNMENT + DMA + IRQ AUTHORITY", setup);
    if (!setup) goto fail;

    u64 physical = 0ULL;
    u64 buffer_size = 0ULL;
    bool span = pci_dma_buffer_device_span(&g_probe_dma_buffer, &physical, &buffer_size) &&
        buffer_size >= 256ULL;
    bool configured = span && probe_send(JCOS_PCI_DRIVER_PROBE_CONFIG,
        JCOS_PCI_DRIVER_PROBE_DMA_VIRTUAL, buffer_size, g_probe_irq_event_cap, 4U);
    IpcMessage reply;
    configured = configured && probe_receive(&reply) && reply.word_count == 2U &&
        reply.words[0] == JCOS_PCI_DRIVER_PROBE_CONFIGURED &&
        reply.words[1] == buffer_size;
    report("HOSTILE DRIVER / VIRTUAL DEVICE CONFIG", configured);
    if (!configured) goto fail;

    const u64 hang_offset = 192ULL;
    const u64 hang_size = 32ULL;
    const u8 hang_value = 0xD1U;
    bool dma_requested = probe_send(JCOS_PCI_DRIVER_PROBE_REQUEST_DMA_THEN_HANG,
            hang_offset, hang_size, hang_value, 4U) &&
        probe_receive(&reply) && reply.word_count == 4U &&
        reply.words[0] == JCOS_PCI_DRIVER_PROBE_DMA_REQUEST &&
        reply.words[1] == hang_offset && reply.words[2] == hang_size &&
        reply.words[3] == hang_value;
    bool dma_active = dma_requested &&
        pci_dma_buffer_unmap(&g_probe_dma_buffer, &g_probe_program.process,
            g_probe_dma_buffer_cap) &&
        pci_dma_submission_begin(&g_probe_dma_submission, &g_probe_assignment,
            &g_probe_program.process, g_probe_assignment_cap, &g_probe_dma_buffer,
            g_probe_dma_buffer_cap, hang_offset, hang_size, PCI_DMA_BIDIRECTIONAL) &&
        probe_virtual_dma_write(&g_probe_dma_submission, 0ULL, hang_size, hang_value) &&
        pci_dma_submission_state(&g_probe_dma_submission) == PCI_DMA_SUBMISSION_ACTIVE &&
        pci_assignment_dma_gate_active(&g_probe_assignment);
    report("HOSTILE DRIVER / DMA SUBMISSION LEFT ACTIVE", dma_active);
    if (!dma_active) goto fail;

    bool irq_wait = pci_irq_resource_arm(&g_probe_irq, &g_probe_program.process,
            g_probe_irq_cap) &&
        probe_send(JCOS_PCI_DRIVER_PROBE_WAIT_IRQ_AND_HANG, 0ULL, 0ULL, 0ULL, 1U) &&
        pci_irq_resource_signal(&g_probe_irq) && probe_receive(&reply) &&
        reply.word_count == 4U && reply.words[0] == JCOS_PCI_DRIVER_PROBE_HANGING &&
        reply.words[1] && reply.words[2] == 0ULL &&
        reply.words[3] == g_probe_irq.resource.id &&
        pci_irq_resource_pending_sequence(&g_probe_irq) == reply.words[1] &&
        pci_irq_resource_state(&g_probe_irq) == PCI_IRQ_STATE_PENDING;
    report("HOSTILE DRIVER CONSUMES IRQ / WITHHOLDS ACK / SPINS", irq_wait);
    if (!irq_wait) goto fail;

    bool preempted = process_thread_count(&g_probe_program.process) == 1ULL &&
        !probe_thread_terminal() && scheduler_preemption_enabled();
    report("LIVE PREEMPTION KEEPS SUPERVISOR RUNNABLE", preempted);
    if (!preempted) goto fail;

    bool suppressed = true;
    for (u32 i = 0U; i < PCI_IRQ_STORM_THRESHOLD; ++i) {
        if (pci_irq_resource_signal(&g_probe_irq)) suppressed = false;
    }
    u64 pending_sequence = reply.words[1];
    bool stormed = suppressed &&
        pci_irq_resource_state(&g_probe_irq) == PCI_IRQ_STATE_STORMED &&
        pci_irq_resource_delivery_masked(&g_probe_irq) &&
        pci_irq_resource_pending_sequence(&g_probe_irq) == pending_sequence &&
        pci_irq_resource_delivered_count(&g_probe_irq) == 1ULL &&
        pci_irq_resource_acknowledged_count(&g_probe_irq) == 0ULL &&
        pci_irq_resource_suppressed_count(&g_probe_irq) >= PCI_IRQ_STORM_THRESHOLD;
    report("UNACKNOWLEDGED IRQ ENTERS STORMED / MASKED STATE", stormed);
    if (!stormed) goto fail;

    u64 owner = g_probe_program.process.id;
    u64 thread_id = g_probe_program.thread.id;
    bool stopped = owner && thread_id && task_quiesce_process(&g_probe_program.process);
    CapabilityTable *driver_caps = process_capabilities(&g_probe_program.process);
    bool authority_gone = stopped && driver_caps && !capability_table_count(driver_caps) &&
        !process_thread_count(&g_probe_program.process) &&
        process_exit_queue_pending_count(&g_probe_exit_queue) == 1U;
    report("SUPERVISOR TERMINATES HUNG DRIVER / REVOKES AUTHORITY", authority_gone);
    if (!authority_gone) goto fail;

    ProcessExitInfo exit_info;
    k_memset(&exit_info, 0, sizeof(exit_info));
    bool terminated_record = process_exit_queue_try_receive(&g_probe_exit_queue, &exit_info) &&
        exit_info.reason == PROCESS_EXIT_TERMINATED &&
        exit_info.process_id == owner && exit_info.thread_id == thread_id &&
        process_exit_queue_pending_count(&g_probe_exit_queue) == 0U &&
        process_exit_queue_watch_count(&g_probe_exit_queue) == 0U;
    report("HUNG DRIVER TERMINATION EVENT RETAINED", terminated_record);
    if (!terminated_record) goto fail;

    bool contained = pci_assignment_force_quarantine(&g_probe_assignment, owner) &&
        pci_irq_resource_force_disarm(&g_probe_irq, owner) &&
        pci_dma_submission_force_quarantine(&g_probe_dma_submission) &&
        pci_assignment_state(&g_probe_assignment) == PCI_ASSIGNMENT_QUARANTINED &&
        pci_irq_resource_state(&g_probe_irq) == PCI_IRQ_STATE_DISARMED &&
        !pci_irq_resource_pending_sequence(&g_probe_irq) &&
        pci_irq_resource_delivery_masked(&g_probe_irq) &&
        pci_dma_submission_state(&g_probe_dma_submission) == PCI_DMA_SUBMISSION_QUARANTINED &&
        pci_dma_buffer_state(&g_probe_dma_buffer) == PCI_DMA_BUFFER_QUARANTINED &&
        pci_assignment_dma_gate_active(&g_probe_assignment) &&
        pci_assignment_active_access_count(&g_probe_assignment) == 1U &&
        !pci_dma_buffer_destroy(&g_probe_dma_buffer);
    report("SUPERVISOR MASKS STORM / RETAINS ACTIVE DMA QUARANTINE", contained);
    if (!contained) goto fail;

    bool reset = pci_dma_submission_reset_complete(&g_probe_dma_submission) &&
        !pci_assignment_dma_gate_active(&g_probe_assignment) &&
        pci_assignment_active_access_count(&g_probe_assignment) == 0U &&
        pci_dma_buffer_state(&g_probe_dma_buffer) == PCI_DMA_BUFFER_READY;
    report("VIRTUAL DEVICE RESET RELEASES HOSTILE DMA QUARANTINE", reset);
    if (!reset) goto fail;

    bool reclaimed = pci_irq_resource_destroy(&g_probe_irq) &&
        pci_dma_buffer_destroy(&g_probe_dma_buffer) &&
        pci_assignment_child_count(&g_probe_assignment) == 0U &&
        pci_assignment_complete_quarantine(&g_probe_assignment, owner) &&
        pci_assignment_destroy(&g_probe_assignment) &&
        program_terminate(&g_probe_program) && probe_close_exit_queue();
    if (reclaimed) {
        reclaimed = probe_revoke_kernel_cap(&g_probe_kernel_send,
                &g_probe_kernel_send_cap) &&
            probe_revoke_kernel_cap(&g_probe_kernel_receive,
                &g_probe_kernel_receive_cap) &&
            probe_revoke_kernel_cap(&g_probe_command_grant,
                &g_probe_command_grant_cap) &&
            probe_revoke_kernel_cap(&g_probe_reply_grant,
                &g_probe_reply_grant_cap);
    }
    if (reclaimed && endpoint_destroy(&g_probe_command)) {
        g_probe_command_created = false;
    } else if (reclaimed) {
        reclaimed = false;
    }
    if (reclaimed && endpoint_destroy(&g_probe_reply)) {
        g_probe_reply_created = false;
    } else if (reclaimed) {
        reclaimed = false;
    }
    report("HOSTILE DRIVER / ALL DEVICE + PROCESS RESOURCES RECLAIMED", reclaimed);
    if (!reclaimed) goto fail;

    bool baseline_restored = baseline_matches(baseline);
    report("HOSTILE DRIVER FULL RESOURCE BASELINE", baseline_restored);
    return baseline_restored;

fail:
    (void)probe_cleanup_all();
    return false;
}

static bool revoke_if_live(CapabilityHandle *handle) {
    if (!handle || *handle == CAPABILITY_INVALID_HANDLE) return true;
    if (!process_storage_in_use(&g_driver)) return false;
    CapabilityTable *caps = process_capabilities(&g_driver);
    if (!caps || !capability_revoke(caps, *handle)) return false;
    *handle = CAPABILITY_INVALID_HANDLE;
    return true;
}

static const PciDevice *find_inert_function(u16 *out_command) {
    if (out_command) *out_command = 0U;
    for (u32 i = 0U; i < pci_device_count(); ++i) {
        const PciDevice *device = pci_device(i);
        if (!device || !pci_device_is_enumerated(device)) continue;
        u16 command = pci_config_read16(device->address, PCI_RECOVERY_COMMAND_OFFSET);
        if (command == 0xFFFFU) continue;
        u16 unsafe = PCI_COMMAND_IO_SPACE | PCI_COMMAND_MEMORY_SPACE |
            PCI_COMMAND_BUS_MASTER;
        if ((command & unsafe) || !(command & PCI_COMMAND_INT_DISABLE)) continue;
        if (out_command) *out_command = command;
        return device;
    }
    return 0;
}

static bool cleanup_all(void) {
    bool ok = probe_cleanup_all();
    if (pci_device_recovery_valid(&g_recovery) &&
        pci_device_recovery_state(&g_recovery) == PCI_DEVICE_RECOVERY_QUIESCED &&
        !pci_device_recovery_reset_required(&g_recovery) &&
        !pci_assignment_bus_master_exposed(&g_assignment) &&
        !pci_device_recovery_complete_without_reset(&g_recovery)) ok = false;

    if (pci_dma_submission_valid(&g_dma_submission)) {
        if (pci_dma_submission_state(&g_dma_submission) == PCI_DMA_SUBMISSION_ACTIVE &&
            !pci_dma_submission_force_quarantine(&g_dma_submission)) ok = false;
        if (pci_dma_submission_state(&g_dma_submission) == PCI_DMA_SUBMISSION_QUARANTINED &&
            !pci_dma_submission_reset_complete(&g_dma_submission)) ok = false;
    }

    if (pci_dma_buffer_valid(&g_dma_buffer)) {
        u64 owner = pci_dma_buffer_owner_process_id(&g_dma_buffer);
        if (!pci_dma_buffer_force_unmap(&g_dma_buffer, owner)) ok = false;
        if (pci_dma_buffer_state(&g_dma_buffer) == PCI_DMA_BUFFER_DEVICE_ACTIVE &&
            !pci_dma_buffer_force_quarantine(&g_dma_buffer, owner)) ok = false;
        if (pci_dma_buffer_state(&g_dma_buffer) == PCI_DMA_BUFFER_QUARANTINED &&
            !pci_dma_buffer_reset_complete(&g_dma_buffer, owner)) ok = false;
    }

    if (pci_irq_resource_valid(&g_irq)) {
        u64 owner = pci_irq_resource_owner_process_id(&g_irq);
        if (!pci_irq_resource_force_disarm(&g_irq, owner)) ok = false;
    }

    if (!revoke_if_live(&g_irq_event_cap)) ok = false;
    if (!revoke_if_live(&g_irq_cap)) ok = false;
    if (pci_irq_resource_valid(&g_irq) && !pci_irq_resource_destroy(&g_irq)) ok = false;
    if (!revoke_if_live(&g_dma_buffer_cap)) ok = false;
    if (pci_dma_buffer_valid(&g_dma_buffer) && !pci_dma_buffer_destroy(&g_dma_buffer)) ok = false;
    if (!revoke_if_live(&g_assignment_cap)) ok = false;

    if (pci_device_recovery_valid(&g_recovery) &&
        pci_device_recovery_state(&g_recovery) == PCI_DEVICE_RECOVERY_DMA_SAFE &&
        !pci_device_recovery_complete_assignment(&g_recovery)) ok = false;

    if (pci_assignment_valid(&g_assignment)) {
        u64 owner = pci_assignment_owner_process_id(&g_assignment);
        PciAssignmentState state = pci_assignment_state(&g_assignment);
        if (state == PCI_ASSIGNMENT_ASSIGNED) {
            if (!pci_assignment_begin_quiesce(&g_assignment, owner) ||
                !pci_assignment_complete_release(&g_assignment, owner)) ok = false;
        } else if (state == PCI_ASSIGNMENT_QUARANTINED) {
            if (!pci_assignment_complete_quarantine(&g_assignment, owner)) ok = false;
        }
        if (pci_assignment_state(&g_assignment) == PCI_ASSIGNMENT_AVAILABLE &&
            !pci_assignment_destroy(&g_assignment)) ok = false;
    }
    if (process_storage_in_use(&g_driver) && !process_destroy(&g_driver)) ok = false;
    return ok;
}

void pci_device_recovery_test_cleanup_run(void) {
    terminal_writeln("PCI DEVICE RECOVERY CLEANUP:");
    report("RECOVERY / DRIVER / DEVICE BASELINE", cleanup_all());
}

void pci_driver_hostile_test_cleanup_run(void) {
    terminal_writeln("RING3 HOSTILE PCI DRIVER CLEANUP:");
    report("HOSTILE DRIVER / DEVICE BASELINE", cleanup_all());
}

void pci_driver_hostile_test_run(void) {
    terminal_writeln("RING3 HOSTILE PCI DRIVER / IRQ STORM RECOVERY TEST:");

    bool clean = cleanup_all();
    report("CLEAN START", clean);

    PciDeviceRecoveryBaseline baseline;
    k_memset(&baseline, 0, sizeof(baseline));
    bool captured = clean && capture_baseline(&baseline);
    report("RESOURCE BASELINE CAPTURED", captured);
    if (!captured) goto fail;

    bool hostile = ring3_hostile_driver_contract(&baseline);
    report("HUNG RING3 DRIVER / IRQ STORM / DMA RECOVERY", hostile);
    if (!hostile) goto fail;

    bool restored = baseline_matches(&baseline);
    report("POST-RECOVERY FULL BASELINE", restored);
    if (!restored) goto fail;

    terminal_set_color(terminal_accent_color());
    terminal_writeln("RING3 HOSTILE PCI DRIVER / IRQ STORM RECOVERY TEST: PASS");
    terminal_set_color(terminal_default_color());
    return;

fail:
    report("FAILURE CLEANUP / FULL BASELINE",
        cleanup_all() && (!captured || baseline_matches(&baseline)));
    terminal_set_color(terminal_error_color());
    terminal_writeln("RING3 HOSTILE PCI DRIVER / IRQ STORM RECOVERY TEST: FAILED");
    terminal_set_color(terminal_default_color());
}

void pci_device_recovery_test_run(void) {
    terminal_writeln("PCI DEVICE QUIESCENCE / RECOVERY TEST:");

    bool clean = cleanup_all();
    report("CLEAN START", clean);
    if (!clean) return;

    PciDeviceRecoveryBaseline baseline;
    if (!capture_baseline(&baseline)) return;

    bool flr_discovery = flr_discovery_contract();
    report("PCIE CAPABILITY WALK / FLR DISCOVERY READ-ONLY", flr_discovery);
    if (!flr_discovery) goto fail;

    bool ring3_driver = ring3_virtual_driver_contract(&baseline);
    report("RING3 MEDIATED VIRTUAL DEVICE / DMA + IRQ + FAULT", ring3_driver);
    if (!ring3_driver) goto fail;

    u16 command_before = 0U;
    const PciDevice *device = find_inert_function(&command_before);
    if (!device) {
        report("NO INERT PCI FUNCTION / DESTRUCTIVE RECOVERY REFUSED", true);
        report("RECOVERY BASELINE UNCHANGED", baseline_matches(&baseline));
        terminal_set_color(terminal_accent_color());
        terminal_writeln("PCI DEVICE QUIESCENCE / RECOVERY TEST: PASS");
        terminal_set_color(terminal_default_color());
        return;
    }
    report("INERT PCI FUNCTION AVAILABLE", true);

    bool setup = pci_assignment_create(&g_assignment, device) &&
        process_create(&g_driver) &&
        pci_assignment_claim(&g_assignment, &g_driver, &g_assignment_cap) &&
        pci_irq_resource_create(&g_irq, &g_assignment, &g_driver, g_assignment_cap,
            &g_irq_cap, &g_irq_event_cap) &&
        pci_irq_resource_arm(&g_irq, &g_driver, g_irq_cap) &&
        pci_dma_buffer_create(&g_dma_buffer, &g_assignment, &g_driver,
            g_assignment_cap, PCI_RECOVERY_TEST_DMA_PAGES, &g_dma_buffer_cap) &&
        pci_dma_submission_begin(&g_dma_submission, &g_assignment, &g_driver,
            g_assignment_cap, &g_dma_buffer, g_dma_buffer_cap, 0ULL, 128ULL,
            PCI_DMA_BIDIRECTIONAL);
    report("LOGICAL IRQ + DMA DRIVER STATE", setup);
    if (!setup) goto fail;

    bool pre_recovery = pci_irq_resource_state(&g_irq) == PCI_IRQ_STATE_ARMED &&
        pci_dma_submission_state(&g_dma_submission) == PCI_DMA_SUBMISSION_ACTIVE &&
        pci_assignment_dma_gate_active(&g_assignment) &&
        pci_assignment_active_access_count(&g_assignment) == 2U;
    report("ACTIVE ACCESS PINS BEFORE RECOVERY", pre_recovery);
    if (!pre_recovery) goto fail;

    bool begun = pci_device_recovery_begin(&g_recovery, &g_assignment, &g_driver) &&
        pci_device_recovery_state(&g_recovery) == PCI_DEVICE_RECOVERY_QUIESCED &&
        !pci_device_recovery_reset_required(&g_recovery) &&
        !pci_assignment_bus_master_exposed(&g_assignment) &&
        pci_assignment_state(&g_assignment) == PCI_ASSIGNMENT_QUARANTINED &&
        pci_irq_resource_state(&g_irq) == PCI_IRQ_STATE_DISARMED &&
        pci_dma_submission_state(&g_dma_submission) == PCI_DMA_SUBMISSION_QUARANTINED &&
        pci_dma_buffer_state(&g_dma_buffer) == PCI_DMA_BUFFER_QUARANTINED &&
        pci_assignment_dma_gate_active(&g_assignment) &&
        pci_assignment_active_access_count(&g_assignment) == 1U;
    report("RECOVERY QUARANTINES AUTHORITY / IRQ / DMA", begun);
    if (!begun) goto fail;

    u8 flr_capability = 0U;
    bool flr_expected = pci_flr_supported(device, &flr_capability);
    bool flr_available = pci_device_recovery_flr_available(&g_recovery);
    bool flr_contract = flr_available == flr_expected &&
        pci_device_recovery_reset_method(&g_recovery) == PCI_DEVICE_RESET_NONE;
    report("RECOVERY FLR BACKEND AVAILABILITY", flr_contract);
    if (!flr_contract) goto fail;

    u16 command_quiesced = pci_config_read16(device->address,
        PCI_RECOVERY_COMMAND_OFFSET);
    bool command_safe = command_quiesced == command_before &&
        !(command_quiesced & (PCI_COMMAND_IO_SPACE | PCI_COMMAND_MEMORY_SPACE |
            PCI_COMMAND_BUS_MASTER)) &&
        (command_quiesced & PCI_COMMAND_INT_DISABLE);
    report("PCI COMMAND REMAINS QUIESCED", command_safe);
    if (!command_safe) goto fail;

    bool no_reset = pci_device_recovery_complete_without_reset(&g_recovery) &&
        pci_device_recovery_state(&g_recovery) == PCI_DEVICE_RECOVERY_DMA_SAFE &&
        !pci_dma_submission_valid(&g_dma_submission) &&
        pci_dma_buffer_state(&g_dma_buffer) == PCI_DMA_BUFFER_READY &&
        !pci_assignment_dma_gate_active(&g_assignment) &&
        pci_assignment_active_access_count(&g_assignment) == 0U;
    report("BUS-MASTER-NEVER-ENABLED PROOF RELEASES DMA QUARANTINE", no_reset);
    if (!no_reset) goto fail;

    bool caps = revoke_if_live(&g_irq_event_cap) && revoke_if_live(&g_irq_cap) &&
        revoke_if_live(&g_dma_buffer_cap);
    bool children = caps && pci_irq_resource_destroy(&g_irq) &&
        pci_dma_buffer_destroy(&g_dma_buffer) &&
        pci_assignment_child_count(&g_assignment) == 0U;
    report("CHILD DEVICE RESOURCES RECLAIMED", children);
    if (!children) goto fail;

    bool released = revoke_if_live(&g_assignment_cap) &&
        pci_device_recovery_complete_assignment(&g_recovery) &&
        !pci_device_recovery_valid(&g_recovery) &&
        pci_assignment_state(&g_assignment) == PCI_ASSIGNMENT_AVAILABLE &&
        pci_assignment_destroy(&g_assignment) && process_destroy(&g_driver) &&
        pci_config_read16(device->address, PCI_RECOVERY_COMMAND_OFFSET) == command_before &&
        baseline_matches(&baseline);
    report("QUARANTINE RELEASE / FULL BASELINE", released);
    if (!released) goto fail;

    terminal_set_color(terminal_accent_color());
    terminal_writeln("PCI DEVICE QUIESCENCE / RECOVERY TEST: PASS");
    terminal_set_color(terminal_default_color());
    return;

fail:
    report("FAILURE CLEANUP", cleanup_all() && baseline_matches(&baseline));
    terminal_set_color(terminal_error_color());
    terminal_writeln("PCI DEVICE QUIESCENCE / RECOVERY TEST: FAILED");
    terminal_set_color(terminal_default_color());
}
