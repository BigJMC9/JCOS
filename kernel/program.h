#ifndef JA_OS_PROGRAM_H
#define JA_OS_PROGRAM_H

#include "capability.h"
#include "process.h"
#include "thread.h"
#include "user_elf.h"
#include "vfs.h"
#include "../include/program_startup.h"

struct ProcessExitQueue;

typedef struct {
    const CapabilityTable *authority_table;
    CapabilityHandle authority_handle;
    void *object;
    CapabilityType type;
    CapabilityRights rights;
} ProgramGrantSpec;

typedef struct {
    const VfsNode *file;
    /* Optional terminal observer. Registration commits inside launch after all
     * other fallible setup and before run-queue publication. */
    struct ProcessExitQueue *exit_queue;
    ProgramGrantSpec startup_grants[JCOS_PROGRAM_STARTUP_MAX_CAPABILITIES];
    u32 startup_grant_count;
    u64 startup_arguments[JCOS_PROGRAM_STARTUP_MAX_ARGUMENTS];
    u32 startup_argument_count;
} ProgramLaunchSpec;

typedef struct {
    Process process;
    Thread thread;
    UserElfImage image;
    frame_t stack_frame;
    CapabilityHandle startup_handles[JCOS_PROGRAM_STARTUP_MAX_CAPABILITIES];
    bool process_created;
    bool stack_frame_allocated;
    bool stack_mapped;
    bool thread_created;
    bool published;
    bool unpublished_space;
    bool unpublished_stack;
    bool unlinked_table;
} ProgramInstance;

/*
 * Kernel-internal R5 launch transaction. There is intentionally no userspace
 * syscall for manufacturing a Process, AddressSpace, Thread, or capability
 * grant. Path lookup and launch policy stay outside this module. Each child
 * capability is attenuated from an existing source handle: the source must
 * carry TRANSFER plus every delegated right, resolve to the same object/type,
 * and remain valid until program_launch returns. Raw object pointers alone are
 * never sufficient launch authority.
 *
 * The initial user RSP points at a versioned JcosProgramStartup block. v1 has
 * ordered explicit capability handles plus opaque 64-bit argument words.
 * argv/string environment and exit status are not part of v1; environment_count
 * is always zero. Unsupported ELF/runtime requirements remain rejected by the
 * existing user_elf loader.
 *
 * An optional exit_queue reserves one durable terminal-event slot before the
 * new thread becomes runnable; failed/unpublished launches remove that watch
 * and never masquerade as a service exit.
 *
 * Successful launch publishes exactly one initial READY thread as the final
 * commit. false may retain owned rollback state when a lower-level cleanup
 * operation itself fails; retry with program_terminate(). Module-owned scratch
 * allocations are part of that retry obligation, even without a live Process.
 * Zero-initialize storage before first use; do not move/copy/reuse an instance
 * until needs_cleanup() is false. Retry its scratch obligations through this
 * API, not by independently reclaiming/reusing the module quarantine slots.
 *
 * Launch/release are serialized by local IRQ exclusion in the UP profile and
 * never wait for a service. Pre-existing constructor quarantine blocks a new
 * launch without adopting another caller's resources. SMP/NMI use is unsupported.
 */
bool program_launch(ProgramInstance *instance, const ProgramLaunchSpec *spec);

/*
 * Reap only after every thread in the owned Process is already DEAD. This does
 * not turn a live program into a dead one. It destroys threads, revokes the
 * child capability table, releases the guarded stack and ELF image, and then
 * destroys the Process.
 */
bool program_reap(ProgramInstance *instance);

/*
 * Force-quiesce all owned threads using the R2 task/IPC cancellation contract,
 * then release the same resources as reap. This is also the retry path for an
 * unpublished launch whose rollback was retained.
 */
bool program_terminate(ProgramInstance *instance);

bool program_instance_needs_cleanup(const ProgramInstance *instance);

/* Monotonic diagnostic/publication counter; increments only after scheduler_add commits. */
u64 program_launch_count(void);

#endif
