#include "serial_worker.h"
#include "ipc.h"
#include "scheduler.h"
#include "timer.h"
#include "interrupts.h"
#include "lib.h"
#define REQUEST 0x53574F524B000001ULL
#define REPLY 0x53574F524B000002ULL
#define IF_BIT (1ULL<<9)
static u64 irq_save(void) {
    u64 flags;__asm__ volatile("pushfq; popq %0":"=r"(flags)::"memory");
    interrupts_disable();return flags;
}
static void irq_restore(u64 flags){if(flags&IF_BIT)interrupts_enable();}
bool serial_worker_present(const SerialWorker *w) {
    return w && (w->request_created || w->reply_created || w->thread_created ||
        w->request_send || w->request_receive || w->reply_send || w->reply_receive);
}
bool serial_worker_dead(const SerialWorker *w) {
    return w && w->thread_created && w->thread.state==THREAD_STATE_DEAD &&
        !w->thread.on_run_queue && !thread_wait_active(&w->thread);
}
static bool caller(const SerialWorker *w) {
    Thread *t=thread_current();
    return w && t && t->process==process_kernel() && t->id==w->owner_id &&
        t->state==THREAD_STATE_RUNNING && t->on_run_queue;
}
static void abort_work(SerialWorker *w) {
    w->poisoned=true;
    (void)w->dispatch(w->context,SERIAL_WORKER_ABORT,0);
}
static void worker_main(void *argument) {
    SerialWorker *w=argument;Process *kernel=process_kernel();
    if (!w || !kernel) scheduler_exit_current();
    for (;;) {
        if (w->poisoned || !timer_initialized() || timer_frequency()!=w->frequency ||
            !scheduler_preemption_enabled()) {abort_work(w);break;}
        u32 result=w->dispatch(w->context,SERIAL_WORKER_TICK,0);
        ++w->periodic_calls;
        if (result!=SERIAL_WORKER_OK) {abort_work(w);break;}
        if (w->poisoned) {abort_work(w);break;}
        u64 started=timer_ticks();
        IpcMessage q;k_memset(&q,0,sizeof(q));
        if (!ipc_receive_blocking_for(kernel,w->request_receive,&q,w->interval)) {
            if (!endpoint_closed(&w->request) && timer_frequency()==w->frequency &&
                timer_ticks()-started>=w->interval) continue;
            abort_work(w);break;
        }
        if (w->poisoned || q.word_count!=4 || q.words[0]!=REQUEST ||
            w->received==~0ULL || q.words[1]!=w->received+1 || !q.words[2] ||
            q.words[2]>=SERIAL_WORKER_ABORT) {abort_work(w);break;}
        w->received=q.words[1];
        result=w->dispatch(w->context,(u32)q.words[2],q.words[3]);
        if (result&~(SERIAL_WORKER_OK|SERIAL_WORKER_STOP)) {abort_work(w);break;}
        IpcMessage r;k_memset(&r,0,sizeof(r));r.word_count=4;
        r.words[0]=REPLY;r.words[1]=w->received;r.words[2]=result;r.words[3]=w->thread.id;
        if (!ipc_send_blocking_for(kernel,w->reply_send,&r,(u64)w->frequency*2)) {abort_work(w);break;}
        if (result&SERIAL_WORKER_STOP) break;
    }
    w->active=false;
    scheduler_exit_current();
}
bool serial_worker_reap(SerialWorker *w) {
    if (!w || !serial_worker_present(w)) return true;
    if (!caller(w) || w->calling || thread_current()==&w->thread) return false;
    u64 flags=irq_save();
    bool safe=!w->thread_created || serial_worker_dead(w) ||
        (!w->published && !w->thread.on_run_queue && !thread_wait_active(&w->thread));
    irq_restore(flags);
    if (!safe) return false;
    if (w->thread_created) {if(!thread_destroy(&w->thread))return false;w->thread_created=false;}
    if (w->request_created && !ipc_endpoint_close(&w->request)) return false;
    if (w->reply_created && !ipc_endpoint_close(&w->reply)) return false;
    CapabilityTable *caps=process_capabilities(process_kernel());if(!caps)return false;
    CapabilityHandle *handles[4]={&w->request_send,&w->request_receive,&w->reply_send,&w->reply_receive};
    for(u32 i=0;i<4;++i)if(*handles[i]) {
        if(!capability_revoke(caps,*handles[i]))return false;
        *handles[i]=0;
    }
    if(w->request_created){if(!endpoint_destroy(&w->request))return false;w->request_created=false;}
    if(w->reply_created){if(!endpoint_destroy(&w->reply))return false;w->reply_created=false;}
    k_memset(w,0,sizeof(*w));return true;
}
bool serial_worker_start(SerialWorker *w,SerialWorkerDispatch dispatch,void *context,u32 interval) {
    Thread *t=thread_current();Process *kernel=process_kernel();
    u64 flags=irq_save();irq_restore(flags);
    if(!w || serial_worker_present(w) || !dispatch || !interval || !timer_initialized() ||
        !timer_frequency() || !scheduler_preemption_enabled() || !(flags&IF_BIT) ||
        !t || t->process!=kernel || t->state!=THREAD_STATE_RUNNING || !t->on_run_queue) return false;
    k_memset(w,0,sizeof(*w));w->owner_id=t->id;w->dispatch=dispatch;w->context=context;
    w->frequency=timer_frequency();w->interval=interval;
    CapabilityTable *caps=process_capabilities(kernel);if(!caps)goto fail;
    if(!endpoint_create(&w->request))goto fail;w->request_created=true;
    if(!endpoint_create(&w->reply))goto fail;w->reply_created=true;
    if(!capability_insert(caps,&w->request,CAPABILITY_TYPE_ENDPOINT,CAPABILITY_RIGHT_SEND,&w->request_send) ||
       !capability_insert(caps,&w->request,CAPABILITY_TYPE_ENDPOINT,CAPABILITY_RIGHT_RECEIVE,&w->request_receive) ||
       !capability_insert(caps,&w->reply,CAPABILITY_TYPE_ENDPOINT,CAPABILITY_RIGHT_SEND,&w->reply_send) ||
       !capability_insert(caps,&w->reply,CAPABILITY_TYPE_ENDPOINT,CAPABILITY_RIGHT_RECEIVE,&w->reply_receive))goto fail;
    if(!thread_create(&w->thread,kernel))goto fail;w->thread_created=true;
    if(!thread_prepare_kernel(&w->thread,worker_main,w))goto fail;
    w->active=true;w->published=true;
    if(!scheduler_add(&w->thread)){w->published=false;w->active=false;goto fail;}
    return true;
fail:
    (void)serial_worker_reap(w);return false;
}
static bool wait_dead(SerialWorker *w) {
    u64 started=timer_ticks();
    for(;;) {
        u64 flags=irq_save();
        if(serial_worker_dead(w)){irq_restore(flags);return true;}
        bool yielded=scheduler_yield();irq_restore(flags);
        if(!yielded || timer_ticks()-started>=(u64)w->frequency*2) return false;
    }
}
bool serial_worker_call(SerialWorker *w,u32 operation,u64 argument,bool *out_ok) {
    if(!out_ok)return false;*out_ok=false;
    if(!caller(w) || !w->active || w->poisoned || !operation ||
        operation==SERIAL_WORKER_ABORT) return false;
    if(w->sequence==~0ULL){w->poisoned=true;return false;}
    u64 flags=irq_save();
    if(w->calling || !(flags&IF_BIT)){irq_restore(flags);return false;}
    w->calling=true;irq_restore(flags);
    IpcMessage q,r;k_memset(&q,0,sizeof(q));k_memset(&r,0,sizeof(r));
    q.word_count=4;q.words[0]=REQUEST;q.words[1]=++w->sequence;q.words[2]=operation;q.words[3]=argument;
    bool ok=timer_frequency()==w->frequency &&
        ipc_send_blocking_for(process_kernel(),w->request_send,&q,(u64)w->frequency*2) &&
        ipc_receive_blocking_for(process_kernel(),w->reply_receive,&r,(u64)w->frequency*30) &&
        r.word_count==4 && r.words[0]==REPLY && r.words[1]==w->sequence &&
        !(r.words[2]&~(u64)(SERIAL_WORKER_OK|SERIAL_WORKER_STOP)) && r.words[3]==w->thread.id;
    if(ok) {
        *out_ok=(r.words[2]&SERIAL_WORKER_OK)!=0;
        if(r.words[2]&SERIAL_WORKER_STOP)ok=wait_dead(w);
    }
    if(!ok)w->poisoned=true;
    w->calling=false;
    if(ok && (r.words[2]&SERIAL_WORKER_STOP))ok=serial_worker_reap(w);
    return ok;
}
