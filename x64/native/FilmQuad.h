#pragma once
#include <d3d9.h>
#include <wrl/client.h>

namespace WxlFilm {
struct Vertex { float x, y, z, u, v; };
class QuadPipeline {
    IDirect3DDevice9* owner_ = nullptr;
    Microsoft::WRL::ComPtr<IDirect3DVertexShader9> shader_;
    Microsoft::WRL::ComPtr<IDirect3DVertexDeclaration9> declaration_;
public:
    void reset();
    bool draw(IDirect3DDevice9* device, IDirect3DTexture9* texture,
              const D3DMATRIX& viewProjection, const Vertex (&corners)[4]);
};
}
