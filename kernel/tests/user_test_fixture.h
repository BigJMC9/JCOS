#ifndef JA_OS_USER_TEST_FIXTURE_H
#define JA_OS_USER_TEST_FIXTURE_H

#include "address_space.h"
#include "capability.h"
#include "endpoint.h"
#include "process.h"
#include "thread.h"
#include "user_elf.h"

/* Diagnostic owner only. One canonical fixture shared by the migrated tests. */
#define USER_FIXTURE_THREADS 2U
#define USER_FIXTURE_ENDPOINTS 2U
#define USER_FIXTURE_STACKS 2U
#define USER_FIXTURE_GRANTS 8U

typedef struct {
    u64 virtual_address;
    frame_t frame;
    bool allocated;
    bool mapped;
} UserTestStack;

typedef struct {
    CapabilityHandle handle;
    CapabilityRights rights;
    u32 endpoint;
    bool kernel;
    bool owned;
} UserTestGrant;

typedef enum {
    USER_FIXTURE_PAUSE_NONE = 0,
    USER_FIXTURE_AFTER_QUIESCE,
    USER_FIXTURE_AFTER_STACK_UNMAP,
    USER_FIXTURE_AFTER_STACK_FREE,
    USER_FIXTURE_AFTER_ELF,
    USER_FIXTURE_AFTER_KERNEL_CAP,
    USER_FIXTURE_BEFORE_ENDPOINT,
    USER_FIXTURE_BEFORE_PROCESS,
    USER_FIXTURE_BEFORE_RELEASE
} UserFixturePause;

typedef struct {
    Process process;
    UserElfImage image;
    Thread threads[USER_FIXTURE_THREADS];
    Endpoint endpoints[USER_FIXTURE_ENDPOINTS];
    UserTestStack stacks[USER_FIXTURE_STACKS];
    UserTestGrant grants[USER_FIXTURE_GRANTS];
    bool process_created;
    bool thread_created[USER_FIXTURE_THREADS];
    bool endpoint_created[USER_FIXTURE_ENDPOINTS];
    bool unpublished_stack;
    bool unpublished_space;
    bool unlinked_table;
    bool active;
    bool cleanup_started;
    bool quiesced;
    bool checks_ok;
    bool quiet;
    bool last_cleanup_ok;
    bool suppress_expected_retention_report; /* diagnostic-only, consumed by finish */
    u32 cleanup_stage;
    u32 cleanup_index;
    UserFixturePause pause;
    u32 pause_index;
    char label[64];

    Thread *main_thread;
    Process *kernel_process;
    CapabilityTable *kernel_caps;
    CapabilitySlot kernel_slots[CAPABILITY_TABLE_CAPACITY];
    u32 kernel_cap_count;
    u32 process_count;
    u32 thread_count;
    u32 space_count;
    u64 kernel_thread_count;
    Thread *kernel_head;
    Thread *kernel_tail;
    u64 free_pages;
    u64 supervisor_pid;
    u64 supervisor_tid;
    bool preemption_enabled;
} UserTestFixture;

/* Returns false before changing anything when another migrated test is retained. */
bool user_fixture_available(void);
bool user_fixture_busy(void);
UserTestFixture *user_fixture_begin(const char *label);
void user_fixture_set_quiet(UserTestFixture *fixture, bool quiet);
void user_fixture_check(const char *name, bool pass);

bool user_fixture_process_create(UserTestFixture *fixture);
bool user_fixture_endpoint_create(UserTestFixture *fixture, u32 index);
bool user_fixture_grant(UserTestFixture *fixture, bool kernel, u32 endpoint,
    CapabilityRights rights, CapabilityHandle *handle);
bool user_fixture_load(UserTestFixture *fixture, const VfsNode *file);
bool user_fixture_stack_allocate(UserTestFixture *fixture, u32 index, u64 address);
bool user_fixture_stack_map(UserTestFixture *fixture, u32 index);
bool user_fixture_stack_create(UserTestFixture *fixture, u32 index, u64 address, u64 arg0, u64 arg1);
u64 user_fixture_stack_rsp(const UserTestFixture *fixture, u32 index);
bool user_fixture_thread_create(UserTestFixture *fixture, u32 index);
bool user_fixture_schedule_once(void);

/* Quiesce is also used by the existing process-kill assertions, before cleanup. */
bool user_fixture_quiesce(UserTestFixture *fixture);
/* false keeps the canonical fixture and all outstanding ownership alive. */
bool user_fixture_cleanup(UserTestFixture *fixture);
bool user_fixture_baselines(const UserTestFixture *fixture);
bool user_fixture_supervisor_ok(const UserTestFixture *fixture);
bool user_fixture_finish(UserTestFixture *fixture, bool behavior_completed);
bool user_fixture_last_result(void);
void user_fixture_cleanup_retry_run(void);

/* Test-only, one-shot return after a successful cleanup step or before a release. */
bool user_fixture_pause_once(UserTestFixture *fixture, UserFixturePause point, u32 index);
bool user_fixture_hooks_idle(void);

#endif
