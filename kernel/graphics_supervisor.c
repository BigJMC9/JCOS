#include "graphics_supervisor.h"
#include "supervisor.h"
static u64 g_next_session;
bool graphics_supervisor_request(GraphicsSupervisorSession *s,u32 event,u64 facts,u64 *action) {
    if (!action) return false;
    *action=GRAPHICS_POLICY_REJECT;
    if (!s || s->failed || !s->session || event<GRAPHICS_POLICY_BEGIN || event>GRAPHICS_POLICY_CLOSE ||
        (facts&~GRAPHICS_POLICY_FACTS) || (event!=GRAPHICS_POLICY_OBSERVE && facts)) return false;
    if (!supervisor_running() || supervisor_process_id()!=s->process_id ||
        supervisor_thread_id()!=s->thread_id || s->sequence==~0ULL) {s->failed=true;return false;}
    /* Consume the request before delivery. Ambiguous results are never replayed. */
    u64 decision=GRAPHICS_POLICY_REJECT;
    bool ok=supervisor_graphics_exchange(s->session,++s->sequence,
        GRAPHICS_POLICY_VERSION|(u64)event|facts,&decision) &&
        supervisor_process_id()==s->process_id && supervisor_thread_id()==s->thread_id &&
        decision<=GRAPHICS_POLICY_HEADLESS;
    if (!ok) {s->failed=true;return false;}
    *action=decision;return true;
}
bool graphics_supervisor_begin(GraphicsSupervisorSession *s) {
    if (!s || s->session || s->failed || g_next_session==~0ULL || !supervisor_idle()) return false;
    s->process_id=supervisor_process_id();s->thread_id=supervisor_thread_id();
    if (!s->process_id || !s->thread_id) return false;
    s->session=++g_next_session;s->sequence=0;
    u64 action;
    bool ok=graphics_supervisor_request(s,GRAPHICS_POLICY_BEGIN,0,&action) && action==GRAPHICS_POLICY_ACCEPT;
    if (!ok) s->failed=true;
    return ok;
}
