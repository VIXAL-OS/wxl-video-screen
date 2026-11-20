#include "FilmQuad.h"
#include "FilmVertexBytecode.h"
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

namespace WxlFilm {
void QuadPipeline::reset() {
    shader_.Reset(); declaration_.Reset(); owner_ = nullptr;
}
bool QuadPipeline::draw(IDirect3DDevice9* d, IDirect3DTexture9* texture,
              const D3DMATRIX& projection, const Vertex (&v)[4]) {
    if (!d || !texture) return false;
    if (owner_ != d) {
        reset();
        const D3DVERTEXELEMENT9 elements[] = {
            {0,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
            {0,16,D3DDECLTYPE_FLOAT2,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_TEXCOORD,0}, D3DDECL_END()
        };
        if (FAILED(d->CreateVertexShader(reinterpret_cast<const DWORD*>(filmVertexBytecode), &shader_)) ||
            FAILED(d->CreateVertexDeclaration(elements, &declaration_))) { reset(); return false; }
        owner_ = d;
    }
    struct ClipVertex { float clip[4]; float u, v; } clip[4];
    for (int i=0; i<4; ++i) {
        for (int j=0; j<4; ++j)
            clip[i].clip[j] = v[i].x*projection.m[0][j] + v[i].y*projection.m[1][j] +
                              v[i].z*projection.m[2][j] + projection.m[3][j];
        clip[i].u=v[i].u; clip[i].v=v[i].v;
    }
    ComPtr<IDirect3DStateBlock9> state;
    ComPtr<IDirect3DVertexBuffer9> stream;
    UINT offset, stride;
    if (FAILED(d->CreateStateBlock(D3DSBT_ALL, &state)) || FAILED(state->Capture()) ||
        FAILED(d->GetStreamSource(0, &stream, &offset, &stride))) return false;

    bool ok = true;
    auto check = [&](HRESULT hr) { if (FAILED(hr)) ok = false; };
    check(d->SetVertexShader(shader_.Get())); check(d->SetPixelShader(nullptr));
    check(d->SetVertexDeclaration(declaration_.Get()));
    check(d->SetStreamSourceFreq(0, 1));
    struct State { D3DRENDERSTATETYPE id; DWORD value; };
    const State states[] = {
        {D3DRS_ZENABLE,D3DZB_TRUE},{D3DRS_ZWRITEENABLE,TRUE},{D3DRS_ZFUNC,D3DCMP_LESSEQUAL},
        {D3DRS_DEPTHBIAS,0},{D3DRS_SLOPESCALEDEPTHBIAS,0},{D3DRS_CULLMODE,D3DCULL_NONE},
        {D3DRS_LIGHTING,FALSE},{D3DRS_FOGENABLE,FALSE},{D3DRS_STENCILENABLE,FALSE},
        {D3DRS_ALPHABLENDENABLE,FALSE},{D3DRS_ALPHATESTENABLE,FALSE},{D3DRS_CLIPPLANEENABLE,0},
        {D3DRS_CLIPPING,TRUE},{D3DRS_FILLMODE,D3DFILL_SOLID},{D3DRS_COLORWRITEENABLE,15},
        {D3DRS_SRGBWRITEENABLE,FALSE},{D3DRS_VERTEXBLEND,D3DVBF_DISABLE},{D3DRS_INDEXEDVERTEXBLENDENABLE,FALSE}
    };
    for (auto s : states) check(d->SetRenderState(s.id, s.value));
    check(d->SetTexture(0, texture));
    check(d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1));
    check(d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
    check(d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1));
    check(d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR));
    check(d->SetRenderState(D3DRS_TEXTUREFACTOR, 0xffffffff));
    check(d->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE));
    check(d->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0));
    check(d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE));
    check(d->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE));
    check(d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR));
    check(d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR));
    check(d->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE));
    check(d->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
    check(d->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
    check(d->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE));
    if (ok) check(d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, clip, sizeof(ClipVertex)));
    check(state->Apply());
    check(d->SetStreamSource(0, stream.Get(), offset, stride));
    return ok;
}
}
