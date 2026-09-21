#include "boot_archive_portal.h"

#include "ipc.h"
#include "lib.h"
#include "process.h"
#include "scheduler.h"
#include "task.h"
#include "../include/boot_archive_portal_abi.h"

static void reply_clear(IpcMessage *reply) {
    if (reply) k_memset(reply, 0, sizeof(*reply));
}

static u64 pack_bytes(const u8 *data, u32 count) {
    u64 value = 0ULL;
    if (!data) return 0ULL;
    for (u32 i = 0; i < count; ++i) value |= (u64)data[i] << (i * 8U);
    return value;
}

static void portal_reply_error(IpcMessage *reply, u64 code, u64 incarnation, u64 detail) {
    reply_clear(reply);
    reply->word_count = 4U;
    reply->words[0] = JCOS_BOOT_ARCHIVE_PORTAL_HEADER(
        code, JCOS_BOOT_ARCHIVE_PORTAL_PROTOCOL_VERSION, 0U);
    reply->words[1] = incarnation;
    reply->words[2] = detail;
}

static void portal_handle(BootArchivePortal *portal, const IpcMessage *request, IpcMessage *reply) {
    reply_clear(reply);
    if (!portal || !request || request->word_count < 2U) {
        portal_reply_error(reply, JCOS_BOOT_ARCHIVE_PORTAL_REPLY_INVALID_REQUEST, 0ULL, 0ULL);
        return;
    }

    u64 header = request->words[0];
    u64 operation = JCOS_BOOT_ARCHIVE_PORTAL_HEADER_CODE(header);
    u64 version = JCOS_BOOT_ARCHIVE_PORTAL_HEADER_VERSION(header);
    u64 count = JCOS_BOOT_ARCHIVE_PORTAL_HEADER_COUNT(header);
    u64 incarnation = request->words[1];

    if (!incarnation) {
        portal_reply_error(reply, JCOS_BOOT_ARCHIVE_PORTAL_REPLY_INVALID_REQUEST, 0ULL, operation);
        return;
    }
    if (version != JCOS_BOOT_ARCHIVE_PORTAL_PROTOCOL_VERSION) {
        portal_reply_error(reply, JCOS_BOOT_ARCHIVE_PORTAL_REPLY_INCOMPATIBLE_VERSION,
            incarnation, version);
        return;
    }

    if (operation == JCOS_BOOT_ARCHIVE_PORTAL_OP_INFO) {
        if (request->word_count != 2U || count) {
            portal_reply_error(reply, JCOS_BOOT_ARCHIVE_PORTAL_REPLY_INVALID_REQUEST,
                incarnation, operation);
            return;
        }
        ++portal->info_count;
        reply->word_count = 4U;
        reply->words[0] = JCOS_BOOT_ARCHIVE_PORTAL_HEADER(
            JCOS_BOOT_ARCHIVE_PORTAL_REPLY_INFO,
            JCOS_BOOT_ARCHIVE_PORTAL_PROTOCOL_VERSION, 0U);
        reply->words[1] = incarnation;
        reply->words[2] = portal->archive_size;
        reply->words[3] = JCOS_BOOT_ARCHIVE_PORTAL_MAX_READ_BYTES;
        return;
    }

    if (operation == JCOS_BOOT_ARCHIVE_PORTAL_OP_READ) {
        if (request->word_count != 3U || !count ||
            count > JCOS_BOOT_ARCHIVE_PORTAL_MAX_READ_BYTES) {
            portal_reply_error(reply, JCOS_BOOT_ARCHIVE_PORTAL_REPLY_INVALID_REQUEST,
                incarnation, operation);
            return;
        }
        u64 offset = request->words[2];
        if (offset >= portal->archive_size || count > portal->archive_size - offset) {
            portal_reply_error(reply, JCOS_BOOT_ARCHIVE_PORTAL_REPLY_OUT_OF_RANGE,
                incarnation, offset);
            return;
        }

        ++portal->read_count;
        portal->last_read_offset = offset;
        reply->word_count = 4U;
        reply->words[0] = JCOS_BOOT_ARCHIVE_PORTAL_HEADER(
            JCOS_BOOT_ARCHIVE_PORTAL_REPLY_DATA,
            JCOS_BOOT_ARCHIVE_PORTAL_PROTOCOL_VERSION, count);
        reply->words[1] = incarnation;
        reply->words[2] = offset;
        reply->words[3] = pack_bytes(portal->archive + offset, (u32)count);
        return;
    }

    portal_reply_error(reply, JCOS_BOOT_ARCHIVE_PORTAL_REPLY_INVALID_REQUEST,
        incarnation, operation);
}

static void portal_thread_main(void *argument) {
    BootArchivePortal *portal = (BootArchivePortal *)argument;
    Process *kernel = process_kernel();
    if (!portal || !kernel) scheduler_exit_current();

    for (;;) {
        IpcMessage request;
        IpcMessage reply;
        k_memset(&request, 0, sizeof(request));
        k_memset(&reply, 0, sizeof(reply));
        if (!ipc_receive_blocking(kernel, portal->kernel_request_receive_handle, &request)) break;
        portal_handle(portal, &request, &reply);
        if (!reply.word_count ||
            !ipc_send_blocking(kernel, portal->kernel_reply_send_handle, &reply)) break;
    }

    scheduler_exit_current();
}

static bool portal_needs_cleanup(const BootArchivePortal *portal) {
    return portal && (portal->request_endpoint_created || portal->reply_endpoint_created ||
        portal->thread_created || portal->kernel_request_receive_cap ||
        portal->kernel_request_transfer_cap || portal->kernel_reply_send_cap ||
        portal->kernel_reply_transfer_cap || portal->active || portal->archive ||
        portal->archive_size ||
        portal->kernel_request_receive_handle != CAPABILITY_INVALID_HANDLE ||
        portal->kernel_request_transfer_handle != CAPABILITY_INVALID_HANDLE ||
        portal->kernel_reply_send_handle != CAPABILITY_INVALID_HANDLE ||
        portal->kernel_reply_transfer_handle != CAPABILITY_INVALID_HANDLE);
}

static bool drain_endpoint(Process *kernel, CapabilityHandle receive_handle, Endpoint *endpoint) {
    if (!kernel || !receive_handle || !endpoint) return false;
    while (endpoint_message_ready(endpoint)) {
        IpcMessage discard;
        k_memset(&discard, 0, sizeof(discard));
        if (!ipc_try_receive(kernel, receive_handle, &discard)) return false;
    }
    return !endpoint_message_ready(endpoint);
}

bool boot_archive_portal_start(BootArchivePortal *portal, const void *archive, u64 archive_size) {
    if (!portal || !archive || !archive_size || portal_needs_cleanup(portal)) return false;
    k_memset(portal, 0, sizeof(*portal));

    Process *kernel = process_kernel();
    Thread *current = thread_current();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!kernel || !current || current->process != kernel ||
        current->state != THREAD_STATE_RUNNING || !current->on_run_queue || !caps) return false;

    portal->archive = (const u8 *)archive;
    portal->archive_size = archive_size;

    if (!endpoint_create(&portal->request_endpoint)) goto fail;
    portal->request_endpoint_created = true;
    if (!endpoint_create(&portal->reply_endpoint)) goto fail;
    portal->reply_endpoint_created = true;

    if (!capability_insert(caps, &portal->request_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE, &portal->kernel_request_receive_handle)) goto fail;
    portal->kernel_request_receive_cap = true;
    if (!capability_insert(caps, &portal->request_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND | CAPABILITY_RIGHT_TRANSFER,
            &portal->kernel_request_transfer_handle)) goto fail;
    portal->kernel_request_transfer_cap = true;

    if (!capability_insert(caps, &portal->reply_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND, &portal->kernel_reply_send_handle)) goto fail;
    portal->kernel_reply_send_cap = true;
    if (!capability_insert(caps, &portal->reply_endpoint, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE | CAPABILITY_RIGHT_TRANSFER,
            &portal->kernel_reply_transfer_handle)) goto fail;
    portal->kernel_reply_transfer_cap = true;

    if (!thread_create(&portal->thread, kernel)) goto fail;
    portal->thread_created = true;
    if (!thread_prepare_kernel(&portal->thread, portal_thread_main, portal)) goto fail;
    if (!scheduler_add(&portal->thread)) goto fail;

    portal->active = true;
    return true;

fail:
    (void)boot_archive_portal_stop(portal);
    return false;
}

bool boot_archive_portal_stop(BootArchivePortal *portal) {
    if (!portal) return false;
    if (!portal_needs_cleanup(portal)) return true;
    if (thread_current() == &portal->thread || portal->thread.state == THREAD_STATE_RUNNING) return false;

    if (portal->thread_created && portal->thread.state != THREAD_STATE_DEAD) {
        if (!task_terminate_thread(&portal->thread)) return false;
    }
    if (portal->thread_created && (portal->thread.on_run_queue || thread_wait_active(&portal->thread))) return false;

    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!kernel || !caps) return false;

    if (portal->request_endpoint_created && portal->kernel_request_receive_cap &&
        !drain_endpoint(kernel, portal->kernel_request_receive_handle, &portal->request_endpoint)) return false;
    if (portal->reply_endpoint_created && portal->kernel_reply_transfer_cap) {
        /* The kernel does not normally RECEIVE replies. Use the transfer-authority
         * handle only for cleanup; it carries RECEIVE as required by its role. */
        if (!drain_endpoint(kernel, portal->kernel_reply_transfer_handle, &portal->reply_endpoint)) return false;
    }

    if (portal->kernel_request_receive_cap) {
        if (!capability_revoke(caps, portal->kernel_request_receive_handle)) return false;
        portal->kernel_request_receive_cap = false;
        portal->kernel_request_receive_handle = CAPABILITY_INVALID_HANDLE;
    }
    if (portal->kernel_request_transfer_cap) {
        if (!capability_revoke(caps, portal->kernel_request_transfer_handle)) return false;
        portal->kernel_request_transfer_cap = false;
        portal->kernel_request_transfer_handle = CAPABILITY_INVALID_HANDLE;
    }
    if (portal->kernel_reply_send_cap) {
        if (!capability_revoke(caps, portal->kernel_reply_send_handle)) return false;
        portal->kernel_reply_send_cap = false;
        portal->kernel_reply_send_handle = CAPABILITY_INVALID_HANDLE;
    }
    if (portal->kernel_reply_transfer_cap) {
        if (!capability_revoke(caps, portal->kernel_reply_transfer_handle)) return false;
        portal->kernel_reply_transfer_cap = false;
        portal->kernel_reply_transfer_handle = CAPABILITY_INVALID_HANDLE;
    }

    if (portal->thread_created) {
        if (!thread_destroy(&portal->thread)) return false;
        portal->thread_created = false;
    }
    if (portal->request_endpoint_created) {
        if (!endpoint_destroy(&portal->request_endpoint)) return false;
        portal->request_endpoint_created = false;
    }
    if (portal->reply_endpoint_created) {
        if (!endpoint_destroy(&portal->reply_endpoint)) return false;
        portal->reply_endpoint_created = false;
    }

    portal->archive = 0;
    portal->archive_size = 0ULL;
    portal->info_count = 0ULL;
    portal->read_count = 0ULL;
    portal->last_read_offset = 0ULL;
    portal->active = false;
    return true;
}

bool boot_archive_portal_grant_specs(const BootArchivePortal *portal,
    ProgramGrantSpec *request_send, ProgramGrantSpec *reply_receive) {
    if (!portal || !request_send || !reply_receive || !boot_archive_portal_active(portal) ||
        !portal->kernel_request_transfer_cap || !portal->kernel_reply_transfer_cap) return false;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps) return false;

    k_memset(request_send, 0, sizeof(*request_send));
    request_send->authority_table = caps;
    request_send->authority_handle = portal->kernel_request_transfer_handle;
    request_send->object = (void *)&portal->request_endpoint;
    request_send->type = CAPABILITY_TYPE_ENDPOINT;
    request_send->rights = CAPABILITY_RIGHT_SEND;

    k_memset(reply_receive, 0, sizeof(*reply_receive));
    reply_receive->authority_table = caps;
    reply_receive->authority_handle = portal->kernel_reply_transfer_handle;
    reply_receive->object = (void *)&portal->reply_endpoint;
    reply_receive->type = CAPABILITY_TYPE_ENDPOINT;
    reply_receive->rights = CAPABILITY_RIGHT_RECEIVE;
    return true;
}

bool boot_archive_portal_active(const BootArchivePortal *portal) {
    return portal && portal->active && portal->archive && portal->archive_size &&
        portal->request_endpoint_created && portal->reply_endpoint_created &&
        portal->thread_created && portal->kernel_request_receive_cap &&
        portal->kernel_request_transfer_cap && portal->kernel_reply_send_cap &&
        portal->kernel_reply_transfer_cap && portal->thread.state != THREAD_STATE_DEAD;
}

bool boot_archive_portal_idle(const BootArchivePortal *portal) {
    return boot_archive_portal_active(portal) &&
        portal->thread.state == THREAD_STATE_BLOCKED && !portal->thread.on_run_queue &&
        portal->thread.interrupt_context_ready && portal->thread.interrupt_rsp &&
        thread_wait_matches(&portal->thread, THREAD_WAIT_IPC_RECEIVE, &portal->request_endpoint) &&
        endpoint_receiver_waiting(&portal->request_endpoint) &&
        !endpoint_message_ready(&portal->reply_endpoint) &&
        !endpoint_sender_waiting(&portal->reply_endpoint);
}

bool boot_archive_portal_present(const BootArchivePortal *portal) {
    return portal_needs_cleanup(portal);
}

u64 boot_archive_portal_archive_size(const BootArchivePortal *portal) {
    return boot_archive_portal_active(portal) ? portal->archive_size : 0ULL;
}

u64 boot_archive_portal_info_count(const BootArchivePortal *portal) {
    return portal ? portal->info_count : 0ULL;
}

u64 boot_archive_portal_read_count(const BootArchivePortal *portal) {
    return portal ? portal->read_count : 0ULL;
}

u64 boot_archive_portal_last_read_offset(const BootArchivePortal *portal) {
    return portal ? portal->last_read_offset : 0ULL;
}


bool boot_archive_portal_extent(const BootArchivePortal *portal, u64 offset, u64 size,
    const u8 **out_data) {
    if (out_data) *out_data = 0;
    if (!out_data || !boot_archive_portal_active(portal) || !size ||
        offset >= portal->archive_size || size > portal->archive_size - offset) return false;
    *out_data = portal->archive + offset;
    return true;
}
