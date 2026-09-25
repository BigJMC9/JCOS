#include "program_launch_test.h"
#include "program_rollback_test.h"

#include "address_space.h"
#include "capability.h"
#include "endpoint.h"
#include "interrupts.h"
#include "ipc.h"
#include "lib.h"
#include "physmap.h"
#include "pmm.h"
#include "process.h"
#include "program.h"
#include "scheduler.h"
#include "supervisor.h"
#include "terminal.h"
#include "thread.h"
#include "timer.h"
#include "user_elf_test.h"
#include "vfs.h"
#include "vmm.h"
#include "../../include/supervisor_protocol.h"

#define PROGRAM_TEST_PATH "/bin/supervisor.elf"
#define PROGRAM_TEST_TIMEOUT_SECONDS 2ULL
#define PROGRAM_TEST_BORROWED_BASE   0x0000020000000000ULL
#define PROGRAM_TEST_BORROWED_OFFSET 0x80ULL
#define PROGRAM_TEST_BORROWED_SIZE   0x400ULL

static ProgramInstance g_program;
static Endpoint g_command;
static Endpoint g_reply;
static CapabilityHandle g_kernel_send;
static CapabilityHandle g_kernel_receive;
static CapabilityHandle g_command_grant;
static CapabilityHandle g_reply_grant;
static bool g_command_created;
static bool g_reply_created;
static bool g_kernel_send_cap;
static bool g_kernel_receive_cap;
static bool g_command_grant_cap;
static bool g_reply_grant_cap;
static frame_t g_borrowed_frame = FRAME_INVALID;

typedef struct {
    u64 free_pages;
    u32 process_count;
    u32 address_space_count;
    u32 thread_count;
    u32 endpoint_count;
    u32 kernel_cap_count;
    u64 kernel_thread_count;
    u64 scheduler_count;
    u64 supervisor_pid;
    u64 supervisor_tid;
} ProgramTestBaseline;

static void report(const char *name, bool pass) {
    terminal_write("  ");
    terminal_write(name);
    terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static u64 timeout_ticks(void) {
    if (!timer_initialized()) return 0;
    u32 frequency = timer_frequency();
    return frequency ? (u64)frequency * PROGRAM_TEST_TIMEOUT_SECONDS : 0;
}

static bool supervisor_ok(u64 pid, u64 tid) {
    u64 reply = 0;
    u64 cookie = 0x50524F474C41554EULL;
    return pid && tid && supervisor_process_id() == pid && supervisor_thread_id() == tid &&
        supervisor_ping(cookie, &reply) && reply == cookie &&
        supervisor_process_id() == pid && supervisor_thread_id() == tid;
}

static void capture_baseline(ProgramTestBaseline *baseline) {
    if (!baseline) return;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    baseline->free_pages = pmm_stats().free_pages;
    baseline->process_count = process_object_count();
    baseline->address_space_count = address_space_object_count();
    baseline->thread_count = thread_object_count();
    baseline->endpoint_count = endpoint_object_count();
    baseline->kernel_cap_count = caps ? capability_table_count(caps) : 0;
    baseline->kernel_thread_count = kernel ? process_thread_count(kernel) : 0;
    baseline->scheduler_count = scheduler_thread_count();
    baseline->supervisor_pid = supervisor_process_id();
    baseline->supervisor_tid = supervisor_thread_id();
}

static bool baseline_matches(const ProgramTestBaseline *baseline) {
    if (!baseline) return false;
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    return kernel && caps && pmm_stats().free_pages == baseline->free_pages &&
        process_object_count() == baseline->process_count &&
        address_space_object_count() == baseline->address_space_count &&
        thread_object_count() == baseline->thread_count &&
        endpoint_object_count() == baseline->endpoint_count &&
        capability_table_count(caps) == baseline->kernel_cap_count &&
        process_thread_count(kernel) == baseline->kernel_thread_count &&
        scheduler_thread_count() == baseline->scheduler_count &&
        supervisor_process_id() == baseline->supervisor_pid &&
        supervisor_thread_id() == baseline->supervisor_tid;
}

static bool cleanup_fixture(void) {
    if (!program_rollback_test_cleanup()) return false;
    if (program_instance_needs_cleanup(&g_program) && !program_terminate(&g_program)) return false;
    if (g_borrowed_frame != FRAME_INVALID) {
        /*
        * Only free the test-owned backing frame after ProgramInstance has
        * relinquished every borrowed mapping of it.
        */
        if (!frame_free(g_borrowed_frame)) return false;

        g_borrowed_frame = FRAME_INVALID;
    }

    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!caps) return false;

    if (g_kernel_send_cap) {
        if (!capability_revoke(caps, g_kernel_send)) return false;
        g_kernel_send_cap = false;
        g_kernel_send = CAPABILITY_INVALID_HANDLE;
    }
    if (g_kernel_receive_cap) {
        if (!capability_revoke(caps, g_kernel_receive)) return false;
        g_kernel_receive_cap = false;
        g_kernel_receive = CAPABILITY_INVALID_HANDLE;
    }
    if (g_command_grant_cap) {
        if (!capability_revoke(caps, g_command_grant)) return false;
        g_command_grant_cap = false;
        g_command_grant = CAPABILITY_INVALID_HANDLE;
    }
    if (g_reply_grant_cap) {
        if (!capability_revoke(caps, g_reply_grant)) return false;
        g_reply_grant_cap = false;
        g_reply_grant = CAPABILITY_INVALID_HANDLE;
    }

    Endpoint *endpoints[2] = { &g_command, &g_reply };
    bool *created[2] = { &g_command_created, &g_reply_created };
    for (u32 i = 0; i < 2U; ++i) {
        Endpoint *endpoint = endpoints[i];
        if (!*created[i]) continue;
        if (endpoint_message_ready(endpoint)) {
            IpcMessage discard;
            k_memset(&discard, 0, sizeof(discard));
            if (!endpoint_try_receive(endpoint, &discard)) return false;
        }
        if (endpoint_receiver_waiting(endpoint) || endpoint_sender_waiting(endpoint)) return false;
        if (!endpoint_destroy(endpoint)) return false;
        *created[i] = false;
    }
    return true;
}

void program_launch_test_cleanup_run(void) {
    terminal_writeln("PROGRAM LAUNCH CLEANUP RETRY:");
    report("RETAINED RESOURCES RELEASED", cleanup_fixture());
}

static bool create_fixture(void) {
    Process *kernel = process_kernel();
    CapabilityTable *caps = kernel ? process_capabilities(kernel) : 0;
    if (!kernel || !caps) return false;

    if (!endpoint_create(&g_command)) return false;
    g_command_created = true;
    if (!endpoint_create(&g_reply)) return false;
    g_reply_created = true;
    if (!capability_insert(caps, &g_command, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND, &g_kernel_send)) return false;
    g_kernel_send_cap = true;
    if (!capability_insert(caps, &g_reply, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE, &g_kernel_receive)) return false;
    g_kernel_receive_cap = true;
    if (!capability_insert(caps, &g_command, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE | CAPABILITY_RIGHT_TRANSFER, &g_command_grant)) return false;
    g_command_grant_cap = true;
    if (!capability_insert(caps, &g_reply, CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_SEND | CAPABILITY_RIGHT_TRANSFER, &g_reply_grant)) return false;
    g_reply_grant_cap = true;
    return true;
}

static void make_spec(ProgramLaunchSpec *spec, const VfsNode *file) {
    k_memset(spec, 0, sizeof(*spec));
    spec->file = file;
    spec->startup_grant_count = 2U;
    Process *kernel = process_kernel();
    CapabilityTable *kernel_caps = kernel ? process_capabilities(kernel) : 0;
    spec->startup_grants[0].authority_table = kernel_caps;
    spec->startup_grants[0].authority_handle = g_command_grant;
    spec->startup_grants[0].object = &g_command;
    spec->startup_grants[0].type = CAPABILITY_TYPE_ENDPOINT;
    spec->startup_grants[0].rights = CAPABILITY_RIGHT_RECEIVE;
    spec->startup_grants[1].authority_table = kernel_caps;
    spec->startup_grants[1].authority_handle = g_reply_grant;
    spec->startup_grants[1].object = &g_reply;
    spec->startup_grants[1].type = CAPABILITY_TYPE_ENDPOINT;
    spec->startup_grants[1].rights = CAPABILITY_RIGHT_SEND;
}

static bool explicit_grants_valid(void) {
    if (!g_program.process_created || !g_program.published ||
        g_program.startup_handles[0] == CAPABILITY_INVALID_HANDLE ||
        g_program.startup_handles[1] == CAPABILITY_INVALID_HANDLE) return false;
    CapabilityTable *caps = process_capabilities(&g_program.process);
    if (!caps || capability_table_count(caps) != 2U) return false;
    void *object = 0;
    if (!capability_lookup_rights(caps, g_program.startup_handles[0], CAPABILITY_TYPE_ENDPOINT,
            CAPABILITY_RIGHT_RECEIVE, &object) || object != &g_command) return false;
    object = 0;
    return capability_lookup_rights(caps, g_program.startup_handles[1], CAPABILITY_TYPE_ENDPOINT,
        CAPABILITY_RIGHT_SEND, &object) && object == &g_reply;
}

static bool send_request(u64 command, u64 cookie, IpcMessage *reply) {
    Process *kernel = process_kernel();
    u64 timeout = timeout_ticks();
    if (!kernel || !reply || !timeout) return false;
    IpcMessage request;
    k_memset(&request, 0, sizeof(request));
    k_memset(reply, 0, sizeof(*reply));
    request.word_count = command == SUPERVISOR_MESSAGE_PING ? 2U : 1U;
    request.words[0] = command;
    request.words[1] = cookie;
    if (!ipc_try_send(kernel, g_kernel_send, &request)) return false;
    return ipc_receive_blocking_for(kernel, g_kernel_receive, reply, timeout);
}

static bool wait_program_dead(void) {
    u64 timeout = timeout_ticks();
    if (!timeout) return false;
    u64 started = timer_ticks();
    while (g_program.thread.state != THREAD_STATE_DEAD) {
        if ((u64)(timer_ticks() - started) >= timeout) return false;
        if (g_program.thread.state != THREAD_STATE_READY || !g_program.thread.on_run_queue) return false;
        if (!scheduler_yield()) return false;
    }
    return !g_program.thread.on_run_queue && !g_program.thread.interrupt_context_ready &&
        !g_program.thread.interrupt_rsp;
}

static bool patch_entry_ud2(void) {
    u64 entry = g_program.image.entry;
    for (u32 i = 0; i < g_program.image.page_count; ++i) {
        UserElfPage *page = &g_program.image.pages[i];
        if (entry < page->virtual_address || entry >= page->virtual_address + VM_PAGE_SIZE) continue;
        u64 offset = entry - page->virtual_address;
        if (offset > VM_PAGE_SIZE - 2ULL) return false;
        u8 *memory = (u8 *)phys_to_virt(frame_to_phys(page->frame));
        if (!memory) return false;
        memory[offset] = 0x0FU;
        memory[offset + 1ULL] = 0x0BU;
        return true;
    }
    return false;
}

static bool borrowed_mapping_case(const ProgramLaunchSpec *base_spec, const ProgramTestBaseline *fixture) {
    if (!base_spec || !fixture || g_borrowed_frame != FRAME_INVALID || program_instance_needs_cleanup(&g_program))
        return false;

    u64 free_before = pmm_stats().free_pages;
    g_borrowed_frame = frame_alloc();

    bool allocated = g_borrowed_frame != FRAME_INVALID && free_before && pmm_stats().free_pages == free_before - 1ULL;
    report("BORROWED TEST FRAME ALLOCATED", allocated);

    if (!allocated) return false;

    /*
     * While this frame is deliberately retained by the test, the correct
     * resource baseline is one page below the surrounding fixture.
     */
    ProgramTestBaseline borrowed_baseline = *fixture;
    borrowed_baseline.free_pages = free_before - 1ULL;

    u64 physical = frame_to_phys(g_borrowed_frame);
    if (!physical) return false;

    ProgramLaunchSpec mapped;
    k_memcpy(&mapped, base_spec, sizeof(mapped));

    /*
     * Deliberately begin inside the frame rather than on its boundary.
     * The launcher should map the minimum page span containing this range.
     */
    mapped.borrowed_mapping.physical_base = physical + PROGRAM_TEST_BORROWED_OFFSET;
    mapped.borrowed_mapping.size = PROGRAM_TEST_BORROWED_SIZE;
    mapped.borrowed_mapping.virtual_base = PROGRAM_TEST_BORROWED_BASE;
    mapped.borrowed_mapping.flags = VM_WRITE | VM_UNCACHED;

    /* Device/firmware mappings must never acquire executable authority. */
    ProgramLaunchSpec executable;
    k_memcpy(&executable, &mapped, sizeof(executable));
    executable.borrowed_mapping.flags |= VM_EXEC;

    u64 launches_before = program_launch_count();

    bool executable_rejected = !program_launch(&g_program, &executable) &&
        !program_instance_needs_cleanup(&g_program) && baseline_matches(&borrowed_baseline) &&
        program_launch_count() == launches_before;

    report("EXECUTABLE BORROWED MAP REJECTED", executable_rejected);

    if (!executable_rejected) goto fail;
    if (!program_launch(&g_program, &mapped)) goto fail;

    AddressSpace *space = process_address_space(&g_program.process);
    frame_t mapped_frame = FRAME_INVALID;
    vm_flags_t flags = 0ULL;
    bool queried = space && address_space_query_page(space, PROGRAM_TEST_BORROWED_BASE, &mapped_frame, &flags);

    bool physical_ok = queried && mapped_frame == g_borrowed_frame &&
        g_program.borrowed_first_frame == g_borrowed_frame &&
        g_program.borrowed_virtual_base == PROGRAM_TEST_BORROWED_BASE && g_program.borrowed_page_count == 1U;

    report("BORROWED PHYSICAL MAP / PAGE GEOMETRY", physical_ok);

    bool permissions_ok = queried &&
        (flags & (VM_USER | VM_WRITE | VM_EXEC | VM_UNCACHED)) == (VM_USER | VM_WRITE | VM_UNCACHED);

    report("BORROWED PHYSICAL MAP / USER RW-NX", permissions_ok);
    bool uncached = queried && (flags & VM_UNCACHED) != 0ULL;
    report("BORROWED PHYSICAL MAP / UNCACHED", uncached);

    if (!physical_ok || !permissions_ok || !uncached) goto fail;

    /* Program teardown owns the mapping, but explicitly does NOT own its backing physical frame. */
    bool terminated = program_terminate(&g_program) && !program_instance_needs_cleanup(&g_program);
    report("BORROWED MAP TEARDOWN", terminated);

    if (!terminated) return false;

    /*
     * Every ordinary program allocation should now be gone, while the one
     * test-owned borrowed frame must remain allocated.
     */
    bool retained = baseline_matches(&borrowed_baseline) && pmm_stats().free_pages == free_before - 1ULL;
    report("BORROWED FRAME NOT FREED BY PROGRAM", retained);

    if (!retained) goto fail;
    if (!frame_free(g_borrowed_frame)) return false;

    g_borrowed_frame = FRAME_INVALID;
    bool restored = baseline_matches(fixture);

    report("BORROWED FRAME OWNER RELEASE / BASELINE", restored);
    return restored;

fail:
    /*
     * Preserve ordering: remove any userspace mapping before freeing its
     * backing test frame. If termination itself fails, leave the frame retained
     * for program_launch_test_cleanup_run().
     */
    if (program_instance_needs_cleanup(&g_program) && !program_terminate(&g_program)) return false;
    if (g_borrowed_frame != FRAME_INVALID) {
        if (!frame_free(g_borrowed_frame)) return false;

        g_borrowed_frame = FRAME_INVALID;
    }

    return false;
}

void program_launch_test_run(void) {
    terminal_writeln("PROGRAM LAUNCH TRANSACTION TEST:");

    if (!cleanup_fixture()) {
        report("NO RETAINED FIXTURE", false);
        return;
    }

    Process *kernel = process_kernel();
    Thread *main = thread_current();
    VfsNode *file = vfs_resolve(vfs_root(), PROGRAM_TEST_PATH);
    ProgramTestBaseline global;
    capture_baseline(&global);
    bool preflight = kernel && main && main->process == kernel && main->state == THREAD_STATE_RUNNING &&
        main->on_run_queue && scheduler_thread_count() == 1ULL && !scheduler_preemption_enabled() &&
        file && file->type == VFS_FILE && file->data && file->size &&
        !program_instance_needs_cleanup(&g_program) && !user_elf_test_faults_armed() &&
        supervisor_ok(global.supervisor_pid, global.supervisor_tid);
    report("GLOBAL BASELINE / PROGRAM FILE", preflight);
    if (!preflight) return;

    bool pass = false;
    if (!create_fixture()) goto done;

    ProgramTestBaseline fixture;
    capture_baseline(&fixture);
    ProgramLaunchSpec spec;
    make_spec(&spec, file);

    if (!program_rollback_test_run(&spec)) goto done;
    if (!borrowed_mapping_case(&spec, &fixture)) goto done;

    u64 launches_before = program_launch_count();
    ProgramLaunchSpec unauthorized;
    k_memcpy(&unauthorized, &spec, sizeof(unauthorized));
    unauthorized.startup_grants[0].authority_handle = CAPABILITY_INVALID_HANDLE;
    bool missing_authority = !program_launch(&g_program, &unauthorized) &&
        !program_instance_needs_cleanup(&g_program) && baseline_matches(&fixture) &&
        program_launch_count() == launches_before;

    ProgramLaunchSpec escalated;
    k_memcpy(&escalated, &spec, sizeof(escalated));
    escalated.startup_grants[0].rights = CAPABILITY_RIGHT_MANAGE;
    bool rights_escalation = !program_launch(&g_program, &escalated) &&
        !program_instance_needs_cleanup(&g_program) && baseline_matches(&fixture) &&
        program_launch_count() == launches_before;
    bool authority_rejected = missing_authority && rights_escalation;
    report("MISSING / ESCALATED AUTHORITY REJECTED", authority_rejected);
    if (!authority_rejected) goto done;

    bool armed = user_elf_test_fail_once(&g_program.image, USER_ELF_TEST_MAP, ADDRESS_SPACE_USER_BASE);
    bool rolled_back = armed && !program_launch(&g_program, &spec) &&
        !program_instance_needs_cleanup(&g_program) && !user_elf_test_faults_armed() && baseline_matches(&fixture);
    report("INJECTED ELF FAILURE / AUTO ROLLBACK", rolled_back);
    if (!rolled_back) goto done;

    bool launched = program_launch(&g_program, &spec);
    bool grants = launched && explicit_grants_valid() && scheduler_thread_count() == 2ULL;
    report("EXPLICIT GRANTS / FINAL PUBLICATION", grants);
    if (!grants) goto done;

    IpcMessage reply;
    bool ping = send_request(SUPERVISOR_MESSAGE_PING, 0x5245354C41554E43ULL, &reply) &&
        reply.word_count == 2U && reply.words[0] == SUPERVISOR_REPLY_PONG &&
        reply.words[1] == 0x5245354C41554E43ULL && g_program.thread.state == THREAD_STATE_BLOCKED &&
        endpoint_receiver_waiting(&g_command);
    report("GENERIC INSTANCE PING / BLOCK", ping);
    if (!ping) goto done;

    bool stopped = send_request(SUPERVISOR_MESSAGE_SHUTDOWN, 0, &reply) &&
        reply.word_count == 1U && reply.words[0] == SUPERVISOR_REPLY_STOPPED && wait_program_dead();
    report("NORMAL EXIT", stopped);
    if (!stopped) goto done;

    bool normal_reap = program_reap(&g_program) && !program_instance_needs_cleanup(&g_program) &&
        baseline_matches(&fixture);
    report("NORMAL REAP / FIXTURE BASELINE", normal_reap);
    if (!normal_reap) goto done;

    launched = program_launch(&g_program, &spec);
    u64 fault_thread_id = launched ? g_program.thread.id : 0;
    bool fault_setup = launched && patch_entry_ud2();
    interrupt_clear_user_fault();
    bool fault_ran = fault_setup && scheduler_yield() && g_program.thread.state == THREAD_STATE_DEAD &&
        !g_program.thread.on_run_queue;
    UserFaultInfo fault;
    k_memset(&fault, 0, sizeof(fault));
    bool fault_captured = fault_ran && interrupt_last_user_fault(&fault) && fault.valid &&
        fault.thread_id == fault_thread_id && fault.vector == 6ULL;
    bool fault_reap = fault_captured && program_reap(&g_program) &&
        !program_instance_needs_cleanup(&g_program) && baseline_matches(&fixture);
    report("FAULT EXIT CONTAINED", fault_captured);
    report("FAULT REAP / FIXTURE BASELINE", fault_reap);
    if (!fault_reap) goto done;

    pass = true;

done: {
        bool released = cleanup_fixture();
        bool baseline = released && baseline_matches(&global) &&
            supervisor_ok(global.supervisor_pid, global.supervisor_tid);
        report("FIXTURE CLEANUP / GLOBAL BASELINE", baseline);
        bool final = pass && baseline;
        terminal_set_color(final ? terminal_accent_color() : terminal_error_color());
        terminal_write("PROGRAM LAUNCH TRANSACTION TEST: ");
        terminal_writeln(final ? "PASS" : "FAILED");
        terminal_set_color(terminal_default_color());
        if (!released) terminal_writeln("PROGRAM TEST FIXTURE RETAINED. RUN test program-launch cleanup.");
    }
}
