#include "../../src/render/dlss_pen_replay.hpp"
#include <array>
#include <cstdio>
#include <vector>
using namespace xyo::render;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);return 1; } } while(0)
int main() {
    std::vector<uint8_t> imageA,imageB,imageC;
    DlssPenReplayStroke stroke{{0.75f,0.0f,0.0f,1.0f},3.0f,-10,0,10,0};
    CHECK(dlssReplayPenStrokes({&stroke,1},480,360,240,180,0,0,imageA));
    CHECK(dlssReplayPenStrokes({&stroke,1},480,360,240,180,0.25f,-0.25f,imageB));
    CHECK(imageA!=imageB);
    CHECK(imageA.size()==240u*180u*4);
    CHECK(dlssReplayPenStrokes({&stroke,1},480,360,240,180,0,0,imageC));
    CHECK(imageA==imageC);
    CHECK(imageA[3]==0); // outside the stroke
    bool opaqueFound=false;
    for (size_t i=3;i<imageA.size();i+=4)
        if (imageA[i]>0) { opaqueFound=true;break; }
    CHECK(opaqueFound);
    DlssPenReplayStroke duplicate = stroke;
    duplicate.rgba={0.0f,0.0f,1.0f,0.5f};
    std::array<DlssPenReplayStroke,2> pair{stroke,duplicate};
    CHECK(dlssReplayPenStrokes(pair,480,360,240,180,0,0,imageC));
    CHECK(imageA!=imageC); // alpha composition changes colors
    CHECK(!dlssReplayPenStrokes(pair,480,360,0,180,0,0,imageC));
    std::vector<DlssPenReplayStroke> huge(kDlssPenReplayMaxStrokes+1,stroke);
    CHECK(!dlssReplayPenStrokes(huge,480,360,240,180,0,0,imageC));
    // Excessive stroke area falls back instead of spending hundreds of
    // millions of pixels on a display-only quality experiment.
    DlssPenReplayStroke wide=stroke;
    wide.diameter=4096;
    std::array<DlssPenReplayStroke, 32> costly{};
    costly.fill(wide);
    CHECK(!dlssReplayPenStrokes(costly,480,360,240,180,0.25f,0,imageC));
    CHECK(!dlssPenReplayWithinBudget(costly,480,360,240,180,0.25f,0));
    CHECK(dlssPenReplayWithinBudget(pair,480,360,240,180,0.25f,0));
    CHECK(!dlssPenReplayWithinBudget(huge,480,360,240,180,0,0));
    std::puts("PASS: analytic pen stroke jitter changes samples; unchanged replay deterministic");
}
