#include "ui/draw.hpp"

#include <SDL.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

using namespace xyo::ui;

// Unlike assert(), these checks remain enabled in Release builds.
#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: %s failed (SDL: %s)\n", \
                 __FILE__, __LINE__, #condition, SDL_GetError()); \
    std::abort(); \
} } while (false)

namespace {
struct Fixture {
    SDL_Surface* surface = nullptr;
    SDL_Renderer* renderer = nullptr;
    DrawList draw;
    Fixture() {
        surface=SDL_CreateRGBSurfaceWithFormat(0,128,128,32,SDL_PIXELFORMAT_RGBA32);
        CHECK(surface);
        renderer=SDL_CreateSoftwareRenderer(surface);
        CHECK(renderer);
        clear();
    }
    ~Fixture() { SDL_DestroyRenderer(renderer); SDL_FreeSurface(surface); }
    void clear() {
        CHECK(SDL_RenderSetClipRect(renderer,nullptr)==0);
        CHECK(SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_NONE)==0);
        CHECK(SDL_SetRenderDrawColor(renderer,0,0,0,255)==0);
        CHECK(SDL_RenderClear(renderer)==0);
        draw.begin(128,128);
    }
    PackedColor pixel(int x, int y) {
        CHECK(SDL_RenderFlush(renderer)==0);
        const auto* row=reinterpret_cast<const Uint32*>(
            static_cast<const unsigned char*>(surface->pixels)+y*surface->pitch);
        Uint8 r,g,b,a;
        SDL_GetRGBA(row[x],surface->format,&r,&g,&b,&a);
        return rgba(r,g,b,a);
    }
};

void concaveAndWinding(Fixture& f) {
    // A C-block mouth: duplicate, closing and collinear points are intentional.
    Vec2 p[]={{10,10},{80,10},{80,10},{80,25},{30,25},{30,45},
              {30,65},{80,65},{80,80},{10,80},{10,10}};
    for (int winding=0;winding<2;++winding) {
        f.clear();
        f.draw.AddConcavePolyFilled(p,11,rgba(255,0,0));
        CHECK(f.draw.getBatchCount()==1);
        CHECK(f.draw.render(f.renderer));
        CHECK(f.pixel(20,40)==rgba(255,0,0));
        CHECK(f.pixel(60,17)==rgba(255,0,0));
        CHECK(f.pixel(60,73)==rgba(255,0,0));
        CHECK(f.pixel(60,45)==rgba(0,0,0)); // no fan across the mouth
        for (int i=0;i<11/2;++i) {
            Vec2 tmp=p[i]; p[i]=p[10-i]; p[10-i]=tmp;
        }
    }
}

void clippingAndBatches(Fixture& f) {
    f.clear();
    f.draw.AddRectFilled({0,0},{100,100},rgba(255,0,0));
    f.draw.AddRectFilled({10,10},{20,20},rgba(255,0,0));
    CHECK(f.draw.getBatchCount()==1);
    f.draw.PushClipRect({40,40},{70,70});
    f.draw.PushClipRect({50,20},{90,60},true);
    f.draw.AddRectFilled({0,0},{100,100},rgba(0,255,0));
    CHECK(f.draw.getBatchCount()==2);
    f.draw.PopClipRect();
    // Replace, do not intersect: blue is outside the previous clip.
    f.draw.PushClipRect({90,90},{100,100},false);
    f.draw.AddRectFilled({0,0},{120,120},rgba(0,0,255));
    f.draw.PopClipRect(); f.draw.PopClipRect(); f.draw.PopClipRect();
    f.draw.AddRectFilled({110,110},{120,120},rgba(255,255,255));
    CHECK(f.draw.getBatchCount()==4);
    CHECK(f.draw.render(f.renderer));
    CHECK(f.pixel(55,45)==rgba(0,255,0));
    CHECK(f.pixel(45,45)==rgba(255,0,0));
    CHECK(f.pixel(65,65)==rgba(255,0,0));
    CHECK(f.pixel(95,95)==rgba(0,0,255));
    CHECK(f.pixel(115,115)==rgba(255,255,255));
}

void alphaTextureAndOrder(Fixture& f) {
    f.clear();
    SDL_Texture* texture=SDL_CreateTexture(f.renderer,SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STATIC,1,1);
    CHECK(texture);
    const unsigned char white[]={255,255,255,255};
    CHECK(SDL_UpdateTexture(texture,nullptr,white,4)==0);
    CHECK(SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND)==0);
    f.draw.AddRectFilled({0,0},{120,120},rgba(255,0,0,128));
    f.draw.AddImage(reinterpret_cast<TextureID>(texture),{20,20},{80,80},
                    {0,0},{1,1},rgba(0,255,0));
    f.draw.AddImage(reinterpret_cast<TextureID>(texture),{90,20},{100,30});
    CHECK(f.draw.getBatchCount()==2); // adjacent same texture merges
    f.draw.AddRectFilled({40,40},{60,60},rgba(0,0,255));
    CHECK(f.draw.getBatchCount()==3); // a solid must unbind the previous texture
    CHECK(f.draw.render(f.renderer));
    const auto red=f.pixel(10,10)&255;
    CHECK(red>=126 && red<=129);
    CHECK(f.pixel(30,30)==rgba(0,255,0));
    CHECK(f.pixel(50,50)==rgba(0,0,255));
    SDL_BlendMode blend;
    CHECK(SDL_GetTextureBlendMode(texture,&blend)==0 && blend==SDL_BLENDMODE_BLEND);
    SDL_DestroyTexture(texture);
}

void localSlicesAcrossGeometry(Fixture& f) {
    f.clear();
    // Give later commands large nonzero frame-global vertex bases. A merged
    // texture batch and merged polygon/stroke batch must rebase independently.
    for (int y=0;y<128;++y)
        for (int x=0;x<32;++x)
            f.draw.AddRectFilled({float(x),float(y)},
                                 {float(x+1),float(y+1)},rgba(255,0,0));
    SDL_Texture* texture=SDL_CreateTexture(f.renderer,SDL_PIXELFORMAT_RGBA32,
                                           SDL_TEXTUREACCESS_STATIC,1,1);
    CHECK(texture);
    const unsigned char white[]={255,255,255,255};
    CHECK(SDL_UpdateTexture(texture,nullptr,white,4)==0);
    CHECK(SDL_SetTextureBlendMode(texture,SDL_BLENDMODE_BLEND)==0);
    f.draw.AddImage(reinterpret_cast<TextureID>(texture),{40,10},{50,20},
                    {0,0},{1,1},rgba(0,0,255));
    f.draw.AddImage(reinterpret_cast<TextureID>(texture),{60,10},{70,20},
                    {0,0},{1,1},rgba(255,255,0));
    Vec2 c[]={{40,40},{90,40},{90,50},{55,50},{55,70},{90,70},{90,80},{40,80}};
    f.draw.AddConcavePolyFilled(c,8,rgba(0,255,0));
    f.draw.AddCircleFilled({70,60},4,rgba(255,255,255));
    f.draw.AddRect({95,95},{120,120},rgba(255,255,255),4,0,2);
    f.draw.PushClipRect({100,0},{110,90});
    f.draw.AddImage(reinterpret_cast<TextureID>(texture),{90,0},{120,90},
                    {0,0},{1,1},rgba(0,255,255));
    f.draw.PopClipRect();
    CHECK(f.draw.getBatchCount()==4);
    const auto before=f.draw.stats();
    CHECK(f.draw.render(f.renderer));
    CHECK(f.pixel(10,100)==rgba(255,0,0));
    CHECK(f.pixel(45,15)==rgba(0,0,255));
    CHECK(f.pixel(65,15)==rgba(255,255,0));
    CHECK(f.pixel(45,60)==rgba(0,255,0));
    CHECK(f.pixel(80,60)==rgba(0,0,0)); // C-mouth stays empty
    CHECK(f.pixel(70,60)==rgba(255,255,255));
    CHECK(f.pixel(105,20)==rgba(0,255,255));
    CHECK(f.pixel(115,20)==rgba(0,0,0));
    std::vector<PackedColor> pixels;
    for (int y=0;y<128;++y) for (int x=0;x<128;++x) pixels.push_back(f.pixel(x,y));
    CHECK(SDL_SetRenderDrawColor(f.renderer,0,0,0,255)==0);
    CHECK(SDL_RenderClear(f.renderer)==0);
    CHECK(f.draw.render(f.renderer)); // replay cannot rebase already-local indices
    for (int y=0;y<128;++y) for (int x=0;x<128;++x)
        CHECK(f.pixel(x,y)==pixels[std::size_t(y)*128+x]);
    CHECK(f.draw.stats().vertices==before.vertices &&
          f.draw.stats().indices==before.indices && f.draw.stats().batches==before.batches);
    SDL_DestroyTexture(texture);
}

void invalidGeometry(Fixture& f) {
    f.clear();
    const float nan=std::numeric_limits<float>::quiet_NaN();
    Vec2 flat[]={{1,1},{2,2},{2,2},{3,3}};
    Vec2 invalid[]={{1,1},{nan,2},{10,10}};
    f.draw.AddConcavePolyFilled(flat,4,rgba(255,255,255));
    f.draw.AddConcavePolyFilled(nullptr,4,rgba(255,255,255));
    f.draw.AddConvexPolyFilled(invalid,3,rgba(255,255,255));
    f.draw.AddPolyline(invalid,3,rgba(255,255,255),0,1);
    f.draw.AddLine({0,0},{10,10},rgba(255,255,255),-1);
    f.draw.AddCircleFilled({5,5},-1,rgba(255,255,255));
    f.draw.AddRectFilled({10,10},{1,1},rgba(255,255,255));
    f.draw.AddRectFilled({1,1},{10,10},rgba(255,255,255,0));
    f.draw.PushClipRect({nan,0},{20,20});
    f.draw.AddRectFilled({1,1},{10,10},rgba(255,255,255));
    f.draw.PopClipRect();
    CHECK(f.draw.getVertexCount()==0 && f.draw.getIndexCount()==0);
    Vec2 offscreen[]={{200,200},{300,200},{300,300},{250,250},{200,300}};
    f.draw.AddConcavePolyFilled(offscreen,5,rgba(255,255,255));
    CHECK(f.draw.getBatchCount()==0);
    CHECK(f.draw.render(f.renderer));
    CHECK(!f.draw.render(nullptr));
}

void stateAndCapacity(Fixture& f) {
    f.clear();
    auto record=[&]() {
        for (int i=0;i<32;++i) {
            f.draw.AddRectFilled({float(i),1},{float(i+10),20},rgba(255,255,255),3);
            f.draw.AddCircle({64,64},30,rgba(0,255,0),32,2);
        }
    };
    record();
    const DrawStats size=f.draw.stats(), capacity=f.draw.capacities();
    SDL_Rect saved{1,2,120,119};
    CHECK(SDL_RenderSetClipRect(f.renderer,&saved)==0);
    CHECK(SDL_SetRenderDrawBlendMode(f.renderer,SDL_BLENDMODE_ADD)==0);
    CHECK(f.draw.render(f.renderer));
    SDL_Rect clip;
    SDL_RenderGetClipRect(f.renderer,&clip);
    CHECK(SDL_RenderIsClipEnabled(f.renderer));
    CHECK(clip.x==saved.x && clip.y==saved.y && clip.w==saved.w && clip.h==saved.h);
    SDL_BlendMode blend;
    CHECK(SDL_GetRenderDrawBlendMode(f.renderer,&blend)==0 && blend==SDL_BLENDMODE_ADD);
    CHECK(f.draw.getVertexCount()==size.vertices); // rendering does not consume
    for (int frame=0;frame<16;++frame) {
        f.draw.begin(128,128);
        CHECK(f.draw.getVertexCount()==0 && f.draw.getIndexCount()==0 && f.draw.getBatchCount()==0);
        auto c=f.draw.capacities();
        CHECK(c.vertices==capacity.vertices && c.indices==capacity.indices && c.batches==capacity.batches);
        record();
        CHECK(f.draw.getVertexCount()==size.vertices && f.draw.getIndexCount()==size.indices);
        c=f.draw.capacities();
        CHECK(c.vertices==capacity.vertices && c.indices==capacity.indices && c.batches==capacity.batches);
    }
    CHECK(SDL_RenderSetClipRect(f.renderer,nullptr)==0);
    CHECK(f.draw.render(f.renderer));
    CHECK(!SDL_RenderIsClipEnabled(f.renderer));
}
}

int main() {
    SDL_SetMainReady();
    CHECK(SDL_Init(SDL_INIT_VIDEO)==0);
    {
        Fixture f;
        concaveAndWinding(f);
        clippingAndBatches(f);
        alphaTextureAndOrder(f);
        localSlicesAcrossGeometry(f);
        invalidGeometry(f);
        stateAndCapacity(f);
    }
    SDL_Quit();
    std::puts("native UI regression tests passed");
    return 0;
}
