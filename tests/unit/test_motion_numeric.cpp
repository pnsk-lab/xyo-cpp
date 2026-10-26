// JIT's already-converted motion ABI must preserve Target::setXY semantics.
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>
#include "core/cast.hpp"
#include "engine/runtime.hpp"
#include "jit/jit.hpp"
#include "load/sb3.hpp"
#include "model/target.hpp"
using namespace xyo;
#if XYO_JIT
extern "C" {
void xyo_mot_gotoxy(JitCtx*, uint64_t, uint64_t);
void xyo_mot_setx(JitCtx*, uint64_t);
void xyo_mot_sety(JitCtx*, uint64_t);
void xyo_mot_changex(JitCtx*, uint64_t);
void xyo_mot_changey(JitCtx*, uint64_t);
void xyo_mot_setxy_num(JitCtx*, double, double);
}
namespace {
struct Event { char kind; double x,y,oldX,oldY; };
thread_local std::vector<Event>* events = nullptr;
void moved(Target* target, double x, double y, bool force) {
    events->push_back({'m',target->x,target->y,x,y});
    if (force) events->push_back({'!',0,0,0,0});
}
struct FenceRenderer : RendererAPI {
    void getFencedPosition(int, double& x,double& y,bool fence) const override {
        events->push_back({'f',x,y,0,0});
        if(fence) {
            if(x>320)x=320; if(x< -320)x=-320;
            if(y>240)y=240; if(y< -240)y=-240;
        }
    }
    void emitVisualChange(Target* target) override {
        events->push_back({'v',target->x,target->y,0,0});
    }
};
bool same(double a,double b) {
    return (std::isnan(a)&&std::isnan(b)) || (a==b && (a!=0 || std::signbit(a)==std::signbit(b)));
}
bool sameEvents(const std::vector<Event>& a,const std::vector<Event>& b) {
    if(a.size()!=b.size())return false;
    for(size_t i=0;i<a.size();++i) if(a[i].kind!=b[i].kind ||
        !same(a[i].x,b[i].x)||!same(a[i].y,b[i].y)||
        !same(a[i].oldX,b[i].oldX)||!same(a[i].oldY,b[i].oldY))return false;
    return true;
}
}
#endif
int test_motion_numeric_main() {
#if !XYO_JIT
    std::puts("numeric motion: skipped (no JIT helpers)");
    return 0;
#else
    Gc::DeferCollect defer;
    FenceRenderer renderer;
    Runtime rt;
    const char* json=R"({"targets":[{"isStage":true,"name":"Stage","variables":{},"lists":{},"blocks":{},"costumes":[],"sounds":[]},{"isStage":false,"name":"S","variables":{},"lists":{},"blocks":{},"costumes":[],"sounds":[],"visible":true}],"monitors":[],"extensions":[],"meta":{}})";
    if(!loadSb3(rt,json,nullptr).ok)return 1;
    Target* target=rt.targets[1];
    JitCtx ctx{}; ctx.target=target;ctx.runtime=&rt;
    double inf=std::numeric_limits<double>::infinity();
    double nan=std::numeric_limits<double>::quiet_NaN();
    std::vector<Value> inputs={Value::num(0),Value::num(-0.0),Value::num(1.5),
        Value::num(-1000),Value::num(1000),Value::num(inf),Value::num(-inf),
        Value::numCanonical(nan),Value::emptyStr(),Value::boolean(true),
        Value::boolean(false),Value::undefined(),
        Value::string(SStr::create(u"42.5")),Value::string(SStr::create(u"foo")),
        Value::string(SStr::create(u"Infinity")),Value::string(SStr::create(u"-0"))};
    int checks=0,failures=0;
    auto check=[&](bool ok){++checks;if(!ok)++failures;};
    for(bool attached:{false,true})for(bool stage:{false,true})
    for(bool dragging:{false,true})for(bool visible:{false,true})
    for(int initial=0;initial<3;++initial)for(Value input:inputs)for(int op=0;op<5;++op) {
        const double ix=initial==0?10:initial==1?inf:nan;
        const double iy=initial==0?-20:initial==1?-inf:-0.0;
        auto reset=[&]{
            target->x=ix;target->y=iy;target->isStage=stage;
            target->dragging=dragging;target->visible=visible;
            target->onTargetMoved=&moved;
            rt.renderer=attached?&renderer:nullptr;
            rt.refreshTargets=false;rt.redrawRequested=false;
        };
        std::vector<Event> oldEvents,newEvents;
        reset();events=&oldEvents;
        switch(op){
        case 0:xyo_mot_changex(&ctx,input.bits);break;
        case 1:xyo_mot_changey(&ctx,input.bits);break;
        case 2:xyo_mot_setx(&ctx,input.bits);break;
        case 3:xyo_mot_sety(&ctx,input.bits);break;
        default:xyo_mot_gotoxy(&ctx,input.bits,input.bits);break;
        }
        double oldX=target->x,oldY=target->y;
        bool oldRefresh=rt.refreshTargets,oldRedraw=rt.redrawRequested;
        reset();events=&newEvents;
        double d=cast::toNumber(input);
        double nx=op==0?ix+d:op==2||op==4?d:ix;
        double ny=op==1?iy+d:op==3||op==4?d:iy;
        xyo_mot_setxy_num(&ctx,nx,ny);
        check(same(target->x,oldX)&&same(target->y,oldY));
        check(rt.refreshTargets==oldRefresh && rt.redrawRequested==oldRedraw);
        check(sameEvents(oldEvents,newEvents));
    }
    target->onTargetMoved=nullptr;target->isStage=false;rt.renderer=nullptr;events=nullptr;
    std::printf("numeric motion: %d checks, %d failures\n",checks,failures);
    return failures;
#endif
}
