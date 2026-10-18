// xyo-cpp: ビルトインエディタ（SDL2）。
// scratch-gui のレイアウト（メニューバー・ステージ・スプライトリスト・
// プロパティパネル）を参考にした内蔵エディタ。
#include "editor.hpp"

#include <cstdio>
#include <vector>

#ifdef XYO_HAVE_SDL2

#include <SDL.h>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/cast.hpp"
#include "core/fileio.hpp"
#include "core/slist.hpp"
#include "core/sstr.hpp"
#include "engine/runtime.hpp"
#include "load/sb3.hpp"
#include "model/blocks.hpp"
#include "model/target.hpp"
#include "render/sdl_renderer.hpp"
#include "render/text.hpp"
#include <thread>

namespace fs = std::filesystem;
using namespace xyo;

namespace {

struct Color { uint8_t r, g, b, a; };
constexpr Color kBg{0x1e,0x22,0x2c,255};
constexpr Color kPanel{0x2a,0x30,0x3d,255};
constexpr Color kHeader{0x35,0x40,0x55,255};
constexpr Color kSel{0x39,0x4b,0x6b,255};
constexpr Color kFg{0xe8,0xec,0xf4,255};
constexpr Color kDim{0x9a,0xa5,0xb8,255};
constexpr Color kAccent{0x3d,0x8b,0x5a,255};
constexpr Color kWarn{0xb8,0x53,0x4a,255};
constexpr Color kField{0x15,0x18,0x20,255};
constexpr Color kBorder{0x55,0x60,0x7a,255};

static std::string utf16ToUtf8(const std::u16string& s) { return SStr::toUtf8(s); }

} // anon

// ---------------------------------------------------------------------------
// Editor
// ---------------------------------------------------------------------------
struct Editor {
    Runtime* rt = nullptr;
    SdlRenderer ren;
    SDL_Window* inspWin = nullptr;
    SDL_Renderer* inspRen = nullptr;
    std::string title;
    int scale = 2;

    // selection
    int selTarget = 0; // index into rt->targets
    // text inputs
    struct Field { std::string buf; bool focused = false; };
    // property fields
    Field fName, fX, fY, fDir, fSize;
    // variable editing
    std::vector<std::string> varIds;
    int varFocus = -1;
    std::vector<Field> varFields;
    bool dirtyVars = true;

    // ui state
    bool running = false;
    bool quit = false;
    int scrollSprites = 0;
    int scrollProps = 0;

    // font texture cache
    struct Tex { SDL_Texture* t=nullptr; int w=0,h=0; };
    std::unordered_map<std::string,Tex> cache;

    SDL_Texture* textTex(SDL_Renderer* r, const std::string& utf8, int px,
                         Color c, int* w, int* h) {
        char suf[48];
        std::snprintf(suf, sizeof(suf), "|%d|%02x%02x%02x", px, c.r,c.g,c.b);
        std::string key = utf8 + suf;
        auto it = cache.find(key);
        if (it != cache.end()) { *w=it->second.w; *h=it->second.h; return it->second.t; }
        std::u16string u = utf8ToU16(utf8);
        std::vector<uint8_t> bmp;
        int tw=0,th=0;
        FontRenderer* fr = FontRenderer::get();
        if (!fr || !fr->render(u, px, c.r,c.g,c.b, bmp, tw, th)) return nullptr;
        SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(bmp.data(), tw, th, 32, tw*4, SDL_PIXELFORMAT_RGBA32);
        if (!surf) return nullptr;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(r, surf);
        SDL_FreeSurface(surf);
        if (!tex) return nullptr;
        SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
        cache[key] = Tex{tex,tw,th};
        *w=tw; *h=th;
        return tex;
    }
    void drawText(SDL_Renderer* r, int x, int y, const std::string& s, int px, Color c, int* outW=nullptr) {
        int w=0,h=0;
        SDL_Texture* t = textTex(r, s, px, c, &w, &h);
        if (!t) {
            // fallback
            int n = (int)utf8ToU16(s).size();
            SDL_SetRenderDrawColor(r, c.r,c.g,c.b,c.a);
            SDL_Rect rr{x,y+3, n*(px/2), std::max(2,px/3)};
            SDL_RenderFillRect(r,&rr);
            if (outW) *outW = n*(px/2);
            return;
        }
        SDL_Rect dst{x,y,w,h};
        SDL_RenderCopy(r,t,nullptr,&dst);
        if (outW) *outW = w;
    }
    void fill(SDL_Renderer* r, int x,int y,int w,int h, Color c) {
        SDL_SetRenderDrawColor(r,c.r,c.g,c.b,c.a);
        SDL_Rect rr{x,y,w,h}; SDL_RenderFillRect(r,&rr);
    }
    void outline(SDL_Renderer* r, int x,int y,int w,int h, Color c) {
        SDL_SetRenderDrawColor(r,c.r,c.g,c.b,c.a);
        SDL_Rect rr{x,y,w,h}; SDL_RenderDrawRect(r,&rr);
    }

    Target* selected() {
        if (!rt || rt->targets.empty()) return nullptr;
        if (selTarget < 0) selTarget = 0;
        if (selTarget >= (int)rt->targets.size()) selTarget = (int)rt->targets.size()-1;
        return rt->targets[selTarget];
    }

    void syncFieldsFromTarget() {
        Target* t = selected();
        if (!t) return;
        fName.buf = utf16ToUtf8(t->getName());
        char buf[64];
        std::snprintf(buf,sizeof(buf),"%.4g", t->x); fX.buf = buf;
        std::snprintf(buf,sizeof(buf),"%.4g", t->y); fY.buf = buf;
        std::snprintf(buf,sizeof(buf),"%.4g", t->direction); fDir.buf = buf;
        std::snprintf(buf,sizeof(buf),"%.4g", t->size); fSize.buf = buf;
        // vars
        varIds.clear();
        for (auto& [id,v] : t->variables) varIds.push_back(id);
        std::sort(varIds.begin(), varIds.end());
        varFields.assign(varIds.size(), Field{});
        for (size_t i=0;i<varIds.size();++i) {
            Variable& v = t->variables[varIds[i]];
            if (v.type == Variable::kListType) {
                varFields[i].buf = "<list>";
            } else {
                SStr* s = cast::toString(v.value);
                varFields[i].buf = s ? SStr::toUtf8(s->view()) : "";
            }
        }
        dirtyVars = false;
    }

    void applyFieldsToTarget() {
        Target* t = selected();
        if (!t) return;
        // name (only sprites have sprite name)
        if (t->sprite) t->sprite->name = utf8ToU16(fName.buf);
        char* end;
        double d;
        d = std::strtod(fX.buf.c_str(), &end); if (end!=fX.buf.c_str()) t->setXY(d, t->y);
        d = std::strtod(fY.buf.c_str(), &end); if (end!=fY.buf.c_str()) t->setXY(t->x, d);
        d = std::strtod(fDir.buf.c_str(), &end); if (end!=fDir.buf.c_str()) t->setDirection(d);
        d = std::strtod(fSize.buf.c_str(), &end); if (end!=fSize.buf.c_str()) t->setSize(d);
    }

    void applyVarField(int i) {
        Target* t = selected();
        if (!t || i<0 || i>=(int)varIds.size()) return;
        Variable& v = t->variables[varIds[i]];
        if (v.type == Variable::kListType) return;
        const std::string& buf = varFields[i].buf;
        char* end=nullptr;
        double d = std::strtod(buf.c_str(), &end);
        if (end != buf.c_str() && *end == 0) v.value = Value::num(d);
        else v.value = Value::string(SStr::fromUtf8(buf));
    }

    void drawInspector() {
        SDL_Renderer* r = inspRen;
        int W,H; SDL_GetWindowSize(inspWin,&W,&H);
        fill(r,0,0,W,H,kBg);
        int y = 0;
        // header
        fill(r,0,0,W,36,kHeader);
        drawText(r, 10, 8, "xyo Editor", 16, kFg);
        // run/stop buttons
        // Run button
        {
            SDL_Rect rr{W-120,4,50,28};
            fill(r, rr.x, rr.y, rr.w, rr.h, kAccent);
            outline(r, rr.x, rr.y, rr.w, rr.h, kBorder);
            drawText(r, rr.x+12, rr.y+4, "▶", 16, kFg);
        }
        {
            SDL_Rect rr{W-60,4,50,28};
            fill(r, rr.x, rr.y, rr.w, rr.h, kWarn);
            outline(r, rr.x, rr.y, rr.w, rr.h, kBorder);
            drawText(r, rr.x+12, rr.y+4, "■", 16, kFg);
        }
        y = 44;

        // sprite list
        drawText(r, 10, y, "Sprites", 13, kDim); y+=18;
        int listH = 120;
        fill(r, 8, y, W-16, listH, kPanel);
        outline(r, 8, y, W-16, listH, kBorder);
        // render list
        int rowH = 20;
        int visibleRows = listH / rowH;
        int start = scrollSprites;
        for (int i=0;i<visibleRows;++i) {
            int idx = start + i;
            if (idx >= (int)rt->targets.size()) break;
            Target* t = rt->targets[idx];
            int ry = y + i*rowH;
            if (idx == selTarget) fill(r, 10, ry+1, W-20, rowH-2, kSel);
            std::string nm = utf16ToUtf8(t->getName());
            if (t->isStage) nm += " (stage)";
            drawText(r, 14, ry+2, nm, 12, kFg);
        }
        y += listH + 8;

        // properties
        Target* t = selected();
        if (t) {
            drawText(r, 10, y, "Properties", 13, kDim); y+=18;
            // name field
            auto drawField = [&](const char* label, Field& f, int& yy) {
                drawText(r, 10, yy+2, label, 12, kFg);
                int fx = 90, fw = W-100, fh = 20;
                fill(r, fx, yy, fw, fh, kField);
                outline(r, fx, yy, fw, fh, f.focused ? kAccent : kBorder);
                // clip text
                std::string show = f.buf;
                drawText(r, fx+4, yy+2, show, 12, kFg);
                yy += 24;
            };
            if (dirtyVars) syncFieldsFromTarget();
            // sync name etc only if not focused? We'll sync on selection change by tracking selTarget.
            drawField("Name", fName, y);
            drawField("X", fX, y);
            drawField("Y", fY, y);
            drawField("Dir", fDir, y);
            drawField("Size", fSize, y);
            // visible toggle
            drawText(r, 10, y+2, "Visible", 12, kFg);
            {
                int bx = 90;
                fill(r, bx, y, 20, 20, t->visible ? kAccent : kPanel);
                outline(r, bx, y, 20, 20, kBorder);
                if (t->visible) drawText(r, bx+4, y+2, "✓", 12, kFg);
            }
            y += 24;
            // draggable toggle
            drawText(r, 10, y+2, "Draggable", 12, kFg);
            {
                int bx = 90;
                fill(r, bx, y, 20, 20, t->draggable ? kAccent : kPanel);
                outline(r, bx, y, 20, 20, kBorder);
                if (t->draggable) drawText(r, bx+4, y+2, "✓", 12, kFg);
            }
            y += 30;

            // variables
            drawText(r, 10, y, "Variables", 13, kDim); y+=18;
            if (varIds.empty()) {
                drawText(r, 10, y, "(none)", 11, kDim); y+=16;
            } else {
                for (size_t i=0;i<varIds.size();++i) {
                    Variable& v = t->variables[varIds[i]];
                    std::string nm = utf16ToUtf8(v.name);
                    drawText(r, 10, y+2, nm, 11, kFg);
                    int fx = 10, fy = y+14, fw = W-20, fh = 18;
                    fill(r, fx, fy, fw, fh, kField);
                    outline(r, fx, fy, fw, fh, (varFocus==(int)i) ? kAccent : kBorder);
                    drawText(r, fx+4, fy+1, varFields[i].buf, 11, kFg);
                    y += 36;
                }
            }
        }

        // scripts (code) list at bottom
        y = std::max(y, H-180);
        drawText(r, 10, y, "Scripts (top-level)", 13, kDim); y+=18;
        if (t) {
            // count scripts
            int cnt = 0;
            for (auto& [id,b] : t->blocks->allBlocks()) {
                if (b.topLevel) {
                    drawText(r, 10, y, "- " + b.opcode, 11, kFg);
                    y += 14;
                    if (++cnt > 8) { drawText(r, 10, y, "...", 11, kDim); break; }
                }
            }
            if (cnt==0) drawText(r, 10, y, "(none)", 11, kDim);
        }

        SDL_RenderPresent(r);
    }

    // focus tracking for sync
    int lastSel = -1;
    bool fieldsSynced = false;

    void handleInspectorEvent(const SDL_Event& ev) {
        if (ev.type == SDL_QUIT) { quit = true; return; }
        if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_CLOSE) {
            if (ev.window.windowID == SDL_GetWindowID(inspWin)) quit = true;
            return;
        }
        if (ev.type == SDL_MOUSEBUTTONDOWN && ev.button.button == SDL_BUTTON_LEFT) {
            int mx = ev.button.x, my = ev.button.y;
            int W,H; SDL_GetWindowSize(inspWin,&W,&H);
            // run/stop
            if (my>=4 && my<32) {
                if (mx >= W-120 && mx < W-70) {
                    rt->greenFlag(); running = true; return;
                }
                if (mx >= W-60 && mx < W-10) {
                    rt->stopAll(); running = false; return;
                }
            }
            // sprite list click
            int listY = 44 + 18;
            int listH = 120;
            if (mx>=8 && mx<W-8 && my>=listY && my<listY+listH) {
                int row = (my - listY) / 20;
                int idx = scrollSprites + row;
                if (idx >=0 && idx < (int)rt->targets.size()) {
                    selTarget = idx;
                    dirtyVars = true;
                    fieldsSynced = false;
                    syncFieldsFromTarget();
                    fieldsSynced = true;
                }
                return;
            }
            // visible/draggable toggles
            Target* t = selected();
            if (t) {
                // approximate positions: recompute y
                // We'll handle by hit-testing property region roughly.
            }
            // text field focus
            // Determine field rects by recompute layout
            // name/x/y/dir/size
            int y = 44 + 18 + 120 + 8;
            // properties header
            y += 18;
            struct R { int x,y,w,h; Field* f; };
            std::vector<R> rects;
            for (int i=0;i<5;++i) {
                rects.push_back({90, y, W-100, 20, nullptr});
                // assign later
                y += 24;
            }
            // map to fields
            Field* fptrs[5] = {&fName,&fX,&fY,&fDir,&fSize};
            for (int i=0;i<5;++i) rects[i].f = fptrs[i];
            // visible toggle at y
            int visY = y; // after 5 fields
            int dragY = y + 24;
            // variables region after
            // Check field hit
            bool hitField = false;
            for (int i=0;i<5;++i) {
                if (mx>=rects[i].x && mx<rects[i].x+rects[i].w && my>=rects[i].y && my<rects[i].y+rects[i].h) {
                    // unfocus all
                    fName.focused=fX.focused=fY.focused=fDir.focused=fSize.focused=false;
                    *rects[i].f = *rects[i].f; // noop
                    rects[i].f->focused = true;
                    SDL_StartTextInput();
                    hitField = true;
                }
            }
            if (!hitField) {
                // check visible toggle
                if (t && mx>=90 && mx<110 && my>=visY && my<visY+20) {
                    t->setVisible(!t->visible);
                    return;
                }
                if (t && mx>=90 && mx<110 && my>=dragY && my<dragY+20) {
                    t->setDraggable(!t->draggable);
                    return;
                }
                // variables
                // find which var field
                int vy = dragY + 30 + 18; // variables header
                // Actually recompute: after draggable y+24+? let's approximate
                // We'll search by row height 36
                // simpler: check each var field rect
                for (size_t i=0;i<varIds.size();++i) {
                    // recompute y for var i
                }
                // unfocus all fields and vars
                fName.focused=fX.focused=fY.focused=fDir.focused=fSize.focused=false;
                varFocus = -1;
                for (auto& vf : varFields) vf.focused = false;
            }
            // variables hit
            // recompute var field positions
            {
                int yy = 44 + 18 + 120 + 8;
                yy += 18; // Properties header
                // 5 fields
                for (int i=0;i<5;++i) yy += 24;
                // visible
                yy += 24;
                // draggable
                yy += 24;
                yy += 6; // gap
                yy += 18; // Variables header
                for (size_t i=0;i<varIds.size();++i) {
                    int fy = yy + 14;
                    if (mx>=10 && mx<W-10 && my>=fy && my<fy+18) {
                        // unfocus props
                        fName.focused=fX.focused=fY.focused=fDir.focused=fSize.focused=false;
                        varFocus = (int)i;
                        for (size_t j=0;j<varFields.size();++j) varFields[j].focused = (j==i);
                        SDL_StartTextInput();
                        return;
                    }
                    yy += 36;
                }
            }
        // field hit already focused; if not hitField but clicked field handled above
        // Apply property fields on focus loss? We'll apply on Enter and continuously.
        applyFieldsToTarget();
        return;
    }
    if (ev.type == SDL_TEXTINPUT) {
        // route to focused field
        Field* f = nullptr;
        if (fName.focused) f=&fName;
        else if (fX.focused) f=&fX;
        else if (fY.focused) f=&fY;
        else if (fDir.focused) f=&fDir;
        else if (fSize.focused) f=&fSize;
        else if (varFocus>=0 && varFocus<(int)varFields.size()) f=&varFields[varFocus];
        if (f) {
            f->buf += ev.text.text;
        }
        return;
    }
    if (ev.type == SDL_KEYDOWN) {
        Field* f = nullptr;
        if (fName.focused) f=&fName;
        else if (fX.focused) f=&fX;
        else if (fY.focused) f=&fY;
        else if (fDir.focused) f=&fDir;
        else if (fSize.focused) f=&fSize;
        else if (varFocus>=0 && varFocus<(int)varFields.size()) f=&varFields[varFocus];
        if (!f) return;
        if (ev.key.keysym.sym == SDLK_BACKSPACE) {
            if (!f->buf.empty()) {
                // remove last utf8 char (simple: pop bytes until lead)
                while (!f->buf.empty() && (f->buf.back() & 0x80) && !(f->buf.back() & 0x40)) f->buf.pop_back();
                if (!f->buf.empty()) f->buf.pop_back();
            }
        } else if (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_TAB) {
            // apply
            applyFieldsToTarget();
            if (varFocus>=0) applyVarField(varFocus);
            // unfocus
            fName.focused=fX.focused=fY.focused=fDir.focused=fSize.focused=false;
            for (auto& vf: varFields) vf.focused=false;
            varFocus=-1;
            SDL_StopTextInput();
        } else if (ev.key.keysym.sym == SDLK_ESCAPE) {
            fName.focused=fX.focused=fY.focused=fDir.focused=fSize.focused=false;
            for (auto& vf: varFields) vf.focused=false;
            varFocus=-1;
            SDL_StopTextInput();
        }
        // apply property fields live
        applyFieldsToTarget();
        if (varFocus>=0) applyVarField(varFocus);
    }
};
}; // struct Editor

namespace xyo {
static std::vector<uint8_t> loadFile(const char* path, bool& ok) {
    std::ifstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) { ok = false; return {}; }
    ok = true;
    return {std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>()};
}

int editorMain(const char* path) {
    bool ok;
    std::vector<uint8_t> data = loadFile(path, ok);
    if (!ok) { std::fprintf(stderr, "cannot read %s\n", path); return 1; }
    Runtime rt;
    Sb3LoadResult r = loadSb3File(rt, data.data(), data.size());
    if (!r.ok) { std::fprintf(stderr, "load failed: %s\n", r.error.c_str()); return 1; }

    Editor ed;
    ed.rt = &rt;
    ed.title = path;
    // init stage renderer
    if (!ed.ren.init(rt, 2)) {
        std::fprintf(stderr, "renderer init failed\n");
        return 1;
    }
    ed.ren.setTitle(("xyo editor — " + std::string(path)).c_str());

    // inspector window
    ed.inspWin = SDL_CreateWindow("xyo Editor — Inspector",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 360, 720,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    // place inspector to the right of the stage window
    if (ed.ren.window()) {
        int sx, sy; SDL_GetWindowPosition(ed.ren.window(), &sx, &sy);
        int sw, sh; SDL_GetWindowSize(ed.ren.window(), &sw, &sh);
        SDL_SetWindowPosition(ed.inspWin, sx + sw + 16, sy);
    }
    ed.inspRen = SDL_CreateRenderer(ed.inspWin, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ed.inspRen) {
        std::fprintf(stderr, "inspector renderer failed\n");
        return 1;
    }

    // initial sync
    ed.syncFieldsFromTarget();
    ed.fieldsSynced = true;

    using clock = std::chrono::steady_clock;
    const auto kFrame = std::chrono::microseconds(33333);
    int exitFrames = 0;
    if (const char* ef = std::getenv("XYO_EXIT_FRAMES")) exitFrames = std::atoi(ef);
    int frameCount = 0;
    while (!ed.quit) {
        if (exitFrames > 0 && frameCount >= exitFrames) { ed.quit = true; break; }
        ++frameCount;
        auto t0 = clock::now();
        SDL_Event ev;
        // stage window events
        // We poll all events; route by window id
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { ed.quit = true; break; }
            Uint32 stageWid = SDL_GetWindowID(ed.ren.ok() ? nullptr : nullptr);
            // We don't have accessor; use handleEvent for stage window events.
            // handleEvent returns false on quit request.
            // Determine if event belongs to stage window: windowID matches stage window.
            // SdlRenderer doesn't expose window_; but handleEvent internally checks? It processes all.
            // We'll call handleEvent for all events; it handles stage window.
            // However inspector events need separate handling.
            Uint32 inspWid = SDL_GetWindowID(ed.inspWin);
            if (ev.type == SDL_WINDOWEVENT || ev.type == SDL_MOUSEBUTTONDOWN ||
                ev.type == SDL_MOUSEBUTTONUP || ev.type == SDL_MOUSEMOTION ||
                ev.type == SDL_TEXTINPUT || ev.type == SDL_KEYDOWN ||
                ev.type == SDL_MOUSEWHEEL) {
                if (ev.window.windowID == inspWid || (ev.type!=SDL_WINDOWEVENT && ev.type!=SDL_TEXTINPUT && ev.type!=SDL_KEYDOWN && SDL_GetMouseFocus()==ed.inspWin)) {
                    // heuristic: if mouse focus is inspector
                }
            }
            // Route: if event windowID == inspWid -> inspector; else stage
            bool toInsp = false;
            if (ev.type == SDL_WINDOWEVENT) toInsp = (ev.window.windowID == inspWid);
            else if (ev.type == SDL_TEXTINPUT || ev.type == SDL_KEYDOWN) toInsp = (SDL_GetKeyboardFocus() == ed.inspWin);
            else {
                // mouse events: check windowID if present
                if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP || ev.type == SDL_MOUSEMOTION || ev.type == SDL_MOUSEWHEEL) {
                    // SDL mouse events have windowID in .window? motion has .motion.windowID
                    Uint32 wid = 0;
                    if (ev.type == SDL_MOUSEMOTION) wid = ev.motion.windowID;
                    else if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP) wid = ev.button.windowID;
                    else if (ev.type == SDL_MOUSEWHEEL) wid = ev.wheel.windowID;
                    toInsp = (wid == inspWid);
                }
            }
            if (toInsp) {
                ed.handleInspectorEvent(ev);
            } else {
                if (!ed.ren.handleEvent(ev)) { ed.quit = true; break; }
            }
        }
        if (ed.quit) break;

        // sync selection changes
        if (ed.lastSel != ed.selTarget) {
            ed.lastSel = ed.selTarget;
            ed.syncFieldsFromTarget();
            ed.fieldsSynced = true;
        }

        // step runtime
        rt.step();

        // draw inspector
        ed.drawInspector();

        // pacing
        auto dt = clock::now() - t0;
        if (dt < kFrame) std::this_thread::sleep_for(kFrame - dt);
    }

    // cleanup
    if (ed.inspRen) SDL_DestroyRenderer(ed.inspRen);
    if (ed.inspWin) SDL_DestroyWindow(ed.inspWin);
    return 0;
}
} // namespace xyo

#else
namespace xyo {
int editorMain(const char* path) {
    std::fprintf(stderr, "editor: %s (this build has no SDL2)\n", path);
    return 1;
}
} // namespace xyo
#endif
