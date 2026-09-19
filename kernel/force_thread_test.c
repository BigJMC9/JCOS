#include "force_thread_test.h"

#include "capability.h"
#include "endpoint.h"
#include "interrupts.h"
#include "ipc.h"
#include "lib.h"
#include "pmm.h"
#include "pmm_test.h"
#include "process.h"
#include "scheduler.h"
#include "supervisor.h"
#include "task.h"
#include "terminal.h"
#include "thread.h"

#include "../include/runtime_cancel_test_abi.h"

#define FORCE_RFLAGS_IF (1ULL << 9)

typedef enum {
    FORCE_CASE_BLOCKED_RECEIVE = 1,
    FORCE_CASE_READY_RECEIVE,
    FORCE_CASE_BLOCKED_SEND,
    FORCE_CASE_READY_SEND
} ForceCase;

typedef enum {
    CUT_NONE, 
    CUT_ENDPOINT, 
    CUT_SEND_CAP, 
    CUT_RECEIVE_CAP,
    CUT_THREAD, 
    CUT_QUEUE, 
    CUT_BLOCKED, 
    CUT_READY
} SetupCut;

typedef enum {
    PAUSE_NONE, 
    PAUSE_BEFORE_STOP, 
    PAUSE_AFTER_STOP,
    PAUSE_AFTER_REAP, 
    PAUSE_AFTER_SEND_CAP, 
    PAUSE_BEFORE_ENDPOINT
} CleanupPause;

/* Every object reachable after publication has boot-lifetime storage. */
typedef struct {
    bool retained;
    bool endpoint_owned;
    bool worker_owned;
    bool unpublished_stack_pending;
    bool send_cap_owned;
    bool receive_cap_owned;
    bool cut_hit;
    bool pause_hit;
    volatile bool started;
    volatile bool returned;
    CleanupPause pause;
    ForceCase test_case;
    Endpoint endpoint;
    Thread worker;
    CapabilityHandle send_handle;
    CapabilityHandle receive_handle;
    Process *process;
    CapabilityTable *caps;
    Thread *main_thread;
    Thread *original_head;
    Thread *original_tail;
    u64 free_before;
    u64 process_threads_before;
    u32 thread_objects_before;
    CapabilityTable caps_before;
} ForceFixture;

static ForceFixture g_fixture;

static void print_test(const char *name, bool pass) {
    terminal_write("  "); terminal_write(name); terminal_write(": ");
    terminal_writeln(pass ? "PASS" : "FAILED");
}

static void print_count(const char *name, u64 count) {
    terminal_write("  "); terminal_write(name); terminal_write(": ");
    terminal_write_u64(count); terminal_putchar('\n');
}

static u64 interrupt_save(void) {
    u64 flags;
    __asm__ volatile ("pushfq\n\tpopq %0" : "=r"(flags) : : "memory");
    interrupts_disable();
    return flags;
}

static void interrupt_restore(u64 flags) {
    if (flags & FORCE_RFLAGS_IF) interrupts_enable();
}

static bool schedule_once(void) {
    u64 flags = interrupt_save();
    bool result = scheduler_yield();
    interrupt_restore(flags);
    return result;
}

static bool sender_case(ForceCase test_case) {
    return test_case == FORCE_CASE_BLOCKED_SEND || test_case == FORCE_CASE_READY_SEND;
}

static bool ready_case(ForceCase test_case) {
    return test_case == FORCE_CASE_READY_RECEIVE || test_case == FORCE_CASE_READY_SEND;
}

static const char *case_name(ForceCase test_case) {
    /* Character storage, not a relocated table of string pointers. */
    static const char names[4][24] = {
        " BLOCKED RECEIVE:", 
        " READY RECEIVE:", 
        " BLOCKED SEND:", 
        " COMMITTED SEND:"
    };
    if (test_case < FORCE_CASE_BLOCKED_RECEIVE || test_case > FORCE_CASE_READY_SEND) return " INVALID CASE:";
    return names[(u32)test_case - 1U];
}

static void fill_message(IpcMessage *message, bool prefill) {
    k_memset(message, 0, sizeof(*message));
    message->word_count = IPC_MESSAGE_MAX_WORDS;
    message->words[0] = prefill ? JCOS_RTC_PREFILL_WORD0 : JCOS_RTC_SEND_WORD0;
    message->words[1] = prefill ? JCOS_RTC_PREFILL_WORD1 : JCOS_RTC_SEND_WORD1;
    message->words[2] = prefill ? JCOS_RTC_PREFILL_WORD2 : JCOS_RTC_SEND_WORD2;
    message->words[3] = prefill ? JCOS_RTC_PREFILL_WORD3 : JCOS_RTC_SEND_WORD3;
}

static bool message_matches(const IpcMessage *message, bool prefill) {
    IpcMessage expected;
    fill_message(&expected, prefill);
    if (!message || message->word_count != expected.word_count) return false;
    for (u32 i = 0; i < IPC_MESSAGE_MAX_WORDS; ++i) {
        if (message->words[i] != expected.words[i]) return false;
    }
    return true;
}

static bool endpoint_has_waits(void) {
    return g_fixture.endpoint.waiting_receiver || g_fixture.endpoint.waiting_sender ||
        g_fixture.endpoint.waiting_sender_message_ready;
}

static bool caps_unchanged(void) {
    ForceFixture *f = &g_fixture;
    if (!f->caps || capability_table_count(f->caps) != f->caps_before.count) return false;
    for (u32 i = 0; i < CAPABILITY_TABLE_CAPACITY; ++i) {
        const CapabilitySlot *old = &f->caps_before.slots[i];
        const CapabilitySlot *now = &f->caps->slots[i];
        if (old->occupied != now->occupied) return false;
        /* Generations of formerly empty, reused test slots may advance. */
        if (old->occupied && (old->object != now->object || old->generation != now->generation ||
            old->type != now->type || old->rights != now->rights)) return false;
    }
    return true;
}

static bool baseline_restored(void) {
    ForceFixture *f = &g_fixture;
    return caps_unchanged() && pmm_stats().free_pages == f->free_before &&
        process_thread_count(f->process) == f->process_threads_before &&
        f->process->thread_head == f->original_head && f->process->thread_tail == f->original_tail &&
        thread_object_count() == f->thread_objects_before &&
        thread_current() == f->main_thread && f->main_thread->state == THREAD_STATE_RUNNING &&
        f->main_thread->on_run_queue && scheduler_thread_count() == 1ULL;
}

static bool fixture_open(ForceCase test_case) {
    /* Do not even inspect/reinitialize the retained object's interior. */
    if (g_fixture.retained) return false;
    if (pmm_test_free_failure_armed() || thread_creation_cleanup_pending()) return false;
    Process *process = process_kernel();
    Thread *main_thread = thread_current();
    CapabilityTable *caps = process ? process_capabilities(process) : 0;
    if (!process || !caps || !main_thread || main_thread->process != process ||
        main_thread->state != THREAD_STATE_RUNNING || !main_thread->on_run_queue ||
        scheduler_thread_count() != 1ULL || scheduler_preemption_enabled()) return false;
    if (test_case < FORCE_CASE_BLOCKED_RECEIVE || test_case > FORCE_CASE_READY_SEND) return false;
    k_memset(&g_fixture, 0, sizeof(g_fixture));
    g_fixture.retained = true;
    g_fixture.test_case = test_case;
    g_fixture.process = process;
    g_fixture.caps = caps;
    g_fixture.main_thread = main_thread;
    g_fixture.original_head = process->thread_head;
    g_fixture.original_tail = process->thread_tail;
    g_fixture.process_threads_before = process_thread_count(process);
    g_fixture.thread_objects_before = thread_object_count();
    g_fixture.free_before = pmm_stats().free_pages;
    k_memcpy(&g_fixture.caps_before, caps, sizeof(*caps));
    return true;
}

static bool hit_cut(SetupCut requested, SetupCut here) {
    if (requested != here) return false;
    g_fixture.cut_hit = true;
    return true;
}

static bool hit_pause(CleanupPause here) {
    if (g_fixture.pause != here) return false;
    g_fixture.pause = PAUSE_NONE;
    g_fixture.pause_hit = true;
    return true;
}

static void receiver_thread(void *argument) {
    ForceFixture *f = argument;
    f->started = true;
    IpcMessage message;
    k_memset(&message, 0, sizeof(message));
    (void)ipc_receive_blocking(f->process, f->receive_handle, &message);
    f->returned = true;
    scheduler_exit_current();
}

static void sender_thread(void *argument) {
    ForceFixture *f = argument;
    f->started = true;
    IpcMessage message;
    fill_message(&message, false);
    (void)ipc_send_blocking(f->process, f->send_handle, &message);
    f->returned = true;
    scheduler_exit_current();
}

static bool fixture_prepare(SetupCut cut) {
    ForceFixture *f = &g_fixture;
    f->endpoint_owned = endpoint_create(&f->endpoint);
    if (!f->endpoint_owned || hit_cut(cut, CUT_ENDPOINT)) return false;
    f->send_cap_owned = capability_insert(f->caps, &f->endpoint, CAPABILITY_TYPE_ENDPOINT,
        CAPABILITY_RIGHT_SEND, &f->send_handle);
    if (!f->send_cap_owned || hit_cut(cut, CUT_SEND_CAP)) return false;
    f->receive_cap_owned = capability_insert(f->caps, &f->endpoint, CAPABILITY_TYPE_ENDPOINT,
        CAPABILITY_RIGHT_RECEIVE, &f->receive_handle);
    if (!f->receive_cap_owned || hit_cut(cut, CUT_RECEIVE_CAP)) return false;
    if (sender_case(f->test_case)) {
        IpcMessage prefill;
        fill_message(&prefill, true);
        if (!ipc_try_send(f->process, f->send_handle, &prefill)) return false;
    }
    f->worker_owned = thread_create(&f->worker, f->process);
    if (!f->worker_owned) {
        /* Entry required the module-owned constructor cleanup slot to be empty. */
        f->unpublished_stack_pending = thread_creation_cleanup_pending();
        return false;
    }
    if (hit_cut(cut, CUT_THREAD)) return false;
    if (!thread_prepare_kernel(&f->worker,
            sender_case(f->test_case) ? sender_thread : receiver_thread, f)) return false;
    if (!scheduler_add(&f->worker) || hit_cut(cut, CUT_QUEUE)) return false;
    if (!schedule_once()) return false;
    ThreadWaitKind kind = sender_case(f->test_case) ? THREAD_WAIT_IPC_SEND : THREAD_WAIT_IPC_RECEIVE;
    Thread *waiter = sender_case(f->test_case) ? f->endpoint.waiting_sender : f->endpoint.waiting_receiver;
    bool blocked = f->started && !f->returned && thread_current() == f->main_thread &&
        f->worker.state == THREAD_STATE_BLOCKED && !f->worker.on_run_queue &&
        f->worker.interrupt_context_ready && f->worker.interrupt_rsp && waiter == &f->worker &&
        thread_wait_matches(&f->worker, kind, &f->endpoint) && scheduler_thread_count() == 1ULL;
    if (!blocked || hit_cut(cut, CUT_BLOCKED)) return false;
    if (ready_case(f->test_case)) {
        IpcMessage message;
        if (sender_case(f->test_case)) {
            if (!ipc_try_receive(f->process, f->receive_handle, &message) ||
                !message_matches(&message, true)) return false;
        } else {
            fill_message(&message, false);
            if (!ipc_try_send(f->process, f->send_handle, &message)) return false;
        }
        if (f->worker.state != THREAD_STATE_READY || !f->worker.on_run_queue ||
            !thread_wait_matches(&f->worker, kind, &f->endpoint) ||
            scheduler_thread_count() != 2ULL) return false;
        if (hit_cut(cut, CUT_READY)) return false;
    }
    return true;
}

static bool fixture_stop_worker(void) {
    ForceFixture *f = &g_fixture;
    if (!f->worker_owned) return true;
    if (&f->worker == thread_current()) return false;
    if (f->worker.state != THREAD_STATE_DEAD && !task_terminate_thread(&f->worker)) return false;
    return f->worker.state == THREAD_STATE_DEAD && !f->worker.on_run_queue &&
        !f->worker.run_next && !f->worker.interrupt_context_ready && !f->worker.interrupt_rsp &&
        !thread_wait_active(&f->worker) && !endpoint_has_waits();
}

static bool fixture_cleanup_locked(void) {
    ForceFixture *f = &g_fixture;
    if (!f->retained) return true;
    if (thread_current() != f->main_thread || scheduler_preemption_enabled()) return false;
    if (f->worker_owned) {
        if (hit_pause(PAUSE_BEFORE_STOP)) return false;
        if (!fixture_stop_worker()) return false;
        if (hit_pause(PAUSE_AFTER_STOP)) return false;
        /* No subsequent resource is touched after a rejected reap. */
        if (!thread_destroy(&f->worker)) return false;
        f->worker_owned = false;
        if (hit_pause(PAUSE_AFTER_REAP)) return false;
    }
    if (f->unpublished_stack_pending) {
        if (!thread_reclaim_unpublished_stack()) return false;
        f->unpublished_stack_pending = false;
    }
    if (f->endpoint_owned) {
        if (endpoint_has_waits()) return false;
        if (endpoint_message_ready(&f->endpoint)) {
            IpcMessage discard;
            if (!endpoint_try_receive(&f->endpoint, &discard)) return false;
            if (endpoint_message_ready(&f->endpoint)) return false;
        }
    }
    if (f->send_cap_owned) {
        if (!capability_revoke(f->caps, f->send_handle)) return false;
        f->send_cap_owned = false;
        if (hit_pause(PAUSE_AFTER_SEND_CAP)) return false;
    }
    if (f->receive_cap_owned) {
        if (!capability_revoke(f->caps, f->receive_handle)) return false;
        f->receive_cap_owned = false;
    }
    if (f->endpoint_owned) {
        if (hit_pause(PAUSE_BEFORE_ENDPOINT)) return false;
        if (!endpoint_destroy(&f->endpoint)) return false;
        f->endpoint_owned = false;
    }
    if (!baseline_restored()) return false;
    f->retained = false;
    return true;
}

static bool fixture_cleanup(void) {
    u64 flags = interrupt_save();
    bool result = fixture_cleanup_locked();
    interrupt_restore(flags);
    return result;
}

static bool check_message_semantics(void) {
    ForceFixture *f = &g_fixture;
    if (endpoint_has_waits()) return false;
    if (f->test_case == FORCE_CASE_BLOCKED_RECEIVE) return !endpoint_message_ready(&f->endpoint);
    IpcMessage message;
    if (!ipc_try_receive(f->process, f->receive_handle, &message)) return false;
    if (!message_matches(&message, f->test_case == FORCE_CASE_BLOCKED_SEND)) return false;
    return !ipc_try_receive(f->process, f->receive_handle, &message) &&
        !endpoint_message_ready(&f->endpoint);
}

static void retained_notice(void) {
    if (!g_fixture.retained) return;
    terminal_writeln("  FIXTURE RETAINED; DO NOT RUN OTHER TESTS YET.");
    terminal_writeln("  RUN forcecleanupretry. IF IT FAILS, REBOOT.");
}

static void finish_report(const char *name, bool pass, u64 before) {
    print_count("FREE BEFORE", before);
    print_count("FREE AFTER", pmm_stats().free_pages);
    terminal_set_color(pass ? terminal_accent_color() : terminal_error_color());
    terminal_write(name); terminal_writeln(pass ? ": PASS" : ": FAILED");
    terminal_set_color(terminal_default_color());
    retained_notice();
}

static bool run_case(ForceCase test_case) {
    terminal_writeln(case_name(test_case));
    if (!fixture_open(test_case)) return false;
    bool prepared = fixture_prepare(CUT_NONE);
    print_test("BLOCKED / RESERVATION PREPARED", prepared);
    bool stopped = prepared && fixture_stop_worker();
    print_test("FORCED TERMINATION", stopped);
    print_test("CONTINUATION NOT RESUMED", !g_fixture.returned);
    bool semantics = stopped && !g_fixture.returned && check_message_semantics();
    print_test("MESSAGE SEMANTICS", semantics);
    /* Cleanup must run even when the semantic assertion failed. */
    bool clean = fixture_cleanup();
    print_test("THREAD REAP", !g_fixture.worker_owned);
    print_test("CAPS RESTORED", caps_unchanged());
    print_test("ENDPOINT DESTROY", !g_fixture.endpoint_owned);
    print_test("FRAME COUNT RESTORED", pmm_stats().free_pages == g_fixture.free_before);
    return prepared && stopped && semantics && clean;
}

void force_thread_test_run(void) {
    terminal_writeln("FORCED THREAD TERMINATION TEST:");
    u64 before = pmm_stats().free_pages;
    bool pass = !g_fixture.retained;
    for (u32 i = FORCE_CASE_BLOCKED_RECEIVE; pass && i <= FORCE_CASE_READY_SEND; ++i) {
        pass = run_case((ForceCase)i);
    }
    finish_report("FORCED THREAD TERMINATION TEST", pass && pmm_stats().free_pages == before, before);
}

static bool fixture_caps_valid(void) {
    ForceFixture *f = &g_fixture;
    void *object = 0;
    if (f->send_cap_owned && (!capability_lookup(f->caps, f->send_handle,
            CAPABILITY_TYPE_ENDPOINT, &object) || object != &f->endpoint)) return false;
    if (f->receive_cap_owned && (!capability_lookup(f->caps, f->receive_handle,
            CAPABILITY_TYPE_ENDPOINT, &object) || object != &f->endpoint)) return false;
    return true;
}

static bool retained_reuse_rejected(void) {
    u64 id = g_fixture.worker.id;
    CapabilityHandle handle = g_fixture.send_handle;
    bool owned = g_fixture.worker_owned;
    bool rejected = !fixture_open(FORCE_CASE_BLOCKED_RECEIVE);
    return rejected && g_fixture.retained && g_fixture.worker.id == id &&
        g_fixture.send_handle == handle && g_fixture.worker_owned == owned && fixture_caps_valid();
}

/* This function returns after cleanup fails; all referenced storage must survive it. */
static bool return_after_reap_failure(ForceCase test_case) {
    if (!fixture_open(test_case)) return false;
    if (!fixture_prepare(CUT_NONE)) { (void)fixture_cleanup(); return false; }
    frame_t first = phys_to_frame(g_fixture.worker.kernel_stack_physical);
    u64 count = g_fixture.worker.kernel_stack_size / FRAME_SIZE;
    if (!pmm_test_fail_free_range_once(first, count)) { (void)fixture_cleanup(); return false; }
    bool clean = fixture_cleanup();
    return !clean && !pmm_test_free_failure_armed();
}

static bool injected_reap_case(ForceCase test_case) {
    terminal_writeln(case_name(test_case));
    if (!return_after_reap_failure(test_case)) return false;
    ForceFixture *f = &g_fixture;
    bool retained = f->retained && f->worker_owned && f->endpoint_owned &&
        f->send_cap_owned && f->receive_cap_owned && f->worker.id &&
        f->worker.state == THREAD_STATE_DEAD && !f->worker.on_run_queue &&
        !thread_wait_active(&f->worker) && !endpoint_has_waits() &&
        process_thread_contains(f->process, &f->worker) &&
        process_thread_count(f->process) == f->process_threads_before + 1ULL &&
        thread_object_count() == f->thread_objects_before + 1U &&
        fixture_caps_valid() && !f->returned;
    frame_t first = phys_to_frame(f->worker.kernel_stack_physical);
    u64 count = f->worker.kernel_stack_size / FRAME_SIZE;
    for (u64 i = 0; retained && i < count; ++i) retained = pmm_test_frame_releasable(first + i);
    retained = retained && count == THREAD_KERNEL_STACK_PAGES &&
        pmm_stats().free_pages == f->free_before - count;
    print_test("FAILED REAP RETURNED WITH DURABLE OWNERSHIP", retained);
    bool mailbox = retained && (test_case == FORCE_CASE_BLOCKED_RECEIVE
        ? !endpoint_message_ready(&f->endpoint)
        : endpoint_message_ready(&f->endpoint) &&
          message_matches(&f->endpoint.message, test_case == FORCE_CASE_BLOCKED_SEND));
    print_test("STOPPED MESSAGE STATE RETAINED", mailbox);
    bool guarded = mailbox && retained_reuse_rejected();
    print_test("RETAINED FIXTURE NOT OVERWRITTEN", guarded);
    bool retry = guarded && fixture_cleanup();
    print_test("REAP RETRY / DEPENDENCY CLEANUP", retry);
    bool idempotent = retry && fixture_cleanup() && baseline_restored();
    print_test("REPEATED CLEANUP / BASELINES", idempotent);
    return retained && guarded && retry && idempotent;
}

static bool setup_failure_case(SetupCut cut) {
    /* READY receive visits every setup cut without staging a SEND. */
    if (!fixture_open(FORCE_CASE_READY_RECEIVE)) return false;
    bool prepared = fixture_prepare(cut);
    bool expected = !prepared && g_fixture.cut_hit;
    bool clean = fixture_cleanup();
    return expected && clean && !g_fixture.returned && baseline_restored();
}

static bool partial_cleanup_case(CleanupPause pause) {
    if (!fixture_open(FORCE_CASE_READY_RECEIVE)) return false;
    if (!fixture_prepare(CUT_NONE)) { (void)fixture_cleanup(); return false; }
    g_fixture.pause = pause;
    bool first = fixture_cleanup();
    ForceFixture *f = &g_fixture;
    bool state = !first && f->retained && f->pause_hit && !f->returned && fixture_caps_valid();
    if (pause == PAUSE_BEFORE_STOP) {
        state = state && f->worker_owned && f->worker.state == THREAD_STATE_READY &&
            f->worker.on_run_queue && f->endpoint.waiting_receiver == &f->worker &&
            thread_wait_active(&f->worker);
    } else if (pause == PAUSE_AFTER_STOP) {
        state = state && f->worker_owned && f->worker.state == THREAD_STATE_DEAD &&
            !f->worker.on_run_queue && !thread_wait_active(&f->worker) && !endpoint_has_waits();
    } else {
        state = state && !f->worker_owned && process_thread_count(f->process) == f->process_threads_before;
    }
    if (pause == PAUSE_AFTER_SEND_CAP) state = state && !f->send_cap_owned && f->receive_cap_owned;
    if (pause == PAUSE_BEFORE_ENDPOINT) state = state && !f->send_cap_owned && !f->receive_cap_owned && f->endpoint_owned;
    if (!state || !retained_reuse_rejected()) return false;
    return fixture_cleanup() && fixture_cleanup() && baseline_restored();
}

void published_cleanup_test_run(void) {
    terminal_writeln("PUBLISHED FIXTURE CLEANUP TEST:");
    u64 before = pmm_stats().free_pages;
    bool pass = !g_fixture.retained && !pmm_test_free_failure_armed() &&
        !thread_creation_cleanup_pending() && !scheduler_preemption_enabled();
    print_test("NO RETAINED FIXTURE / FAULT HOOK IDLE", pass);
    u64 pid = supervisor_process_id();
    u64 tid = supervisor_thread_id();
    u64 reply = 0;
    pass = pass && supervisor_ping(0x505542434C45414EULL, &reply) && reply == 0x505542434C45414EULL;
    print_test("SUPERVISOR BEFORE", pass);
    for (u32 i = CUT_ENDPOINT; pass && i <= CUT_READY; ++i) {
        pass = setup_failure_case((SetupCut)i);
        terminal_write("  SETUP ABORT "); terminal_write_u64(i);
        terminal_writeln(pass ? ": PASS" : ": FAILED");
    }
    for (u32 i = FORCE_CASE_BLOCKED_RECEIVE; pass && i <= FORCE_CASE_READY_SEND; ++i) {
        pass = injected_reap_case((ForceCase)i);
    }
    for (u32 i = PAUSE_BEFORE_STOP; pass && i <= PAUSE_BEFORE_ENDPOINT; ++i) {
        pass = partial_cleanup_case((CleanupPause)i);
        terminal_write("  PARTIAL CLEANUP "); terminal_write_u64(i);
        terminal_writeln(pass ? ": PASS" : ": FAILED");
    }
    /* Do not yield into other services while a failed fixture remains live. */
    bool supervisor_ok = !g_fixture.retained && supervisor_ping(0x505542434C45414FULL, &reply) &&
        reply == 0x505542434C45414FULL && supervisor_process_id() == pid && supervisor_thread_id() == tid;
    print_test("SUPERVISOR AFTER / IDENTITY", supervisor_ok);
    bool persistent = pass && supervisor_ok && baseline_restored();
    print_test("PERSISTENT BASELINES", persistent);
    finish_report("PUBLISHED FIXTURE CLEANUP TEST", persistent &&
        pmm_stats().free_pages == before, before);
}

void force_thread_cleanup_run(void) {
    terminal_writeln("FORCED TEST CLEANUP RETRY:");
    if (!g_fixture.retained) {
        terminal_writeln("  NO RETAINED FIXTURE: PASS");
        return;
    }
    /* Ignore test-only pause requests; do not erase actual ownership or PMM faults. */
    g_fixture.pause = PAUSE_NONE;
    bool clean = fixture_cleanup();
    print_test("RETAINED FIXTURE RECLAIMED", clean);
    retained_notice();
}
