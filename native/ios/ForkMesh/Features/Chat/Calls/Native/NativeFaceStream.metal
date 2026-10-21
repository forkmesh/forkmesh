#include <metal_stdlib>
using namespace metal;

constexpr sampler fmStreamLinear(coord::normalized, address::clamp_to_edge, filter::linear);
constexpr sampler fmStreamMip(coord::normalized, address::clamp_to_edge, filter::linear, mip_filter::linear);

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

struct FmStreamWarpUniforms {
    float4 toBufferX;
    float4 toBufferY;
    float4 eyeL;
    float4 eyeR;
    float4 look;
    float4 close;
};

struct FmStreamWarpVarying {
    float4 position [[position]];
    float2 uv;
    float hole;
    float region;
};

struct FmStreamWarpTargets {
    float4 warp [[color(0)]];
    float4 aux [[color(1)]];
};

vertex FmStreamWarpVarying fmFaceStreamWarpVertex(uint vid [[vertex_id]],
                                                  const device float* vertices [[buffer(0)]],
                                                  constant FmStreamWarpUniforms& u [[buffer(1)]]) {
    const device float* v = vertices + vid * 7;
    float3 upright = float3(v[0], v[1], 1.0);
    float bx = dot(u.toBufferX.xyz, upright);
    float by = dot(u.toBufferY.xyz, upright);
    FmStreamWarpVarying out;
    float depth = clamp(-v[6] * 0.5, -0.99, 0.99) * 0.5 + 0.5;
    out.position = float4(bx / u.toBufferX.w * 2.0 - 1.0, 1.0 - by / u.toBufferY.w * 2.0, depth, 1.0);
    out.uv = float2(v[2], v[3]);
    out.hole = v[4];
    out.region = v[5];
    return out;
}

static float2 fmStreamGaze(float2 uv, float4 eye, float2 look) {
    float2 q = (uv - eye.xy) / eye.zw;
    float m = 1.0 - smoothstep(0.45, 1.0, length(q));
    return uv - look * eye.zw * m;
}

static float3 fmStreamLid(float3 color, float4 eye, float close, float2 at, texture2d<float> photo) {
    if (close < 0.01) return color;
    float2 q = (at - eye.xy) / eye.zw;
    float m = 1.0 - smoothstep(0.6, 1.05, length(q * float2(1.0, 0.8)));
    float3 skin = 0.5 * (photo.sample(fmStreamMip, float2(at.x, eye.y - eye.w * 1.25), bias(1.5)).rgb +
                         photo.sample(fmStreamMip, float2(at.x, eye.y + eye.w * 1.6), bias(1.5)).rgb);
    float lash = (1.0 - smoothstep(0.0, 0.22, abs(q.y - 0.3))) * (1.0 - smoothstep(0.5, 0.95, abs(q.x))) * 0.45;
    return mix(color, skin * (1.0 - lash), m * smoothstep(0.15, 0.85, close));
}

fragment FmStreamWarpTargets fmFaceStreamWarpFragment(FmStreamWarpVarying in [[stage_in]],
                                                      texture2d<float> photo [[texture(0)]],
                                                      texture2d<float> auxPhoto [[texture(1)]],
                                                      constant FmStreamWarpUniforms& u [[buffer(0)]]) {
    float3 aux = auxPhoto.sample(fmStreamMip, in.uv).rgb;
    float skin = auxPhoto.sample(fmStreamMip, in.uv, bias(2.5)).g;
    float open = 1.0 - smoothstep(0.05, 0.95, in.hole);
    float2 uv = fmStreamGaze(fmStreamGaze(in.uv, u.eyeL, u.look.xy), u.eyeR, u.look.zw);
    float3 color = photo.sample(fmStreamMip, uv).rgb;
    color = fmStreamLid(fmStreamLid(color, u.eyeL, u.close.x, in.uv, photo), u.eyeR, u.close.y, in.uv, photo);
    FmStreamWarpTargets out;
    out.warp = float4(color, aux.r * open);
    out.aux = float4(skin * aux.r * open, aux.b, 0.5 * (1.0 - open) + 0.5 * smoothstep(1.0, 2.0, in.hole), in.region);
    return out;
}

struct FmStreamMouthVarying {
    float4 position [[position]];
    float2 uvA;
    float2 uvB;
    float alpha;
};

struct FmStreamMouthUniforms {
    float4 size;
    float4 tiles;
    float4 gainA;
    float4 gainB;
};

vertex FmStreamMouthVarying fmFaceStreamMouthVertex(uint vid [[vertex_id]],
                                                    const device float* vertices [[buffer(0)]],
                                                    constant FmStreamMouthUniforms& u [[buffer(1)]]) {
    const device float* v = vertices + vid * 7;
    FmStreamMouthVarying out;
    out.position = float4(v[0] / u.size.x * 2.0 - 1.0, 1.0 - v[1] / u.size.y * 2.0, 0.0, 1.0);
    out.uvA = float2(v[2], v[3]);
    out.uvB = float2(v[4], v[5]);
    out.alpha = v[6];
    return out;
}

static float3 fmStreamTile(texture2d<float> atlas, float index, float count, float2 uv, float2 half_texel) {
    float2 q = clamp(uv, half_texel, float2(1.0) - half_texel);
    return atlas.sample(fmStreamLinear, float2((index + q.x) / count, q.y)).rgb;
}

fragment float4 fmFaceStreamMouthFragment(FmStreamMouthVarying in [[stage_in]],
                                          texture2d<float> atlas [[texture(0)]],
                                          constant FmStreamMouthUniforms& u [[buffer(0)]]) {
    float3 a = fmStreamTile(atlas, u.tiles.y, u.tiles.x, in.uvA, u.size.zw) * u.gainA.rgb;
    float3 b = fmStreamTile(atlas, u.tiles.z, u.tiles.x, in.uvB, u.size.zw) * u.gainB.rgb;
    return float4(saturate(mix(a, b, u.tiles.w)), in.alpha);
}

struct FmStreamUniforms {
    float4 center;
    float4 mouthAxes;
    float4 flags;
    float4 lip;
    float4 teeth;
};

static float fmStreamGrain(float2 at, float seed) {
    return fract(sin(dot(floor(at) + float2(seed, seed * 0.61), float2(12.9898, 78.233))) * 43758.5453) - 0.5;
}

static float3 fmStreamInterior(float2 p, float cavity, constant FmStreamUniforms& u) {
    float2 axis = u.mouthAxes.xy;
    float2 d = p - u.center.xy;
    float2 ay = float2(-axis.y, axis.x);
    float x = dot(d, axis) / u.mouthAxes.z;
    float y = dot(d, ay);
    float opening = u.mouthAxes.w;
    float span = u.flags.x;
    float top = y + opening;
    float bottom = opening - y;
    float gap = 2.0 * opening / span;
    float3 lip = u.lip.rgb;
    float3 teeth = u.teeth.rgb;
    float3 c = mix(lip * 0.3, lip * 0.07, smoothstep(0.0, 1.0, cavity));
    c *= mix(0.55, 1.0, smoothstep(0.0, 0.6, abs(x)));
    float floorShow = max(u.flags.y, 0.6 * smoothstep(0.18, 0.4, gap));
    float2 tq = float2(x / 0.7, (bottom - 0.3 * opening) / (0.55 * opening + 0.5));
    float tongue = (1.0 - smoothstep(0.5, 1.0, length(tq))) * floorShow;
    c = mix(c, lip * float3(0.95, 0.62, 0.66), tongue);
    float height = 0.12 * span * max(1.0 - 1.2 * x * x, 0.0);
    float upper = (1.0 - smoothstep(height * 0.75, height, top)) * (1.0 - smoothstep(0.6, 0.85, abs(x))) * smoothstep(0.04, 0.16, gap);
    float gaps = 1.0 - 0.15 * smoothstep(0.9, 1.0, cos(x * 18.85));
    float3 tooth = teeth * mix(0.55, 0.85, smoothstep(0.0, height + 0.01, top)) * (1.0 - 0.45 * x * x) * gaps;
    c = mix(c, tooth, upper);
    float lowHeight = 0.07 * span * max(1.0 - 1.6 * x * x, 0.0);
    float lower = (1.0 - smoothstep(lowHeight * 0.75, lowHeight, bottom)) * smoothstep(0.22, 0.4, gap) * (1.0 - smoothstep(0.45, 0.65, abs(x)));
    c = mix(c, teeth * 0.55, lower * 0.6);
    c *= mix(0.6, 1.0, smoothstep(0.0, 0.03 * span, top));
    return mix(lip * 0.62, c, smoothstep(0.03, 0.1, gap));
}

fragment float4 fmFaceStreamComposite(FmStreamQuad in [[stage_in]],
                                      texture2d<float> backdrop [[texture(0)]],
                                      texture2d<float> warp [[texture(1)]],
                                      texture2d<float> aux [[texture(2)]],
                                      texture2d<float> mouth [[texture(3)]],
                                      constant FmStreamUniforms& u [[buffer(0)]]) {
    float3 color = backdrop.sample(fmStreamLinear, in.uv).rgb;
    float2 pixels = u.center.zw;
    float2 p = in.uv * pixels;
    if (u.flags.z > 0.5) {
        float4 w = warp.sample(fmStreamLinear, in.uv);
        float4 a = aux.sample(fmStreamLinear, in.uv);
        float hole = min(a.b * 2.0, 1.0);
        float cavity = max(a.b * 2.0 - 1.0, 0.0);
        color = mix(color, fmStreamInterior(p, cavity, u), hole);
        color *= 1.0 - 0.18 * hole * (1.0 - hole) * 4.0;
        float3 face = saturate(w.rgb + fmStreamGrain(p, u.flags.w) * 0.006);
        color = mix(color, face, w.a);
        float on = u.lip.w;
        if (on > 0.001) {
            float4 m = mouth.sample(fmStreamLinear, in.uv);
            color = mix(color, m.rgb, m.a * on);
        }
    }
    return float4(color, 1.0);
}
