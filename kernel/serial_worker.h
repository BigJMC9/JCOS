#ifndef JA_OS_SERIAL_WORKER_H
#define JA_OS_SERIAL_WORKER_H
#include "thread.h"
#include "endpoint.h"
#include "capability.h"
#define SERIAL_WORKER_TICK 0U
#define SERIAL_WORKER_ABORT 0xFFFFFFFFU
#define SERIAL_WORKER_OK 1U
#define SERIAL_WORKER_STOP 2U
typedef u32 (*SerialWorkerDispatch)(void *context,u32 operation,u64 argument);
/* One privileged control thread,one consumer. Never force-terminate a trusted
 * callback or free a stack while its continuation can retain device resources. */
typedef struct {
    Endpoint request,reply;
    Thread thread;
    CapabilityHandle request_send,request_receive,reply_send,reply_receive;
    SerialWorkerDispatch dispatch;
    void *context;
    u64 owner_id,sequence,received,periodic_calls;
    u32 frequency,interval;
    bool request_created,reply_created,thread_created,published,active,calling;
    volatile bool poisoned;
} SerialWorker;
bool serial_worker_start(SerialWorker *worker,SerialWorkerDispatch dispatch,void *context,u32 interval_ticks);
bool serial_worker_call(SerialWorker *worker,u32 operation,u64 argument,bool *out_ok);
bool serial_worker_reap(SerialWorker *worker);
bool serial_worker_present(const SerialWorker *worker);
bool serial_worker_dead(const SerialWorker *worker);
#endif
