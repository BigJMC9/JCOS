#ifndef JA_OS_PROCESS_EXIT_QUEUE_H
#define JA_OS_PROCESS_EXIT_QUEUE_H

#include "process.h"
#include "thread.h"

#define PROCESS_EXIT_QUEUE_CAPACITY 16U
#define PROCESS_EXIT_QUEUE_STORAGE_CAPACITY 16U

typedef struct {
    Process *process;
    u64 process_id;
    ProcessExitInfo event;
    bool watching;
    bool pending;
} ProcessExitQueueSlot;

typedef struct ProcessExitQueue {
    u64 id;
    ProcessExitQueueSlot slots[PROCESS_EXIT_QUEUE_CAPACITY];
    Thread *waiting_receiver;
    u64 waiting_receiver_id;
    u8 pending_order[PROCESS_EXIT_QUEUE_CAPACITY];
    u32 pending_head;
    u32 pending_tail;
    u32 watch_count;
    u32 pending_count;
    bool closed;
    bool initialized;
} ProcessExitQueue;

bool process_exit_queue_create(ProcessExitQueue *queue);
bool process_exit_queue_close(ProcessExitQueue *queue);
bool process_exit_queue_destroy(ProcessExitQueue *queue);
bool process_exit_queue_storage_in_use(const ProcessExitQueue *queue);
u32 process_exit_queue_object_count(void);

/* Kernel policy registers a Process before publication. The reserved slot is
 * the event's storage guarantee: publication cannot lose an event merely
 * because its recovery authority is asleep. One Process may have one watcher. */
bool process_exit_queue_watch(ProcessExitQueue *queue, Process *process);
bool process_exit_queue_unwatch_process(Process *process);
bool process_exit_queue_publish_process(Process *process, const ProcessExitInfo *info);

/* R6a.2 kernel recovery boundary. Userspace receives no raw queue pointer.
 * receive_blocking is currently restricted to the kernel Process; a later
 * capability-backed service ABI can reuse this object without weakening its
 * lifetime rules. try_receive may be used by kernel policy while IRQ-safe. */
bool process_exit_queue_try_receive(ProcessExitQueue *queue, ProcessExitInfo *out);
bool process_exit_queue_receive_blocking(ProcessExitQueue *queue, ProcessExitInfo *out);
bool process_exit_queue_abort_wait(Thread *thread);

u32 process_exit_queue_watch_count(const ProcessExitQueue *queue);
u32 process_exit_queue_pending_count(const ProcessExitQueue *queue);

#endif
