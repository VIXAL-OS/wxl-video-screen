#pragma once
#include "FilmQuad.h"
#include <wxl/services/camera.h>
#include <vector>

namespace WxlFilm {
struct CameraView { wxl_vec3 eye, forward, up; double fov; };
class Screen {
    QuadPipeline pipeline_;
    Microsoft::WRL::ComPtr<IDirect3DTexture9> texture_;
    IDirect3DDevice9* owner_ = nullptr;
    unsigned width_ = 0, height_ = 0;
    wxl_vec3 center_{}, right_{};
    bool placed_ = false, upload_ = true;
public:
    void releaseDevice();
    void hide() { placed_ = false; releaseDevice(); }
    bool place(const CameraView& camera);
    bool placed() const { return placed_; }
    const char* draw(IDirect3DDevice9* device, const CameraView& camera, float farClip,
                     const std::vector<unsigned char>& pixels, unsigned w, unsigned h,
                     bool changed, float aspect);
};
}

