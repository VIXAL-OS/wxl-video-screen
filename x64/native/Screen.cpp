#include "Screen.h"
#include <cmath>
#include <cstring>

namespace WxlFilm {
namespace {
wxl_vec3 cross(wxl_vec3 a, wxl_vec3 b) {
    return {a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x};
}
float length(wxl_vec3 v) { return std::sqrt(v.x*v.x+v.y*v.y+v.z*v.z); }
bool normalize(wxl_vec3& v) {
    float n = length(v);
    if (!std::isfinite(n) || n < 0.001f) return false;
    v = {v.x/n, v.y/n, v.z/n}; return true;
}
}
bool Screen::place(const CameraView& v) {
    wxl_vec3 direction = v.forward, right = {direction.y, -direction.x, 0};
    if (!normalize(direction) || !normalize(right)) return false;
    right_ = right;
    center_ = {v.eye.x + 12*direction.x, v.eye.y + 12*direction.y, v.eye.z + 12*direction.z};
    placed_ = true; return true;
}
void Screen::releaseDevice() {
    texture_.Reset(); pipeline_.reset(); owner_=nullptr; upload_=true;
}
const char* Screen::draw(IDirect3DDevice9* device, const CameraView& v, float farClip,
                         const std::vector<unsigned char>& pixels, unsigned w, unsigned h,
                         bool changed, float aspect) {
    if (!placed_ || !device || pixels.size()!=size_t(w)*h*4 || !w || !h) return "Waiting for video.";
    wxl_vec3 p = {center_.x-v.eye.x,center_.y-v.eye.y,center_.z-v.eye.z};
    if (changed) upload_=true;
    if (length(p)>160) return "Screen is beyond 160 yards.";
    D3DVIEWPORT9 viewport;
    if (FAILED(device->GetViewport(&viewport)) || !viewport.Width || !viewport.Height) return "Waiting for the world viewport.";
    wxl_vec3 forward=v.forward, right=cross(forward,v.up), up=cross(right,forward);
    if (!normalize(forward) || !normalize(right) || !normalize(up) || !(v.fov>0 && v.fov<3.15))
        return "Waiting for the world camera.";
    // The client's CSimpleCamera uses 0.6 * FOV vertically. At map load the near clip is 0.2.
    const float yScale = 1.f/std::tan(float(v.fov)*0.3f);
    const float xScale = yScale*float(viewport.Height)/float(viewport.Width);
    const float depth = farClip/(farClip-0.2f);
    D3DMATRIX projection{};
    projection._11=right.x*xScale; projection._21=right.y*xScale; projection._31=right.z*xScale;
    projection._12=up.x*yScale; projection._22=up.y*yScale; projection._32=up.z*yScale;
    projection._13=forward.x*depth; projection._23=forward.y*depth; projection._33=forward.z*depth;
    projection._14=forward.x; projection._24=forward.y; projection._34=forward.z; projection._43=-0.2f*depth;

    if (owner_!=device || width_!=w || height_!=h) releaseDevice();
    if (!texture_) {
        if (FAILED(device->CreateTexture(w,h,1,D3DUSAGE_DYNAMIC,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&texture_,nullptr)))
            return "Could not allocate the video texture.";
        owner_=device; width_=w; height_=h; upload_=true;
    }
    if (upload_) {
        D3DLOCKED_RECT lock{};
        if (FAILED(texture_->LockRect(0,&lock,nullptr,D3DLOCK_DISCARD))) return "Waiting to upload the frame.";
        for (unsigned y=0; y<h; ++y)
            std::memcpy(static_cast<char*>(lock.pBits)+size_t(y)*lock.Pitch,pixels.data()+size_t(y)*w*4,w*4);
        if (FAILED(texture_->UnlockRect(0))) { releaseDevice(); return "Video upload failed."; }
        upload_=false;
    }
    float rx=4*right_.x, ry=4*right_.y, rz=4/aspect;
    Vertex quad[]={{p.x-rx,p.y-ry,p.z+rz,0,0},{p.x+rx,p.y+ry,p.z+rz,1,0},
                   {p.x-rx,p.y-ry,p.z-rz,0,1},{p.x+rx,p.y+ry,p.z-rz,1,1}};
    return pipeline_.draw(device,texture_.Get(),projection,quad) ? "" : "World-screen draw failed.";
}
}

