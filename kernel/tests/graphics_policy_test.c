#include "graphics_policy_test.h"
#include "graphics_supervisor.h"
#include "supervisor.h"
#include "terminal.h"
#include "pmm.h"
#include "process.h"
#include "address_space.h"
#include "capability.h"
#include "endpoint.h"
#include "thread.h"
#include "lib.h"
static GraphicsSupervisorSession current;
static bool check(const char *name,bool ok) {
    terminal_write("  ");terminal_write(name);terminal_writeln(ok ? ": PASS" : ": FAILED");
    return ok;
}
static bool request(u32 event,u64 facts,u64 expected) {
    u64 action;
    return graphics_supervisor_request(&current,event,facts,&action) && action==expected;
}
void graphics_policy_test_cleanup_run(void) {
    if (current.session && !current.failed) (void)request(GRAPHICS_POLICY_CLOSE,0,GRAPHICS_POLICY_HEADLESS);
}
void graphics_policy_test_run(void) {
    terminal_writeln("RING3 GRAPHICS RECOVERY POLICY TEST (NO GPU IO):");
    u64 pid=supervisor_process_id(),tid=supervisor_thread_id(),pages=pmm_stats().free_pages;
    u32 processes=process_object_count(),spaces=address_space_object_count(),threads=thread_object_count();
    u32 endpoints=endpoint_object_count(),caps=capability_table_count(process_capabilities(process_kernel()));
    u64 cookie=0,action,old_session=0;
    const u64 alive=GRAPHICS_POLICY_PRIMARY_LIVE|GRAPHICS_POLICY_RESCUE_LIVE;
    const u64 all=alive|GRAPHICS_POLICY_PROBED|GRAPHICS_POLICY_PRIMARY_PONG|GRAPHICS_POLICY_RESCUE_PONG;
    if (!check("EXISTING SUPERVISOR / SAME PROCESS AND THREAD",pid&&tid&&supervisor_idle())) goto fail;
    for (u32 n=0;n<4;++n) {
        k_memset(&current,0,sizeof(current));
        if (!check("ORDERED FRESH POLICY SESSION / NO NEW GRANTS",
            graphics_supervisor_begin(&current) && current.session>old_session &&
            current.process_id==pid && current.thread_id==tid &&
            request(GRAPHICS_POLICY_PRIMARY,0,GRAPHICS_POLICY_KEEP) &&
            request(GRAPHICS_POLICY_OBSERVE,all,GRAPHICS_POLICY_KEEP))) goto fail;
        old_session=current.session;
        if (!check("STALE SESSION / SEQUENCE / UNKNOWN SNAPSHOT REFUSED",
            !supervisor_graphics_exchange(current.session+1,current.sequence+1,GRAPHICS_POLICY_VERSION|GRAPHICS_POLICY_CLOSE,&action) &&
            !supervisor_graphics_exchange(current.session,current.sequence,GRAPHICS_POLICY_VERSION|GRAPHICS_POLICY_CLOSE,&action) &&
            !supervisor_graphics_exchange(current.session,current.sequence+1,GRAPHICS_POLICY_VERSION|GRAPHICS_POLICY_CLOSE|(1ULL<<48),&action))) goto fail;
        if (n<2) {
            u64 facts=n ? alive|GRAPHICS_POLICY_PROBED|GRAPHICS_POLICY_RESCUE_PONG : GRAPHICS_POLICY_RESCUE_LIVE;
            if (!check("PRIMARY EXIT OR MISSING HEARTBEAT -> ONE RESERVED RESCUE",
                request(GRAPHICS_POLICY_OBSERVE,facts,GRAPHICS_POLICY_RECOVER) &&
                request(GRAPHICS_POLICY_RESCUED,0,GRAPHICS_POLICY_KEEP) &&
                request(GRAPHICS_POLICY_CLIENT_RECONNECT,GRAPHICS_POLICY_RECONNECT_FACTS,GRAPHICS_POLICY_CONNECT) &&
                !supervisor_graphics_exchange(current.session,current.sequence+1,
                    GRAPHICS_POLICY_VERSION|GRAPHICS_POLICY_CLIENT_RECONNECT|GRAPHICS_POLICY_RECONNECT_FACTS,&action) &&
                request(GRAPHICS_POLICY_OBSERVE,GRAPHICS_POLICY_RESCUE_LIVE|GRAPHICS_POLICY_PROBED|GRAPHICS_POLICY_RESCUE_PONG,GRAPHICS_POLICY_KEEP) &&
                request(GRAPHICS_POLICY_OBSERVE,n ? GRAPHICS_POLICY_RESCUE_LIVE|GRAPHICS_POLICY_PROBED : 0,GRAPHICS_POLICY_HEADLESS))) goto fail;
        } else {
            if (!check("FAILED STANDBY WINS / NO RESCUE",
                request(GRAPHICS_POLICY_OBSERVE,n==2 ? GRAPHICS_POLICY_PRIMARY_LIVE : alive|GRAPHICS_POLICY_PROBED,GRAPHICS_POLICY_HEADLESS))) goto fail;
        }
        if(n<2 && !check("ACTUAL RING3 ONE-SHOT GUI RECONNECT / DUPLICATE REFUSED",current.sequence>4))goto fail;
        if (!check("CLOSED SESSION CANNOT REARM / UNRELATED SUPERVISOR PING",
            !request(GRAPHICS_POLICY_REQUEST_RESCUE,0,GRAPHICS_POLICY_RECOVER) &&
            supervisor_ping(0x475055504F4C4943ULL,&cookie) && cookie==0x475055504F4C4943ULL &&
            supervisor_process_id()==pid && supervisor_thread_id()==tid && supervisor_idle())) goto fail;
    }
    if (!check("POLICY IPC PRESERVES PAGES/OBJECTS/CAPS",pmm_stats().free_pages==pages &&
        process_object_count()==processes && address_space_object_count()==spaces && thread_object_count()==threads &&
        endpoint_object_count()==endpoints && capability_table_count(process_capabilities(process_kernel()))==caps)) goto fail;
    terminal_writeln("RING3 GRAPHICS RECOVERY POLICY TEST: PASS");return;
fail:
    terminal_writeln("RING3 GRAPHICS RECOVERY POLICY TEST: FAILED");
}
