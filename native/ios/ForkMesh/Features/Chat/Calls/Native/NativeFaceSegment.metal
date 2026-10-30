#include <metal_stdlib>
using namespace metal;

constexpr sampler fmSegLinear(coord::normalized, address::clamp_to_edge, filter::linear);
constexpr sampler fmSegNearest(coord::normalized, address::clamp_to_edge, filter::nearest);
constant float3 fmSegLuma = float3(0.299, 0.587, 0.114);

static inline float fmSegActivate(float v, int act, float alpha) {
    if (act == 1) return max(v, 0.0f);
    if (act == 2) return max(v, 0.0f) + alpha * min(v, 0.0f);
    if (act == 3) return v * clamp(v + 3.0f, 0.0f, 6.0f) / 6.0f;
    if (act == 4) return 1.0f / (1.0f + precise::exp(-clamp(v, -30.0f, 30.0f)));
    return v;
}

static inline float fmSegAt(device const half* t, int x, int y, int c, int w, int h, int channels) {
    if (x < 0 || y < 0 || x >= w || y >= h) return 0.0f;
    return float(t[(y * w + x) * channels + c]);
}

kernel void fmSegLayer(device const half* a [[buffer(0)]],
                       device const half* b [[buffer(1)]],
                       device half* out [[buffer(2)]],
                       device const half* w [[buffer(3)]],
                       constant int* p [[buffer(4)]],
                       uint gid [[thread_position_in_grid]]) {
    int count = p[28];
    int index = int(gid);
    if (index >= count) return;
    int op = p[0];
    int act = p[1];
    int inH = p[2], inW = p[3], inC = p[4];
    int outW = p[6], outC = p[7];
    int bH = p[8], bW = p[9], bC = p[10];
    int kh = p[11], kw = p[12], sh = p[13], sw = p[14], pt = p[15], pl = p[16], dh = p[17], dwx = p[18];
    int wOff = p[19], bOff = p[20], aOff = p[21], cin = p[22];
    bool halfPixel = p[24] != 0;
    bool broadcast = p[25] != 0;
    float scaleX = as_type<float>(p[26]);
    float scaleY = as_type<float>(p[27]);
    int c = index % outC;
    int x = (index / outC) % outW;
    int y = index / (outC * outW);
    float v = 0.0f;
    if (op == 0 || op == 1) {
        v = float(w[bOff + c]);
        for (int ky = 0; ky < kh; ky++) {
            for (int kx = 0; kx < kw; kx++) {
                int ix;
                int iy;
                if (op == 0) {
                    ix = x * sw + kx * dwx - pl;
                    iy = y * sh + ky * dh - pt;
                } else {
                    int nx = x + pl - kx;
                    int ny = y + pt - ky;
                    if (nx < 0 || ny < 0 || nx % sw != 0 || ny % sh != 0) continue;
                    ix = nx / sw;
                    iy = ny / sh;
                }
                if (ix < 0 || iy < 0 || ix >= inW || iy >= inH) continue;
                device const half* row = w + wOff + ((c * kh + ky) * kw + kx) * cin;
                device const half* at = a + (iy * inW + ix) * inC;
                for (int g = 0; g < cin; g += 4) {
                    float part = float(row[g]) * float(at[g]);
                    if (g + 1 < cin) part += float(row[g + 1]) * float(at[g + 1]);
                    if (g + 2 < cin) part += float(row[g + 2]) * float(at[g + 2]);
                    if (g + 3 < cin) part += float(row[g + 3]) * float(at[g + 3]);
                    v += part;
                }
            }
        }
    } else if (op == 2) {
        v = float(w[bOff + c]);
        for (int ky = 0; ky < kh; ky++) {
            for (int kx = 0; kx < kw; kx++) {
                v += float(w[wOff + (ky * kw + kx) * outC + c]) * fmSegAt(a, x * sw + kx - pl, y * sh + ky - pt, c, inW, inH, inC);
            }
        }
    } else if (op == 3) {
        for (int i = 0; i < inW; i++) v += fmSegAt(a, i, y, c, inW, inH, inC);
        v /= float(inW);
    } else if (op == 4) {
        for (int i = 0; i < inH; i++) v += fmSegAt(a, 0, i, c, inW, inH, inC);
        v /= float(inH);
    } else if (op == 5 || op == 6) {
        float other = broadcast ? fmSegAt(b, 0, 0, c, bW, bH, bC) : fmSegAt(b, x, y, c, bW, bH, bC);
        float mine = fmSegAt(a, x, y, c, inW, inH, inC);
        v = op == 5 ? mine + other : mine * other;
    } else if (op == 7) {
        float fx = halfPixel ? max((float(x) + 0.5f) * scaleX - 0.5f, 0.0f) : float(x) * scaleX;
        float fy = halfPixel ? max((float(y) + 0.5f) * scaleY - 0.5f, 0.0f) : float(y) * scaleY;
        int x0 = int(floor(fx));
        int y0 = int(floor(fy));
        int x1 = min(x0 + 1, inW - 1);
        int y1 = min(y0 + 1, inH - 1);
        float tx = fx - float(x0);
        float ty = fy - float(y0);
        float top = fmSegAt(a, x0, y0, c, inW, inH, inC) * (1.0f - tx) + fmSegAt(a, x1, y0, c, inW, inH, inC) * tx;
        float bottom = fmSegAt(a, x0, y1, c, inW, inH, inC) * (1.0f - tx) + fmSegAt(a, x1, y1, c, inW, inH, inC) * tx;
        v = top * (1.0f - ty) + bottom * ty;
    } else if (op == 8) {
        v = fmSegAt(a, x, y, c, inW, inH, inC);
    } else if (op == 9) {
        float best = -65000.0f;
        for (int k = 0; k < 4; k++) {
            float t = fmSegAt(a, x * 2 + (k & 1), y * 2 + (k >> 1), c, inW, inH, inC);
            if (t > best) v = float(k);
            best = max(best, t);
        }
    } else if (op == 10) {
        int px = x / 2;
        int py = y / 2;
        float k = float((y - py * 2) * 2 + (x - px * 2));
        v = abs(fmSegAt(b, px, py, c, bW, bH, bC) - k) < 0.5f ? fmSegAt(a, px, py, c, inW, inH, inC) : 0.0f;
    } else if (op == 11) {
        v = c < inC ? fmSegAt(a, x, y, c, inW, inH, inC) : fmSegAt(b, x, y, c - inC, bW, bH, bC);
    } else if (op == 12) {
        v = c < inC ? fmSegAt(a, x, y, c, inW, inH, inC) : 0.0f;
    } else if (op == 13) {
        v = -65000.0f;
        for (int ky = 0; ky < kh; ky++) {
            for (int kx = 0; kx < kw; kx++) {
                int ix = x * sw + kx - pl;
                int iy = y * sh + ky - pt;
                if (ix < 0 || iy < 0 || ix >= inW || iy >= inH) continue;
                v = max(v, fmSegAt(a, ix, iy, c, inW, inH, inC));
            }
        }
    }
    float alpha = aOff >= 0 ? float(w[aOff + c]) : 0.0f;
    out[index] = half(fmSegActivate(v, act, alpha));
}

struct FmHairCropUniforms {
    float4 region;
    float4 frame;
    float4 toBufferX;
    float4 toBufferY;
    int4 size;
};

static inline float2 fmSegBufferUv(float2 upright, constant float4& toX, constant float4& toY) {
    float3 p = float3(upright, 1.0);
    return float2(dot(toX.xyz, p) / toX.w, dot(toY.xyz, p) / toY.w);
}

kernel void fmHairCrop(texture2d<float> camera [[texture(0)]],
                       texture2d<float> prev [[texture(1)]],
                       device half* input [[buffer(0)]],
                       constant FmHairCropUniforms& u [[buffer(1)]],
                       uint2 gid [[thread_position_in_grid]]) {
    int width = u.size.x;
    int height = u.size.y;
    if (int(gid.x) >= width || int(gid.y) >= height) return;
    float2 size = float2(width, height);
    float2 vUv = (float2(gid) + 0.5) / size;
    float2 p = u.region.xy + (vUv - 0.5) * u.region.zw;
    float2 uv = p / u.frame.xy;
    bool inside = uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0;
    float2 step = u.region.zw / size / u.frame.xy / 3.0;
    float3 c = float3(0.0);
    for (int i = -1; i <= 1; i++) {
        for (int j = -1; j <= 1; j++) {
            float2 at = (uv + float2(float(i), float(j)) * step) * u.frame.xy;
            c += camera.sample(fmSegLinear, fmSegBufferUv(at, u.toBufferX, u.toBufferY)).rgb;
        }
    }
    c = inside ? c / 9.0 : float3(0.0);
    float before = inside ? prev.sample(fmSegLinear, uv).r : 0.0;
    device half* at = input + (int(gid.y) * width + int(gid.x)) * 4;
    at[0] = half(c.r);
    at[1] = half(c.g);
    at[2] = half(c.b);
    at[3] = half(before);
}

struct FmSegQuad {
    float4 position [[position]];
    float2 uv;
};

vertex FmSegQuad fmSegQuadVertex(uint vid [[vertex_id]]) {
    float2 p = float2(float((vid << 1) & 2), float(vid & 2));
    FmSegQuad out;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    out.uv = float2(p.x, 1.0 - p.y);
    return out;
}

struct FmHairFrameUniforms {
    float4 region;
    float4 frame;
    float4 logits;
    float4 depth;
    float4 depthToX;
    float4 depthToY;
};

static inline float fmHairAt(device const half* logits, int x, int y, int w, int h) {
    int cx = clamp(x, 0, w - 1);
    int cy = clamp(y, 0, h - 1);
    device const half* l = logits + (cy * w + cx) * 2;
    return 1.0 / (1.0 + precise::exp(clamp(float(l[0]) - float(l[1]), -30.0, 30.0)));
}

fragment float4 fmHairFrame(FmSegQuad in [[stage_in]],
                            device const half* logits [[buffer(1)]],
                            texture2d<float> prev [[texture(0)]],
                            texture2d<float> depth [[texture(1)]],
                            constant FmHairFrameUniforms& u [[buffer(0)]]) {
    float2 c = (in.uv * u.frame.xy - u.region.xy) / u.region.zw + 0.5;
    float before = prev.sample(fmSegLinear, in.uv).r;
    float now = 0.0;
    int lw = int(u.logits.x);
    int lh = int(u.logits.y);
    if (c.x >= 0.0 && c.y >= 0.0 && c.x <= 1.0 && c.y <= 1.0) {
        float2 t = c * u.logits.xy - 0.5;
        float2 f = fract(t);
        int2 base = int2(floor(t));
        float a = fmHairAt(logits, base.x, base.y, lw, lh);
        float b = fmHairAt(logits, base.x + 1, base.y, lw, lh);
        float d = fmHairAt(logits, base.x, base.y + 1, lw, lh);
        float e = fmHairAt(logits, base.x + 1, base.y + 1, lw, lh);
        now = mix(mix(a, b, f.x), mix(d, e, f.x), f.y);
    }
    if (u.depth.x > 0.5) {
        float2 upright = in.uv * u.frame.xy;
        float3 p = float3(upright, 1.0);
        float2 duv = float2(dot(u.depthToX.xyz, p), dot(u.depthToY.xyz, p));
        if (duv.x >= 0.0 && duv.y >= 0.0 && duv.x <= 1.0 && duv.y <= 1.0) {
            float metres = depth.sample(fmSegNearest, duv).r;
            if (metres > 0.1 && metres < 5.0) now *= 1.0 - smoothstep(u.depth.y + u.depth.z, u.depth.y + u.depth.w, metres);
        }
    }
    float change = abs(now - before);
    float rate = mix(u.frame.z, 1.0, smoothstep(0.1, 0.5, change));
    return float4(mix(before, now, rate), 0.0, 0.0, 1.0);
}

struct FmMatteUniforms {
    float4 a;
    float4 b;
    float4 c;
    float4 d;
};

fragment float4 fmMatteFuse(FmSegQuad in [[stage_in]],
                            texture2d<float> vision [[texture(0)]],
                            texture2d<float> depth [[texture(1)]],
                            constant FmMatteUniforms& u [[buffer(0)]]) {
    float seen = vision.sample(fmSegLinear, in.uv).r;
    float3 p = float3(in.uv, 1.0);
    float2 duv = float2(dot(u.b.xyz, p), dot(u.c.xyz, p));
    if (duv.x < 0.0 || duv.y < 0.0 || duv.x > 1.0 || duv.y > 1.0) return float4(seen, 0.0, 0.0, 1.0);
    float metres = depth.sample(fmSegNearest, duv).r;
    if (!(metres > 0.1 && metres < 5.0)) return float4(seen, 0.0, 0.0, 1.0);
    float face = u.a.x;
    float near = 1.0 - smoothstep(face + u.a.y, face + u.a.z, metres);
    float agree = u.a.w;
    return float4(mix(seen, near, agree), 0.0, 0.0, 1.0);
}

fragment float4 fmMatteTemporal(FmSegQuad in [[stage_in]],
                                texture2d<float> raw [[texture(0)]],
                                texture2d<float> prev [[texture(1)]],
                                constant FmMatteUniforms& u [[buffer(0)]]) {
    float now = raw.sample(fmSegLinear, in.uv).r;
    float before = prev.sample(fmSegLinear, in.uv).r;
    float change = abs(now - before);
    float rate = mix(u.a.y, 1.0, smoothstep(0.06, 0.4, change));
    float v = mix(before, now, u.a.x > 0.5 ? 1.0 : rate);
    return float4(v, 0.0, 0.0, 1.0);
}

fragment float4 fmMatteSeed(FmSegQuad in [[stage_in]],
                            texture2d<float> mask [[texture(0)]],
                            texture2d<float> prev [[texture(1)]],
                            constant FmMatteUniforms& u [[buffer(0)]]) {
    float m = 0.0;
    for (int i = -1; i <= 1; i++) {
        for (int j = -1; j <= 1; j++) m = max(m, mask.sample(fmSegLinear, in.uv + float2(float(i), float(j)) * u.a.xy).r);
    }
    float inside = smoothstep(0.25, 0.5, m);
    float3 p = float3(in.uv, 1.0);
    float2 upright = float2(dot(u.c.xyz, p), dot(u.d.xyz, p));
    float seed = step(0.86, upright.y);
    if (u.b.z > 0.0) seed = max(seed, step(length(upright - u.b.xy), u.b.z));
    float before = u.a.z > 0.5 ? 0.0 : prev.sample(fmSegLinear, in.uv).r;
    return float4(min(inside, max(seed, before)), inside, 0.0, 1.0);
}

fragment float4 fmMatteGrow(FmSegQuad in [[stage_in]],
                            texture2d<float> source [[texture(0)]],
                            constant FmMatteUniforms& u [[buffer(0)]]) {
    float2 s = u.a.xy;
    float4 here = source.sample(fmSegLinear, in.uv);
    float grown = here.r;
    grown = max(grown, source.sample(fmSegLinear, in.uv + float2(s.x, 0.0)).r);
    grown = max(grown, source.sample(fmSegLinear, in.uv - float2(s.x, 0.0)).r);
    grown = max(grown, source.sample(fmSegLinear, in.uv + float2(0.0, s.y)).r);
    grown = max(grown, source.sample(fmSegLinear, in.uv - float2(0.0, s.y)).r);
    grown = max(grown, source.sample(fmSegLinear, in.uv + s).r);
    grown = max(grown, source.sample(fmSegLinear, in.uv - s).r);
    grown = max(grown, source.sample(fmSegLinear, in.uv + float2(s.x, -s.y)).r);
    grown = max(grown, source.sample(fmSegLinear, in.uv + float2(-s.x, s.y)).r);
    return float4(min(here.g, grown), here.g, 0.0, 1.0);
}

fragment float4 fmMatteClean(FmSegQuad in [[stage_in]],
                             texture2d<float> mask [[texture(0)]],
                             texture2d<float> keep [[texture(1)]],
                             constant FmMatteUniforms& u [[buffer(0)]]) {
    float k = 0.0;
    for (int i = -1; i <= 1; i++) {
        for (int j = -1; j <= 1; j++) k = max(k, keep.sample(fmSegLinear, in.uv + float2(float(i), float(j)) * u.a.xy).r);
    }
    return float4(mask.sample(fmSegLinear, in.uv).r * smoothstep(0.05, 0.4, k), 0.0, 0.0, 1.0);
}

static inline float fmMatteLuma(texture2d<float> cam, float2 uv) {
    return dot(cam.sample(fmSegLinear, uv).rgb, fmSegLuma);
}

fragment float4 fmMatteMoments(FmSegQuad in [[stage_in]],
                               texture2d<float> cam [[texture(0)]],
                               texture2d<float> mask [[texture(1)]],
                               constant FmMatteUniforms& u [[buffer(0)]]) {
    float2 s = u.a.xy;
    float i = 0.25 * (fmMatteLuma(cam, in.uv + float2(-s.x, -s.y)) + fmMatteLuma(cam, in.uv + float2(s.x, -s.y)) +
                      fmMatteLuma(cam, in.uv + float2(-s.x, s.y)) + fmMatteLuma(cam, in.uv + float2(s.x, s.y)));
    float p = smoothstep(0.1, 0.9, mask.sample(fmSegLinear, in.uv).r);
    return float4(i, p, i * p, i * i);
}

fragment float4 fmMatteBox(FmSegQuad in [[stage_in]],
                           texture2d<float> source [[texture(0)]],
                           constant FmMatteUniforms& u [[buffer(0)]]) {
    float4 sum = float4(0.0);
    for (int k = -4; k <= 4; k++) sum += source.sample(fmSegLinear, in.uv + u.a.xy * float(k));
    return sum / 9.0;
}

fragment float4 fmMatteCoefficients(FmSegQuad in [[stage_in]],
                                    texture2d<float> means [[texture(0)]],
                                    constant FmMatteUniforms& u [[buffer(0)]]) {
    float4 m = means.sample(fmSegLinear, in.uv);
    float eps = u.a.x;
    float variance = max(m.w - m.x * m.x, 0.0);
    float a = (m.z - m.x * m.y) / (variance + eps);
    float b = m.y - a * m.x;
    return float4(a, b, variance / (variance + eps * 3.0), 1.0);
}
