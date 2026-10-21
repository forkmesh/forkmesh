#include <metal_stdlib>
using namespace metal;

constexpr sampler fmFaceLinear(coord::normalized, address::clamp_to_edge, filter::linear);
constexpr sampler fmFaceMip(coord::normalized, address::clamp_to_edge, filter::linear, mip_filter::linear);
constant float3 fmFaceLuma = float3(0.299, 0.587, 0.114);

struct FmFaceQuad {
    float4 position [[position]];
    float2 uv;
};

vertex FmFaceQuad fmFaceQuadVertex(uint vid [[vertex_id]]) {
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    FmFaceQuad out;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    out.uv = float2(p.x, 1.0 - p.y);
    return out;
}

struct FmFaceCameraUniforms {
    float4 range;
    float4 matrix;
};

fragment float4 fmFaceCameraYuv(FmFaceQuad in [[stage_in]],
                                texture2d<float> luma [[texture(0)]],
                                texture2d<float> chroma [[texture(1)]],
                                constant FmFaceCameraUniforms& u [[buffer(0)]]) {
    float y = (luma.sample(fmFaceLinear, in.uv).r - u.range.x) * u.range.y;
    float2 c = (chroma.sample(fmFaceLinear, in.uv).rg - u.range.z) * u.range.w;
    float3 rgb = float3(y + u.matrix.x * c.y, y - u.matrix.y * c.x - u.matrix.z * c.y, y + u.matrix.w * c.x);
    return float4(saturate(rgb), 1.0);
}

fragment float4 fmFaceCameraRgb(FmFaceQuad in [[stage_in]], texture2d<float> source [[texture(0)]]) {
    return float4(source.sample(fmFaceLinear, in.uv).rgb, 1.0);
}

struct FmFaceWarpUniforms {
    float4 toBufferX;
    float4 toBufferY;
    float4 eyeL;
    float4 eyeR;
    float4 look;
    float4 close;
};

struct FmFaceWarpVarying {
    float4 position [[position]];
    float2 uv;
    float hole;
    float region;
};

struct FmFaceWarpTargets {
    float4 warp [[color(0)]];
    float4 aux [[color(1)]];
};

vertex FmFaceWarpVarying fmFaceWarpVertex(uint vid [[vertex_id]],
                                          const device float* vertices [[buffer(0)]],
                                          constant FmFaceWarpUniforms& u [[buffer(1)]]) {
    const device float* v = vertices + vid * 7;
    float3 upright = float3(v[0], v[1], 1.0);
    float bx = dot(u.toBufferX.xyz, upright);
    float by = dot(u.toBufferY.xyz, upright);
    FmFaceWarpVarying out;
    float depth = clamp(-v[6] * 0.5, -0.99, 0.99) * 0.5 + 0.5;
    out.position = float4(bx / u.toBufferX.w * 2.0 - 1.0, 1.0 - by / u.toBufferY.w * 2.0, depth, 1.0);
    out.uv = float2(v[2], v[3]);
    out.hole = v[4];
    out.region = v[5];
    return out;
}

static float2 fmFaceGaze(float2 uv, float4 eye, float2 look) {
    float2 q = (uv - eye.xy) / eye.zw;
    float m = 1.0 - smoothstep(0.45, 1.0, length(q));
    return uv - look * eye.zw * m;
}

static float3 fmFaceLid(float3 color, float4 eye, float close, float2 at, texture2d<float> photo) {
    if (close < 0.01) return color;
    float2 q = (at - eye.xy) / eye.zw;
    float m = 1.0 - smoothstep(0.6, 1.05, length(q * float2(1.0, 0.8)));
    float3 skin = 0.5 * (photo.sample(fmFaceMip, float2(at.x, eye.y - eye.w * 1.25), bias(1.5)).rgb +
                         photo.sample(fmFaceMip, float2(at.x, eye.y + eye.w * 1.6), bias(1.5)).rgb);
    float lash = (1.0 - smoothstep(0.0, 0.22, abs(q.y - 0.3))) * (1.0 - smoothstep(0.5, 0.95, abs(q.x))) * 0.45;
    return mix(color, skin * (1.0 - lash), m * smoothstep(0.15, 0.85, close));
}

fragment FmFaceWarpTargets fmFaceWarpFragment(FmFaceWarpVarying in [[stage_in]],
                                              texture2d<float> photo [[texture(0)]],
                                              texture2d<float> auxPhoto [[texture(1)]],
                                              constant FmFaceWarpUniforms& u [[buffer(0)]]) {
    float3 aux = auxPhoto.sample(fmFaceMip, in.uv).rgb;
    float skin = auxPhoto.sample(fmFaceMip, in.uv, bias(2.5)).g;
    float open = 1.0 - smoothstep(0.05, 0.95, in.hole);
    float2 uv = fmFaceGaze(fmFaceGaze(in.uv, u.eyeL, u.look.xy), u.eyeR, u.look.zw);
    float3 color = photo.sample(fmFaceMip, uv, bias(0.35)).rgb;
    color = fmFaceLid(fmFaceLid(color, u.eyeL, u.close.x, in.uv, photo), u.eyeR, u.close.y, in.uv, photo);
    FmFaceWarpTargets out;
    out.warp = float4(color, aux.r * open);
    out.aux = float4(skin * aux.r * open, aux.b, 0.5 * (1.0 - open) + 0.5 * smoothstep(1.0, 2.0, in.hole), in.region);
    return out;
}

struct FmFaceStepUniforms {
    float4 step;
};

static float4 fmFaceTap(float2 uv, float mode, texture2d<float> cam, texture2d<float> warp, texture2d<float> aux) {
    float4 a4 = aux.sample(fmFaceLinear, uv);
    float a = mode > 1.5 ? 1.0 : a4.r * a4.a;
    float3 color = (mode < 0.5 || mode > 1.5) ? cam.sample(fmFaceLinear, uv).rgb : warp.sample(fmFaceLinear, uv).rgb;
    return float4(color * a, a);
}

fragment float4 fmFaceReduce(FmFaceQuad in [[stage_in]],
                             texture2d<float> cam [[texture(0)]],
                             texture2d<float> warp [[texture(1)]],
                             texture2d<float> aux [[texture(2)]],
                             constant FmFaceStepUniforms& u [[buffer(0)]]) {
    float2 s = u.step.xy;
    float mode = u.step.z;
    return 0.25 * (fmFaceTap(in.uv + float2(-s.x, -s.y), mode, cam, warp, aux) + fmFaceTap(in.uv + float2(s.x, -s.y), mode, cam, warp, aux) +
                   fmFaceTap(in.uv + float2(-s.x, s.y), mode, cam, warp, aux) + fmFaceTap(in.uv + float2(s.x, s.y), mode, cam, warp, aux));
}

fragment float4 fmFaceBlur(FmFaceQuad in [[stage_in]],
                           texture2d<float> source [[texture(0)]],
                           constant FmFaceStepUniforms& u [[buffer(0)]]) {
    float2 s = u.step.xy;
    float4 sum = source.sample(fmFaceLinear, in.uv) * 0.2270;
    sum += (source.sample(fmFaceLinear, in.uv + s * 1.0) + source.sample(fmFaceLinear, in.uv - s * 1.0)) * 0.1945;
    sum += (source.sample(fmFaceLinear, in.uv + s * 2.0) + source.sample(fmFaceLinear, in.uv - s * 2.0)) * 0.1216;
    sum += (source.sample(fmFaceLinear, in.uv + s * 3.0) + source.sample(fmFaceLinear, in.uv - s * 3.0)) * 0.0540;
    sum += (source.sample(fmFaceLinear, in.uv + s * 4.0) + source.sample(fmFaceLinear, in.uv - s * 4.0)) * 0.0162;
    return sum;
}

static float fmFaceCallMatte(texture2d<float> coef, texture2d<float> clean, float3 color, float2 uv) {
    float3 abc = coef.sample(fmFaceLinear, uv).rgb;
    float i = dot(color, fmFaceLuma);
    float guided = clamp(abc.x * i + abc.y, 0.0, 1.0);
    float hard = smoothstep(0.3, 0.7, clean.sample(fmFaceLinear, uv).r);
    return mix(hard, guided, smoothstep(0.15, 0.6, abc.z));
}

static float fmFaceAlphaAt(texture2d<float> cam, texture2d<float> coef, texture2d<float> clean, float hasMatte, float2 uv) {
    return hasMatte > 0.5 ? fmFaceCallMatte(coef, clean, cam.sample(fmFaceLinear, uv).rgb, uv) : 0.0;
}

fragment float4 fmFaceWeigh(FmFaceQuad in [[stage_in]],
                            texture2d<float> cam [[texture(0)]],
                            texture2d<float> coef [[texture(1)]],
                            texture2d<float> clean [[texture(2)]],
                            constant FmFaceStepUniforms& u [[buffer(0)]]) {
    float2 s = u.step.xy;
    float has = u.step.z;
    float2 a = in.uv + float2(-s.x, -s.y);
    float2 b = in.uv + float2(s.x, -s.y);
    float2 c = in.uv + float2(-s.x, s.y);
    float2 d = in.uv + float2(s.x, s.y);
    float3 color = 0.25 * (cam.sample(fmFaceLinear, a).rgb + cam.sample(fmFaceLinear, b).rgb + cam.sample(fmFaceLinear, c).rgb + cam.sample(fmFaceLinear, d).rgb);
    float reach = max(max(fmFaceAlphaAt(cam, coef, clean, has, a), fmFaceAlphaAt(cam, coef, clean, has, b)),
                      max(fmFaceAlphaAt(cam, coef, clean, has, c), fmFaceAlphaAt(cam, coef, clean, has, d)));
    reach = max(reach, max(fmFaceAlphaAt(cam, coef, clean, has, in.uv + float2(4.0 * s.x, 0.0)), fmFaceAlphaAt(cam, coef, clean, has, in.uv - float2(4.0 * s.x, 0.0))));
    reach = max(reach, max(fmFaceAlphaAt(cam, coef, clean, has, in.uv + float2(0.0, 4.0 * s.y)), fmFaceAlphaAt(cam, coef, clean, has, in.uv - float2(0.0, 4.0 * s.y))));
    float w = pow(1.0 - reach, 3.0);
    return float4(color * w, w);
}

fragment float4 fmFacePush(FmFaceQuad in [[stage_in]],
                           texture2d<float> source [[texture(0)]],
                           constant FmFaceStepUniforms& u [[buffer(0)]]) {
    float2 s = u.step.xy;
    return 0.25 * (source.sample(fmFaceLinear, in.uv + float2(-s.x, -s.y)) + source.sample(fmFaceLinear, in.uv + float2(s.x, -s.y)) +
                   source.sample(fmFaceLinear, in.uv + float2(-s.x, s.y)) + source.sample(fmFaceLinear, in.uv + float2(s.x, s.y)));
}

fragment float4 fmFacePull(FmFaceQuad in [[stage_in]],
                           texture2d<float> own [[texture(0)]],
                           texture2d<float> coarse [[texture(1)]],
                           constant FmFaceStepUniforms& u [[buffer(0)]]) {
    float4 mine = own.sample(fmFaceLinear, in.uv);
    float3 color = mine.rgb / max(mine.a, 1e-4);
    if (u.step.z > 0.5) {
        return float4(mine.a > 1e-4 ? color : float3(0.5), 1.0);
    }
    float3 below = coarse.sample(fmFaceLinear, in.uv).rgb;
    float trust = clamp(mine.a * 3.0, 0.0, 1.0);
    return float4(mix(below, color, trust), 1.0);
}

fragment float4 fmFaceBackdrop(FmFaceQuad in [[stage_in]],
                               texture2d<float> blurred [[texture(0)]],
                               texture2d<float> fill [[texture(1)]]) {
    float4 b = blurred.sample(fmFaceLinear, in.uv);
    float3 filled = fill.sample(fmFaceLinear, in.uv).rgb;
    float3 mine = b.rgb / max(b.a, 1e-4);
    return float4(mix(filled, mine, smoothstep(0.02, 0.3, b.a)), clamp(b.a, 0.0, 1.0));
}

struct FmFaceCompositeUniforms {
    float4 toUprightX;
    float4 toUprightY;
    float4 state;
    float4 extra;
    float4 faceUv;
    float4 zoneCenterX;
    float4 zoneYOutfit;
    float4 tongueCenterX;
    float4 tongueYReach;
    float4 outfitCenterX;
    float4 outfitY;
    float4 hairState;
};

struct FmFaceCompositeTargets {
    float4 color [[color(0)]];
    float4 keep [[color(1)]];
};

static float2 fmFaceAxis(float2 px, float2 center, float2 x, float2 y) {
    float2 d = px - center;
    return float2(dot(d, x), dot(d, y));
}

static float fmFaceZoneRadius(float2 q) {
    return length((q - float2(0.0, -0.35)) / float2(2.55, 2.9));
}

static float fmFaceHairAt(texture2d<float> hair, float2 px, float4 state) {
    return hair.sample(fmFaceLinear, px / state.zw).r;
}

static float fmFaceHeadZone(float2 q, float2 px, texture2d<float> hair, float4 state) {
    float side = abs(q.x);
    float neck = smoothstep(0.8, 1.4, q.y) * (1.0 - smoothstep(0.55, 0.85, side));
    if (state.x > 0.5) {
        float face = 1.0 - smoothstep(0.9, 1.0, length((q - float2(0.0, -0.3)) / float2(2.1, 2.6)));
        face *= 1.0 - smoothstep(1.0, 1.5, q.y) * smoothstep(0.8, 1.05, side);
        float h = fmFaceHairAt(hair, px, state);
        for (int k = 0; k < 8; k++) {
            float a = float(k) * 0.785398;
            float2 dir = float2(cos(a), sin(a));
            h = max(h, fmFaceHairAt(hair, px + dir * 4.0, state));
            h = max(h, fmFaceHairAt(hair, px + dir * 9.0, state) * 0.9);
        }
        float strands = smoothstep(0.05, 0.25, h) * (1.0 - neck);
        return max(face * (1.0 - neck), strands);
    }
    float head = 1.0 - smoothstep(0.82, 1.0, fmFaceZoneRadius(q));
    return head * (1.0 - neck);
}

static float fmFaceHeadHalo(float2 q) {
    return 1.0 - smoothstep(0.9, 1.45, fmFaceZoneRadius(q - float2(0.0, 0.3)));
}

static float3 fmFaceChroma(float3 c) {
    return c / (c.r + c.g + c.b + 0.03);
}

static float3 fmFaceDress(float3 front, float3 cam, float alpha, float zone, float2 uv, float2 px,
                          constant FmFaceCompositeUniforms& u, texture2d<float> camera, texture2d<float> outfit, texture2d<float> hair) {
    float2 q = fmFaceAxis(px, u.outfitCenterX.xy, u.outfitCenterX.zw, u.outfitY.xy);
    float4 cloth = outfit.sample(fmFaceMip, (float2(512.0, 176.0) + q * 96.0) / 1024.0);
    if (cloth.a < 0.002) return front;
    float2 reach = u.tongueYReach.zw;
    float mean = 0.0;
    for (int i = 0; i < 8; i++) {
        float angle = float(i) * 0.785398;
        mean += dot(camera.sample(fmFaceLinear, uv + reach * float2(cos(angle), sin(angle))).rgb, fmFaceLuma);
    }
    mean = mean / 8.0;
    float detail = clamp(pow(dot(cam, fmFaceLuma) / max(mean, 0.02), 0.4), 0.75, 1.12);
    float3 skin = float3(0.0);
    for (int i = 0; i < 5; i++) {
        float2 offset = i == 0 ? float2(0.0) : reach * float2(i == 1 ? 1.0 : i == 2 ? -1.0 : 0.0, i == 3 ? 1.0 : i == 4 ? -1.0 : 0.0);
        skin += camera.sample(fmFaceLinear, u.faceUv.zw + offset).rgb;
    }
    skin = skin / 5.0;
    float near = 1.0 - smoothstep(0.035, 0.07, distance(fmFaceChroma(cam), fmFaceChroma(skin)));
    float bright = dot(cam, fmFaceLuma) / max(dot(skin, fmFaceLuma), 0.02);
    float hands = near * smoothstep(0.45, 0.65, bright) * (1.0 - smoothstep(1.6, 2.0, bright)) * smoothstep(1.2, 1.9, q.y);
    float strands = u.hairState.x > 0.5 ? smoothstep(0.4, 0.8, fmFaceHairAt(hair, px, u.hairState)) : 0.0;
    float g = cloth.a * u.zoneYOutfit.z * (1.0 - hands) * (1.0 - strands) * (1.0 - zone);
    float3 tint = cloth.rgb / cloth.a;
    return mix(front, tint * detail * alpha, g);
}

static float fmFaceGrain(float2 at, float seed) {
    return fract(sin(dot(floor(at) + float2(seed, seed * 0.61), float2(12.9898, 78.233))) * 43758.5453) - 0.5;
}

static float3 fmFaceShoulder(float3 x) {
    float k = 0.82;
    float3 over = max(x - k, 0.0);
    return min(x, float3(k)) + (1.0 - k) * (1.0 - exp(-over / (1.0 - k)));
}

static float fmFaceTongueAt(float2 px, float3 cam, constant FmFaceCompositeUniforms& u) {
    float2 q = fmFaceAxis(px, u.tongueCenterX.xy, u.tongueCenterX.zw, u.tongueYReach.xy);
    float inside = 1.0 - smoothstep(0.75, 1.0, length(q));
    float red = (cam.r - cam.g) / (cam.r + cam.g + cam.b + 0.047);
    float skinRed = u.extra.z;
    return inside * smoothstep(skinRed + 0.04, skinRed + 0.1, red);
}

fragment FmFaceCompositeTargets fmFaceComposite(FmFaceQuad in [[stage_in]],
                                                texture2d<float> camera [[texture(0)]],
                                                texture2d<float> coef [[texture(1)]],
                                                texture2d<float> clean [[texture(2)]],
                                                texture2d<float> warpT [[texture(3)]],
                                                texture2d<float> auxT [[texture(4)]],
                                                texture2d<float> lowCamT [[texture(5)]],
                                                texture2d<float> lowFaceT [[texture(6)]],
                                                texture2d<float> wideCamT [[texture(7)]],
                                                texture2d<float> wideFaceT [[texture(8)]],
                                                texture2d<float> fillT [[texture(9)]],
                                                texture2d<float> backdropT [[texture(10)]],
                                                texture2d<float> plainT [[texture(11)]],
                                                texture2d<float> outfit [[texture(12)]],
                                                texture2d<float> hair [[texture(13)]],
                                                constant FmFaceCompositeUniforms& u [[buffer(0)]]) {
    float2 uv = in.uv;
    float3 buffer = float3(uv * float2(u.toUprightX.w, u.toUprightY.w), 1.0);
    float2 px = float2(dot(u.toUprightX.xyz, buffer), dot(u.toUprightY.xyz, buffer));
    float hasMatte = u.state.x;
    bool face = u.state.y > 0.5;
    float presence = u.state.z;
    float hide = u.state.w;
    bool follow = u.extra.x > 0.5;
    float tongue = u.extra.y;
    float3 cam = camera.sample(fmFaceLinear, uv).rgb;
    float alpha = hasMatte > 0.5 ? fmFaceCallMatte(coef, clean, cam, uv) : 1.0;
    float mode = u.hairState.y;
    float3 fill = fillT.sample(fmFaceLinear, uv).rgb;
    float4 warp = float4(0.0);
    float4 aux = float4(0.0);
    float zone = 0.0;
    float hole = 0.0;
    float cavity = 0.0;
    if (face) {
        warp = warpT.sample(fmFaceLinear, uv);
        aux = auxT.sample(fmFaceLinear, uv);
        hole = min(aux.b * 2.0, 1.0);
        cavity = max(aux.b * 2.0 - 1.0, 0.0);
        zone = fmFaceHeadZone(fmFaceAxis(px, u.zoneCenterX.xy, u.zoneCenterX.zw, u.zoneYOutfit.xy), px, hair, u.hairState) * presence * (1.0 - hole) * hasMatte;
    }
    float3 front = clamp(cam - (1.0 - alpha) * fill, float3(0.0), float3(alpha));
    float3 behind = alpha < 0.98 ? (cam - front) / (1.0 - alpha) : fill;
    if (u.zoneYOutfit.z > 0.001 && hasMatte > 0.5) front = fmFaceDress(front, cam, alpha, zone, uv, px, u, camera, outfit, hair);
    float3 backdrop = behind;
    if (mode > 0.5) {
        backdrop = backdropT.sample(fmFaceLinear, uv).rgb;
    } else if (face && !follow) {
        float4 soft = backdropT.sample(fmFaceLinear, uv);
        float2 q = fmFaceAxis(px, u.zoneCenterX.xy, u.zoneCenterX.zw, u.zoneYOutfit.xy);
        float halo = fmFaceHeadHalo(q) * presence * hasMatte * (1.0 - smoothstep(0.45, 0.92, soft.a));
        backdrop = mix(behind, soft.rgb, halo);
    }
    float keep = alpha * (1.0 - zone);
    float3 color = backdrop * (1.0 - keep) + front * (1.0 - zone);
    float cover = 0.0;
    if (face) color *= mix(1.0, mix(0.6, 1.0, smoothstep(0.0, 1.0, cavity)), hole * presence);
    if (face && warp.a > 0.002) {
        float4 lowCam = lowCamT.sample(fmFaceLinear, uv);
        float4 lowFace = lowFaceT.sample(fmFaceLinear, uv);
        float4 wideCam = wideCamT.sample(fmFaceLinear, uv);
        float4 wideFace = wideFaceT.sample(fmFaceLinear, uv);
        float4 centerCam = wideCamT.sample(fmFaceLinear, u.faceUv.xy);
        float4 centerFace = wideFaceT.sample(fmFaceLinear, u.faceUv.xy);
        float3 meanCam = centerCam.rgb / max(centerCam.a, 1e-4);
        float3 meanFace = centerFace.rgb / max(centerFace.a, 1e-4);
        bool known = centerFace.a > 0.01 && centerCam.a > 0.01;
        float3 global = known ? clamp((meanCam + 0.004) / (meanFace + 0.004), 0.3, 3.0) : float3(1.0);
        float3 local = known ? clamp((wideCam.rgb + 0.12 * meanCam) / (wideFace.rgb + 0.12 * meanFace), 0.3, 3.0) : float3(1.0);
        float nearLight = (dot(lowCam.rgb, fmFaceLuma) + 0.004 * lowCam.a) / (dot(lowFace.rgb, fmFaceLuma) + 0.004 * lowFace.a);
        float farLight = (dot(wideCam.rgb, fmFaceLuma) + 0.004 * wideCam.a) / (dot(wideFace.rgb, fmFaceLuma) + 0.004 * wideFace.a);
        float shade = lowFace.a > 0.01 && wideFace.a > 0.01 ? clamp(nearLight / farLight, 0.7, 1.4) : 1.0;
        shade = mix(1.0, shade, smoothstep(0.3, 0.7, lowFace.a));
        float level = max(dot(global, fmFaceLuma), 0.05);
        float3 hue = global / level;
        float3 hair = clamp(pow(hue, float3(0.5)) * pow(level, 0.5), 0.45, 1.8);
        float3 tone = clamp(pow(hue, float3(0.6)) * pow(level, 0.4), 0.55, 1.8);
        float3 lit = clamp(local / max(global, float3(0.05)), 0.6, 1.6) * tone;
        float skin = clamp(aux.r / max(warp.a, 0.002), 0.0, 1.0);
        skin = smoothstep(0.15, 0.85, skin);
        float3 drawn = clamp(fmFaceShoulder(warp.rgb * mix(hair, lit * shade, skin)) + fmFaceGrain(px, u.extra.w) * 0.012, 0.0, 1.0);
        float lip = hole * (1.0 - hole) * 4.0;
        color *= 1.0 - 0.22 * lip * presence;
        cover = warp.a * presence;
        color = mix(color, drawn, cover);
        if (tongue > 0.001) color = mix(color, cam, fmFaceTongueAt(px, cam, u) * tongue * presence);
    }
    float shown = 1.0 - (1.0 - keep) * (1.0 - cover);
    if (hide > 0.001) {
        float3 plain = plainT.sample(fmFaceLinear, uv).rgb;
        float3 hidden = mode > 0.5 ? mix(backdrop, plain, smoothstep(0.1, 0.6, alpha)) : plain;
        color = mix(color, hidden, hide);
        shown = mix(shown, smoothstep(0.1, 0.6, alpha), hide);
    }
    FmFaceCompositeTargets out;
    out.color = float4(color, 1.0);
    out.keep = float4(shown, 0.0, 0.0, 1.0);
    return out;
}
