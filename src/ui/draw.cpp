#include "draw.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#ifdef XYO_HAVE_SDL2
#include <SDL.h>
#if SDL_VERSION_ATLEAST(2, 0, 18)
#define XYO_UI_SDL_GEOMETRY 1
#endif
#endif

namespace xyo::ui {
namespace {
constexpr float pi = 3.14159265358979323846f;
bool finite(Vec2 p) { return std::isfinite(p.x) && std::isfinite(p.y); }
Vec2 sub(Vec2 a, Vec2 b) { return {a.x-b.x, a.y-b.y}; }
Vec2 add(Vec2 a, Vec2 b) { return {a.x+b.x, a.y+b.y}; }
Vec2 mul(Vec2 a, float b) { return {a.x*b, a.y*b}; }
double cross(Vec2 a, Vec2 b, Vec2 c) {
    return (double(b.x)-a.x)*(double(c.y)-a.y) -
           (double(b.y)-a.y)*(double(c.x)-a.x);
}
bool same(Vec2 a, Vec2 b) {
    return std::abs(double(a.x)-b.x) <= 1e-5 && std::abs(double(a.y)-b.y) <= 1e-5;
}
Vec2 normal(Vec2 a, Vec2 b) {
    double dx = double(b.x)-a.x, dy = double(b.y)-a.y;
    double len = std::hypot(dx, dy);
    return len > 1e-10 ? Vec2{float(dy/len),float(-dx/len)} : Vec2{};
}
Vec2 joinNormal(Vec2 a, Vec2 b) {
    Vec2 n = mul(add(a,b), .5f);
    float d = n.x*n.x+n.y*n.y;
    // Limit miters at acute/reversing joins, avoiding unbounded spikes.
    return d > 1e-8f ? mul(n, std::min(1.f/d, 4.f)) : b;
}
double area(const std::vector<Vec2>& p) {
    double a=0;
    for (std::size_t i=1; i+1<p.size(); ++i) a += cross(p[0],p[i],p[i+1]);
    return a;
}
int segmentCount(float radius, int requested) {
    if (requested > 0) return std::clamp(requested, 3, 4096);
    // At most ~0.25px chord error, with a bounded amount of work.
    double angle = std::acos(std::clamp(1.0-.25/double(radius), -1.0, 1.0));
    return std::clamp(int(std::ceil(pi/std::max(angle, .001))), 12, 512);
}
}

void DrawList::begin(float w, float h) {
    vertices_.clear(); indices_.clear(); commands_.clear(); clips_.clear();
    // SDL clip rectangles have signed integer coordinates.
    constexpr float limit = 1000000000.f;
    bounds_ = {0, 0, std::isfinite(w) ? std::clamp(w,0.f,limit) : 0,
                      std::isfinite(h) ? std::clamp(h,0.f,limit) : 0};
    clips_.push_back(bounds_);
}
bool DrawList::drawable(PackedColor c) const {
    return (c >> 24) && !clips_.empty() && clips_.back().x1 > clips_.back().x0 &&
           clips_.back().y1 > clips_.back().y0;
}
bool DrawList::reserveGeometry(std::size_t nv, std::size_t ni) const {
    const auto max = std::size_t(std::numeric_limits<int>::max());
    return nv <= max && ni <= max && vertices_.size() <= max-nv && indices_.size() <= max-ni;
}
void DrawList::vertex(Vec2 p, PackedColor c, Vec2 uv) {
    vertices_.push_back({p,{std::uint8_t(c),std::uint8_t(c>>8),std::uint8_t(c>>16),std::uint8_t(c>>24)},uv});
}
void DrawList::record(TextureID texture, std::size_t first, std::size_t vertexFirst) {
    const std::size_t count = indices_.size()-first;
    if (!count) return;
    const bool merge = !commands_.empty() && commands_.back().texture == texture &&
        commands_.back().clip == clips_.back() &&
        commands_.back().first+commands_.back().count == first &&
        commands_.back().vertexFirst+commands_.back().vertexCount == vertexFirst;
    const std::size_t base = merge ? commands_.back().vertexFirst : vertexFirst;
    // Geometry generation uses frame-global indices. Rebase only this new
    // segment once, so rendering/replaying never copies or mutates the frame.
    // In particular, SDL2-compat converts ALL num_vertices colors for every
    // RenderGeometryRaw call: passing the entire frame here is O(V * batches).
    if (base != 0)
        for (std::size_t i=first;i<indices_.size();++i) indices_[i] -= int(base);
    if (merge) {
        commands_.back().count += count;
        commands_.back().vertexCount = vertices_.size()-base;
    } else {
        commands_.push_back({texture,clips_.back(),first,count,base,vertices_.size()-base});
    }
}
void DrawList::PushClipRect(Vec2 min, Vec2 max, bool intersect) {
    Clip c = bounds_;
    if (!finite(min) || !finite(max)) c = {0,0,0,0};
    else {
        c.x0=std::clamp(min.x,bounds_.x0,bounds_.x1);
        c.y0=std::clamp(min.y,bounds_.y0,bounds_.y1);
        c.x1=std::clamp(max.x,c.x0,bounds_.x1);
        c.y1=std::clamp(max.y,c.y0,bounds_.y1);
        if (intersect && !clips_.empty()) {
            const Clip& p=clips_.back();
            c.x0=std::max(c.x0,p.x0); c.y0=std::max(c.y0,p.y0);
            c.x1=std::max(c.x0,std::min(c.x1,p.x1));
            c.y1=std::max(c.y0,std::min(c.y1,p.y1));
        }
    }
    clips_.push_back(c);
}
void DrawList::PopClipRect() { if (clips_.size()>1) clips_.pop_back(); }

bool DrawList::cleanPolygon(const Vec2* points, int count) {
    clean_.clear();
    if (!points || count<3) return false;
    float x0=points[0].x, x1=x0, y0=points[0].y, y1=y0;
    for (int i=0;i<count;++i) {
        if (!finite(points[i])) return false;
        x0=std::min(x0,points[i].x); x1=std::max(x1,points[i].x);
        y0=std::min(y0,points[i].y); y1=std::max(y1,points[i].y);
    }
    const Clip& clip=clips_.back();
    // Reject fully clipped blocks before duplicate filtering / ear clipping.
    if (x1+1<clip.x0 || y1+1<clip.y0 || x0-1>clip.x1 || y0-1>clip.y1) return false;
    for (int i=0;i<count;++i)
        if (clean_.empty() || !same(clean_.back(),points[i])) clean_.push_back(points[i]);
    if (clean_.size()>1 && same(clean_.front(),clean_.back())) clean_.pop_back();
    // Remove zero-length edges and collinear points, including A-B-A spikes.
    bool changed=true;
    while (changed && clean_.size()>=3) {
        changed=false;
        for (std::size_t i=0;i<clean_.size();++i) {
            Vec2 a=clean_[(i+clean_.size()-1)%clean_.size()];
            Vec2 b=clean_[i], c=clean_[(i+1)%clean_.size()];
            double scale=std::hypot(double(c.x)-a.x,double(c.y)-a.y);
            if (same(a,b) || same(a,c) || std::abs(cross(a,b,c))<=1e-7*std::max(1.0,scale)) {
                clean_.erase(clean_.begin()+std::ptrdiff_t(i)); changed=true; break;
            }
        }
    }
    return clean_.size()>=3 && std::abs(area(clean_))>1e-10;
}

void DrawList::polygonFringe(PackedColor color) {
    const std::size_t n=clean_.size();
    if (!reserveGeometry(n*2,n*6)) return;
    const float direction=area(clean_)>0 ? 1.f : -1.f;
    int base=int(vertices_.size());
    for (std::size_t i=0;i<n;++i) {
        Vec2 a=clean_[(i+n-1)%n], b=clean_[i], c=clean_[(i+1)%n];
        Vec2 offset=mul(joinNormal(normal(a,b),normal(b,c)),direction);
        vertex(b,color); vertex(add(b,offset),color&0x00ffffffu);
    }
    for (std::size_t i=0;i<n;++i) {
        int a=base+int(i)*2, b=base+int((i+1)%n)*2;
        indices_.insert(indices_.end(),{a,b,b+1,a,b+1,a+1});
    }
}
void DrawList::AddConcavePolyFilled(const Vec2* p, int count, PackedColor color) {
    if (!drawable(color) || !cleanPolygon(p,count)) return;
    const int n=int(clean_.size());
    remaining_.clear(); triangles_.clear();
    remaining_.reserve(n);
    for (int i=0;i<n;++i) remaining_.push_back(i);
    const double direction=area(clean_)>0 ? 1 : -1;
    while (remaining_.size()>3) {
        bool found=false;
        for (std::size_t i=0;i<remaining_.size();++i) {
            int ia=remaining_[(i+remaining_.size()-1)%remaining_.size()];
            int ib=remaining_[i], ic=remaining_[(i+1)%remaining_.size()];
            Vec2 a=clean_[ia], b=clean_[ib], c=clean_[ic];
            if (direction*cross(a,b,c)<=1e-10) continue;
            bool blocked=false;
            for (int id:remaining_) {
                if (id==ia || id==ib || id==ic) continue;
                Vec2 q=clean_[id];
                // Repeated nonadjacent coordinates can occur at touching notches.
                if (same(q,a) || same(q,b) || same(q,c)) continue;
                if (direction*cross(a,b,q)>=-1e-10 && direction*cross(b,c,q)>=-1e-10 &&
                    direction*cross(c,a,q)>=-1e-10) { blocked=true; break; }
            }
            if (blocked) continue;
            triangles_.insert(triangles_.end(),{ia,ib,ic});
            remaining_.erase(remaining_.begin()+std::ptrdiff_t(i)); found=true; break;
        }
        if (!found) return; // Never fill a failed concave triangulation with a fan.
    }
    triangles_.insert(triangles_.end(),remaining_.begin(),remaining_.end());
    if (!reserveGeometry(clean_.size()*3,triangles_.size()+clean_.size()*6)) return;
    const std::size_t first=indices_.size();
    int base=int(vertices_.size());
    for (Vec2 q:clean_) vertex(q,color);
    for (int id:triangles_) indices_.push_back(base+id);
    polygonFringe(color);
    record(0,first,std::size_t(base));
}
void DrawList::AddConvexPolyFilled(const Vec2* p, int count, PackedColor color) {
    if (!drawable(color) || !cleanPolygon(p,count)) return;
    std::size_t n=clean_.size();
    if (!reserveGeometry(n*3,(n-2)*3+n*6)) return;
    auto first=indices_.size(); int base=int(vertices_.size());
    for (Vec2 q:clean_) vertex(q,color);
    for (std::size_t i=1;i+1<n;++i)
        indices_.insert(indices_.end(),{base,base+int(i),base+int(i+1)});
    polygonFringe(color); record(0,first,std::size_t(base));
}
void DrawList::AddTriangleFilled(Vec2 a, Vec2 b, Vec2 c, PackedColor color) {
    Vec2 points[]={a,b,c}; AddConvexPolyFilled(points,3,color);
}

void DrawList::AddPolyline(const Vec2* points, int count, PackedColor color, int flags, float thickness) {
    if (!drawable(color) || !points || count<2 || !std::isfinite(thickness) || thickness<=0) return;
    clean_.clear();
    for (int i=0;i<count;++i) {
        if (!finite(points[i])) return;
        if (clean_.empty() || !same(clean_.back(),points[i])) clean_.push_back(points[i]);
    }
    bool closed=(flags&DrawFlags_Closed)!=0;
    if (closed && clean_.size()>1 && same(clean_.front(),clean_.back())) clean_.pop_back();
    std::size_t n=clean_.size();
    if (n<2) return;
    if (n==2) closed=false;
    std::size_t segments=closed?n:n-1;
    if (!reserveGeometry(n*4,segments*18)) return;
    offsets_.resize(n);
    for (std::size_t i=0;i<n;++i) {
        Vec2 prev=normal(clean_[(i+n-1)%n],clean_[i]);
        Vec2 next=normal(clean_[i],clean_[(i+1)%n]);
        offsets_[i]=!closed && i==0 ? next : !closed && i==n-1 ? prev : joinNormal(prev,next);
    }
    const float inner=std::max(0.f,(thickness-1)*.5f), outer=(thickness+1)*.5f;
    PackedColor core=(color&0x00ffffffu) |
        (std::uint32_t(float(color>>24)*std::min(1.f,thickness))<<24);
    auto first=indices_.size(); int base=int(vertices_.size());
    for (std::size_t i=0;i<n;++i) {
        Vec2 q=clean_[i], off=offsets_[i];
        vertex(add(q,mul(off,outer)),color&0x00ffffffu);
        vertex(add(q,mul(off,inner)),core);
        vertex(sub(q,mul(off,inner)),core);
        vertex(sub(q,mul(off,outer)),color&0x00ffffffu);
    }
    for (std::size_t i=0;i<segments;++i) {
        int a=base+int(i)*4, b=base+int((i+1)%n)*4;
        for (int k=0;k<3;++k) indices_.insert(indices_.end(),{a+k,b+k,b+k+1,a+k,b+k+1,a+k+1});
    }
    record(0,first,std::size_t(base));
}
void DrawList::AddLine(Vec2 a, Vec2 b, PackedColor color, float thickness) {
    Vec2 p[]={a,b}; AddPolyline(p,2,color,0,thickness);
}
void DrawList::roundedRectPath(Vec2 min, Vec2 max, float rounding) {
    path_.clear();
    float r=std::clamp(rounding,0.f,std::min(max.x-min.x,max.y-min.y)*.5f);
    if (r<=0) { path_={min,{max.x,min.y},max,{min.x,max.y}}; return; }
    int steps=std::max(2,(segmentCount(r,0)+3)/4);
    Vec2 centers[]={{max.x-r,min.y+r},{max.x-r,max.y-r},{min.x+r,max.y-r},{min.x+r,min.y+r}};
    for (int corner=0;corner<4;++corner)
        for (int i=0;i<=steps;++i) {
            float a=(-.5f+corner*.5f+float(i)/steps*.5f)*pi;
            path_.push_back({centers[corner].x+std::cos(a)*r,centers[corner].y+std::sin(a)*r});
        }
}
void DrawList::AddRectFilled(Vec2 min, Vec2 max, PackedColor color, float rounding) {
    if (!drawable(color) || !finite(min) || !finite(max) || max.x<=min.x || max.y<=min.y || !std::isfinite(rounding)) return;
    if (rounding<=0) { AddImage(0,min,max,{0,0},{0,0},color); return; }
    roundedRectPath(min,max,rounding);
    AddConvexPolyFilled(path_.data(),int(path_.size()),color);
}
void DrawList::AddRect(Vec2 min, Vec2 max, PackedColor color, float rounding, int flags, float thickness) {
    (void)flags; // Rectangles are intrinsically closed; no per-corner flags.
    if (!drawable(color) || !finite(min) || !finite(max) || max.x<=min.x || max.y<=min.y || !std::isfinite(rounding)) return;
    roundedRectPath(min,max,rounding);
    AddPolyline(path_.data(),int(path_.size()),color,DrawFlags_Closed,thickness);
}
void DrawList::circlePath(Vec2 center, float radius, int segments) {
    path_.clear();
    int n=segmentCount(radius,segments);
    for (int i=0;i<n;++i) {
        float a=2*pi*float(i)/n;
        path_.push_back({center.x+std::cos(a)*radius,center.y+std::sin(a)*radius});
    }
}
void DrawList::AddCircleFilled(Vec2 center, float radius, PackedColor color, int segments) {
    if (!drawable(color) || !finite(center) || !std::isfinite(radius) || radius<=0) return;
    circlePath(center,radius,segments); AddConvexPolyFilled(path_.data(),int(path_.size()),color);
}
void DrawList::AddCircle(Vec2 center, float radius, PackedColor color, int segments, float thickness) {
    if (!drawable(color) || !finite(center) || !std::isfinite(radius) || radius<=0) return;
    circlePath(center,radius,segments); AddPolyline(path_.data(),int(path_.size()),color,DrawFlags_Closed,thickness);
}
void DrawList::AddImage(TextureID texture, Vec2 min, Vec2 max, Vec2 uv0, Vec2 uv1, PackedColor color) {
    if (!drawable(color) || !finite(min) || !finite(max) || !finite(uv0) || !finite(uv1) ||
        min.x==max.x || min.y==max.y || !reserveGeometry(4,6)) return;
    const Clip& c=clips_.back();
    // Clip the quad (and its texture coordinates) here instead of leaving it to
    // SDL: the software renderer corrupts textured geometry that straddles the
    // clip rect (SDL 2.30; Ubuntu 24.04 ships it). Clipping an axis-aligned quad
    // is exact, and the recorded batch then never crosses its own clip.
    const float xa=std::clamp(std::min(min.x,max.x),c.x0,c.x1);
    const float xb=std::clamp(std::max(min.x,max.x),c.x0,c.x1);
    const float ya=std::clamp(std::min(min.y,max.y),c.y0,c.y1);
    const float yb=std::clamp(std::max(min.y,max.y),c.y0,c.y1);
    if (xb<=xa || yb<=ya) return;
    const float su=(uv1.x-uv0.x)/(max.x-min.x);
    const float sv=(uv1.y-uv0.y)/(max.y-min.y);
    const Vec2 ta{xa,ya}, tb{xb,yb};
    const Vec2 ua{uv0.x+(xa-min.x)*su,uv0.y+(ya-min.y)*sv};
    const Vec2 ub{uv0.x+(xb-min.x)*su,uv0.y+(yb-min.y)*sv};
    auto first=indices_.size(); int base=int(vertices_.size());
    vertex(ta,color,ua); vertex({tb.x,ta.y},color,{ub.x,ua.y});
    vertex(tb,color,ub); vertex({ta.x,tb.y},color,{ua.x,ub.y});
    indices_.insert(indices_.end(),{base,base+1,base+2,base,base+2,base+3}); record(texture,first,std::size_t(base));
}

bool DrawList::render(SDL_Renderer* renderer) {
#ifdef XYO_UI_SDL_GEOMETRY
    if (!renderer) return false;
    SDL_Rect oldClip{};
    SDL_RenderGetClipRect(renderer,&oldClip);
    SDL_bool oldClipEnabled=SDL_RenderIsClipEnabled(renderer);
    SDL_BlendMode oldBlend=SDL_BLENDMODE_NONE;
    if (SDL_GetRenderDrawBlendMode(renderer,&oldBlend)!=0) return false;
    bool ok=SDL_SetRenderDrawBlendMode(renderer,SDL_BLENDMODE_BLEND)==0;
    for (const Command& command:commands_) {
        const Clip& c=command.clip;
        int x=int(std::floor(c.x0)), y=int(std::floor(c.y0));
        SDL_Rect clip{x,y,int(std::ceil(c.x1))-x,int(std::ceil(c.y1))-y};
        if (SDL_RenderSetClipRect(renderer,&clip)!=0) { ok=false; break; }
        SDL_Texture* texture=reinterpret_cast<SDL_Texture*>(command.texture);
        // Images may deliberately use custom/premultiplied blend modes; preserve
        // those. Font atlases configure straight-alpha blending when created.
        const Vertex* v=vertices_.data()+command.vertexFirst;
        if (SDL_RenderGeometryRaw(renderer,texture,&v[0].pos.x,sizeof(Vertex),
                reinterpret_cast<const SDL_Color*>(&v[0].color),sizeof(Vertex),
                &v[0].uv.x,sizeof(Vertex),int(command.vertexCount),
                indices_.data()+command.first,int(command.count),sizeof(int))!=0) {
            ok=false; break;
        }
    }
    if (SDL_SetRenderDrawBlendMode(renderer,oldBlend)!=0) ok=false;
    if (SDL_RenderSetClipRect(renderer,oldClipEnabled?&oldClip:nullptr)!=0) ok=false;
    return ok;
#else
    (void)renderer;
    return false;
#endif
}

} // namespace xyo::ui
