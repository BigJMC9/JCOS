#include "graphics_policy.h"
enum { IDLE,ARMED,PRIMARY,RECOVERING,RESCUE,CLOSED };
JcosU64 graphics_policy_decide(GraphicsPolicy *p,JcosU64 session,JcosU64 sequence,JcosU64 snapshot) {
    JcosU64 event=snapshot&0xFFULL,facts=snapshot&GRAPHICS_POLICY_FACTS;
    if (!p || !session || !sequence ||
        snapshot!=(GRAPHICS_POLICY_VERSION|event|facts) ||
        event<GRAPHICS_POLICY_BEGIN || event>GRAPHICS_POLICY_CLOSE ||
        (event!=GRAPHICS_POLICY_OBSERVE && facts)) return GRAPHICS_POLICY_REJECT;
    if (event==GRAPHICS_POLICY_BEGIN) {
        if ((p->stage!=IDLE && p->stage!=CLOSED) || session<=p->session || sequence!=1)
            return GRAPHICS_POLICY_REJECT;
        p->session=session;p->sequence=sequence;p->stage=ARMED;
        return GRAPHICS_POLICY_ACCEPT;
    }
    if (p->stage==IDLE || session!=p->session || p->sequence==~0ULL ||
        sequence!=p->sequence+1) return GRAPHICS_POLICY_REJECT;
    JcosU32 next=p->stage;
    JcosU64 action=GRAPHICS_POLICY_REJECT;
    if (event==GRAPHICS_POLICY_CLOSE) {next=CLOSED;action=GRAPHICS_POLICY_HEADLESS;}
    else if (event==GRAPHICS_POLICY_PRIMARY && p->stage==ARMED) {next=PRIMARY;action=GRAPHICS_POLICY_KEEP;}
    else if (event==GRAPHICS_POLICY_RESCUED && p->stage==RECOVERING) {next=RESCUE;action=GRAPHICS_POLICY_KEEP;}
    else if (event==GRAPHICS_POLICY_REQUEST_RESCUE && p->stage==PRIMARY) {next=RECOVERING;action=GRAPHICS_POLICY_RECOVER;}
    else if (event==GRAPHICS_POLICY_OBSERVE && (p->stage==PRIMARY || p->stage==RESCUE)) {
        JcosU64 a=GRAPHICS_POLICY_PRIMARY_LIVE,b=GRAPHICS_POLICY_RESCUE_LIVE;
        JcosU64 ap=GRAPHICS_POLICY_PRIMARY_PONG,bp=GRAPHICS_POLICY_RESCUE_PONG;
        if ((facts&(ap|bp)) && !(facts&GRAPHICS_POLICY_PROBED)) return GRAPHICS_POLICY_REJECT;
        if (((facts&ap) && !(facts&a)) || ((facts&bp) && !(facts&b)) ||
            (p->stage==RESCUE && (facts&(a|ap)))) return GRAPHICS_POLICY_REJECT;
        action=GRAPHICS_POLICY_KEEP;
        /* Failed standby wins over failed primary: never rescue with a bad peer. */
        if (!(facts&b) || ((facts&GRAPHICS_POLICY_PROBED) && !(facts&bp))) {
            next=CLOSED;action=GRAPHICS_POLICY_HEADLESS;
        } else if (p->stage==PRIMARY && (!(facts&a) ||
                   ((facts&GRAPHICS_POLICY_PROBED) && !(facts&ap)))) {
            next=RECOVERING;action=GRAPHICS_POLICY_RECOVER;
        }
    }
    if (action==GRAPHICS_POLICY_REJECT) return action;
    p->stage=next;p->sequence=sequence;
    return action;
}
