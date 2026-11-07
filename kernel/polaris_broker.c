#include "polaris_driver.h"
#include "polaris_program_gate.h"
#include "ipc.h"
#include "timer.h"
#include "scheduler.h"
#include "interrupts.h"
#include "device_mmio.h"
#include "lib.h"
typedef struct {PolarisDriver *driver;u64 started;u32 frequency;} Conversation;
static bool current(const Conversation *c) {
    return c->frequency && timer_initialized() && timer_frequency()==c->frequency &&
        timer_ticks()-c->started<(u64)c->frequency*2 &&
        managed_service_connection_current(&c->driver->service,&c->driver->connection) &&
        managed_service_process_id(&c->driver->service)==c->driver->process_id;
}
static u64 remaining(const Conversation *c) {
    if(!current(c))return 0;
    u64 elapsed=timer_ticks()-c->started,budget=(u64)c->frequency*2;
    return elapsed<budget?budget-elapsed:0;
}
static bool ready(const Conversation *c) {
    for(;;) {
        u64 flags;__asm__ volatile("pushfq; popq %0":"=r"(flags)::"memory");
        interrupts_disable();
        bool valid=current(c),idle=valid && managed_service_idle(&c->driver->service);
        bool scheduled=idle || (valid && scheduler_yield());
        if(flags&(1ULL<<9))interrupts_enable();
        if(idle)return true;
        if(!scheduled)return false;
    }
}
bool polaris_driver_program(PolarisDriver *d,PolarisProgramPlan *p,
    const PolarisProgramIo *io,PolarisProgramResult *out) {
    if(!out)return false;
    k_memset(out,0,sizeof(*out));
    if(!d || !p || d->program_attempted || p->incarnation!=d->connection.incarnation ||
       !p->observation_id || !d->process_id || !timer_initialized() || !timer_frequency() ||
       !device_mmio_interrupts_enabled() || !scheduler_preemption_enabled())return false;
    d->program_attempted=true;
    PolarisProgramGate gate;
    if(!polaris_program_gate_init(&gate,p))return false;
    Conversation c={d,timer_ticks(),timer_frequency()};
    IpcMessage q,r;k_memset(&q,0,sizeof(q));
    q.word_count=4;q.words[0]=POLARIS_NATIVE_HEADER(POLARIS_NATIVE_EXECUTE);q.words[1]=p->observation_id;
    if(!current(&c) || !managed_service_send_oneway(&d->service,&d->connection,&q))return false;
    bool complete=false;
    for(u32 n=0;n<=POLARIS_REBIND_MESSAGES;++n) {
        k_memset(&q,0,sizeof(q));
        u64 timeout=remaining(&c);
        if(!timeout || !ipc_receive_blocking_for(process_kernel(),d->connection.receive_handle,&q,timeout) ||
           !current(&c) || q.word_count!=4 || q.words[1]!=d->connection.incarnation)break;
        if(q.words[0]==POLARIS_NATIVE_REPLY) {
            complete=gate.result.complete && !gate.poisoned &&
                q.words[2]==POLARIS_NATIVE_EXECUTE && q.words[3]==gate.result.frame_delta && ready(&c);
            break;
        }
        u32 value=0;
        if(q.words[0]!=POLARIS_REBIND_REQUEST || !polaris_program_gate_step(&gate,io,q.words[2],q.words[3],&value) ||
           !current(&c))break;
        k_memset(&r,0,sizeof(r));r.word_count=4;
        r.words[0]=POLARIS_REBIND_RESPONSE;r.words[1]=d->connection.incarnation;r.words[2]=q.words[2];r.words[3]=value;
        timeout=remaining(&c);
        if(!timeout || !ipc_send_blocking_for(process_kernel(),d->connection.send_handle,&r,timeout))break;
    }
    *out=gate.result;
    return complete;
}
