#include "../../src/render/dlss_rigid_motion.hpp"
#include <cstdio>
using namespace xyo::render;
#define CHECK(x) do { if (!(x)) { std::printf("FAILED at line %d\n", __LINE__); return 1; } } while (0)
int main() {
    DlssRigidMotionTracker tracker;
    DlssRigidSprite a{1, uintptr_t(0x1234), 100, 90, 50, 40, 150, 140,
                      100, 90, 0, true};
    CHECK(tracker.update({&a,1},480,360).empty());
    a.centerX += 6; a.left += 6; a.right += 6;
    auto mv=tracker.update({&a,1},480,360);
    CHECK(mv.size()==1 && mv[0].deltaX==-6 && mv[0].deltaY==0);
    CHECK(mv[0].left==59 && mv[0].right==153);
    CHECK(tracker.update({&a,1},480,360).empty());
    a.costume++;
    a.centerX+=5; a.left+=5; a.right+=5;
    CHECK(tracker.update({&a,1},480,360).empty());
    a.centerX+=100; a.left+=100; a.right+=100;
    CHECK(tracker.update({&a,1},480,360).empty());
    a.opaqueUnrotated=false; a.centerX+=6; a.left+=6; a.right+=6;
    CHECK(tracker.update({&a,1},480,360).empty());
    a.opaqueUnrotated=true;
    CHECK(tracker.update({&a,1},480,360).empty());
    // Fractional SDL translations must fall back to Optical Flow.
    a.centerX += 0.5f; a.left += 0.5f; a.right += 0.5f;
    CHECK(tracker.update({&a,1},480,360).empty());
    tracker.reset();
    a= {1, uintptr_t(1), 100, 100, 60, 60, 140, 140, 100, 90, 0,true};
    DlssRigidSprite b{2, uintptr_t(2), 140, 90, 135, 80, 150, 105,
                      100,90,0,false};
    DlssRigidSprite both[]{a,b};
    CHECK(tracker.update(both,480,360).empty());
    both[0].centerX+=6;both[0].left+=6;both[0].right+=6;
    CHECK(tracker.update(both,480,360).empty());
    tracker.reset();
    CHECK(tracker.update({&a,1},480,360).empty());
    a.centerY+=5;a.top+=5;a.bottom+=5;
    CHECK(tracker.update({&a,1},960,720).empty());
    a.centerX=1.0e15f; a.left=1.0e15f; a.right=1.0e15f + 100;
    CHECK(tracker.update({&a,1},960,720).empty());
    // Affine: a 24x24 centered sprite rotates 90 degrees clockwise.
    DlssRigidSprite old{}, now{};
    old.id=10;old.costume=0xAA;old.size=100;old.direction=90;
    old.opaqueUnrotated=true;
    old.centerX=100;old.centerY=90;
    old.left=88;old.top=78;old.right=112;old.bottom=102;
    old.rasterX=88;old.rasterY=78;old.rasterW=24;old.rasterH=24;
    old.pivotX=12;old.pivotY=12;
    now=old;now.direction=180;now.renderAngle=90;
    tracker.reset();
    CHECK(tracker.update({&old,1},480,360,true).empty());
    auto affine=tracker.update({&now,1},480,360,true);
    CHECK(affine.size()==1 && affine[0].affine==1);
    auto& rotation=affine[0];
    // Current point 6px right of center maps 6px above in old frame.
    const float x=106.0f,y=90.0f;
    const float prevX=rotation.previousX[0]*x+
        rotation.previousX[1]*y+rotation.previousX[2];
    const float prevY=rotation.previousY[0]*x+
        rotation.previousY[1]*y+rotation.previousY[2];
    CHECK(std::abs(prevX-100.0f)<0.02f);
    CHECK(std::abs(prevY-84.0f)<0.02f);
    const float u=rotation.currentU[0]*x+
        rotation.currentU[1]*y+rotation.currentU[2];
    const float v=rotation.currentV[0]*x+
        rotation.currentV[1]*y+rotation.currentV[2];
    CHECK(u>0 && u<1 && v>0 && v<1);
    CHECK(tracker.update({&now,1},480,360,true).empty());
    // Scale from 24x24 to 30x30, no rotation: (15px -> 12px)
    old=now;
    old.renderAngle=0;old.direction=90;
    now=old;
    now.size=125;now.rasterW=30;now.rasterH=30;
    now.rasterX=85;now.rasterY=75;
    now.pivotX=15;now.pivotY=15;
    now.left=85;now.top=75;now.right=115;now.bottom=105;
    tracker.reset();
    CHECK(tracker.update({&old,1},480,360,true).empty());
    affine=tracker.update({&now,1},480,360,true);
    CHECK(affine.size()==1);
    const auto& scaling=affine[0];
    const float sx=scaling.previousX[0]*110+
        scaling.previousX[1]*90+scaling.previousX[2];
    CHECK(std::abs(sx-108.0f)<0.05f);
    // Costume mismatch or sprite occlusion means NO affine override.
    now.costume++;
    CHECK(tracker.update({&now,1},480,360,true).empty());
    tracker.reset();
    const DlssRigidSprite blocker{11,uintptr_t(2),105,88,
                                  97,82,115,104,100,90,0,false};
    DlssRigidSprite overlapSamples[]{old,blocker};
    CHECK(tracker.update(overlapSamples,480,360,true).empty());
    overlapSamples[0]=now;overlapSamples[0].costume=old.costume;
    CHECK(tracker.update(overlapSamples,480,360,true).empty());
    std::puts("PASS: rigid and affine motion, rotation, scaling, invalid geometry");
}
