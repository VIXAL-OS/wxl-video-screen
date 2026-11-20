struct VertexOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };
VertexOut main(float4 pos : POSITION, float2 uv : TEXCOORD0) {
    VertexOut o;
    o.pos = pos; o.uv = uv;
    return o;
}
