#include "../../src/render/dlss_jitter.hpp"
#include <cmath>
#include <cstdio>
using namespace xyo::render;
#define CHECK(e) do { if (!(e)) { std::printf("fail at %d\n",__LINE__);return 1;} } while(0)
int main() {
    const auto a=dlssJitter(0);
    const auto b=dlssJitter(1);
    CHECK(std::abs(a.x-0.0f)<0.000001f);
    CHECK(std::abs(a.y+1.0f/6.0f)<0.000001f);
    CHECK(std::abs(b.x+0.25f)<0.000001f);
    CHECK(std::abs(b.y-1.0f/6.0f)<0.000001f);
    for (int i=0;i<80;++i) {
        const auto j=dlssJitter(i);
        CHECK(j.x>=-0.5f && j.x<0.5f);
        CHECK(j.y>=-0.5f && j.y<0.5f);
        const auto same=dlssJitter(i+8);
        CHECK(j.x==same.x && j.y==same.y);
    }
    DlssJitterMotionGate gate;
    DlssJitterSubject sprite{1,120,100,100,90,0};
    CHECK(!gate.update({&sprite,1},480,360)); // no previous motion
    sprite.x+=2.0f;
    CHECK(gate.update({&sprite,1},480,360));
    sprite.x+=6.0f;
    CHECK(!gate.update({&sprite,1},480,360));
    // Stable after the jump; do not unnecessarily suppress jitter forever.
    CHECK(gate.update({&sprite,1},480,360));
    sprite.direction+=15;
    CHECK(gate.update({&sprite,1},480,360));
    sprite.direction+=45;
    CHECK(!gate.update({&sprite,1},480,360));
    sprite.costume++;
    CHECK(!gate.update({&sprite,1},480,360));
    CHECK(gate.update({&sprite,1},480,360));
    DlssJitterSubject another{2,10,10,100,90,0};
    DlssJitterSubject both[]{sprite,another};
    CHECK(!gate.update(both,480,360)); // appearance
    CHECK(!gate.update({&sprite,1},480,360)); // disappearance
    CHECK(gate.update({&sprite,1},480,360));
    // Changed input render resolution resets the sample history.
    CHECK(!gate.update({&sprite,1},960,720));
    gate.reset();
    CHECK(!gate.update({&sprite,1},480,360));
    CHECK(gate.update({&sprite,1},480,360));
    std::puts("PASS: Halton jitter + adaptive translation/costume/visibility gating");
}
