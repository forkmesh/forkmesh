#include <metal_stdlib>
using namespace metal;

constexpr sampler fmStreamLinear(coord::normalized, address::clamp_to_edge, filter::linear);

struct FmStreamQuad {
    float4 position [[position]];
    float2 uv;
};

vertex FmStreamQuad fmFaceStreamQuadVertex(uint vid [[vertex_id]]) {
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    FmStreamQuad out;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    out.uv = float2(p.x, 1.0 - p.y);
    return out;
}

struct FmStreamUniforms {
    float4 pixelsMouth;
    float4 mouthAxes;
    float4 flags;
};

static float fmStreamGrain(float2 at, float seed) {
    return fract(sin(dot(floor(at) + float2(seed, seed * 0.61), float2(12.9898, 78.233))) * 43758.5453) - 0.5;
}

fragment float4 fmFaceStreamComposite(FmStreamQuad in [[stage_in]],
                                      texture2d<float> backdrop [[texture(0)]],
                                      texture2d<float> warp [[texture(1)]],
                                      texture2d<float> aux [[texture(2)]],
                                      constant FmStreamUniforms& u [[buffer(0)]]) {
    float3 color = backdrop.sample(fmStreamLinear, in.uv).rgb;
    if (u.flags.y > 0.5) {
        float4 w = warp.sample(fmStreamLinear, in.uv);
        float4 a = aux.sample(fmStreamLinear, in.uv);
        float hole = min(a.b * 2.0, 1.0);
        float cavity = max(a.b * 2.0 - 1.0, 0.0);
        float2 pixels = u.pixelsMouth.xy;
        float2 d = in.uv * pixels - u.pixelsMouth.zw;
        float2 q = float2(dot(d, u.mouthAxes.xy), dot(d, u.mouthAxes.zw));
        float3 inside = mix(float3(0.32, 0.12, 0.12), float3(0.09, 0.03, 0.04), smoothstep(0.0, 1.0, cavity));
        float teeth = (1.0 - smoothstep(-0.5, -0.15, q.y)) * (1.0 - smoothstep(0.45, 0.85, abs(q.x)));
        inside = mix(inside, float3(0.86, 0.84, 0.79), teeth * 0.85);
        float lower = smoothstep(0.55, 0.9, q.y) * (1.0 - smoothstep(0.3, 0.65, abs(q.x)));
        inside = mix(inside, float3(0.76, 0.74, 0.69), lower * 0.3);
        float tongue = u.flags.x * (1.0 - smoothstep(0.55, 1.0, length((q - float2(0.0, 0.55)) / float2(0.75, 0.6))));
        inside = mix(inside, float3(0.78, 0.36, 0.4), tongue);
        color = mix(color, inside, hole);
        color *= 1.0 - 0.22 * hole * (1.0 - hole) * 4.0;
        float3 face = saturate(w.rgb + fmStreamGrain(in.uv * pixels, u.flags.z) * 0.008);
        color = mix(color, face, w.a);
    }
    return float4(color, 1.0);
}
