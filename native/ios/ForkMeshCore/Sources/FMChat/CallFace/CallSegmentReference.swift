import Foundation

public enum CallSegmentReference {
    public static func activate(_ v: Float, _ act: CallSegmentLayer.Activation, alpha: Float) -> Float {
        switch act {
        case .none: return v
        case .relu: return max(v, 0)
        case .prelu: return max(v, 0) + alpha * min(v, 0)
        case .hswish: return v * min(max(v + 3, 0), 6) / 6
        case .sigmoid: return 1 / (1 + exp(-min(max(v, -30), 30)))
        }
    }

    public static func run(_ model: CallSegmentModel, input: [Float], keep: Set<Int> = []) -> [Int: [Float]] {
        let weights = model.weights.map(CallSegmentHalf.toFloat)
        var tensors: [Int: [Float]] = [model.input: input.map(CallSegmentHalf.round)]
        for layer in model.layers {
            let a = model.shapes[layer.inputs[0]]!
            let b = layer.inputs.count > 1 ? model.shapes[layer.inputs[1]]! : CallSegmentShape(0, 0, 0)
            let o = model.shapes[layer.output]!
            let first = tensors[layer.inputs[0]]!
            let second = layer.inputs.count > 1 ? tensors[layer.inputs[1]]! : []
            tensors[layer.output] = weights.withUnsafeBufferPointer { w in
                first.withUnsafeBufferPointer { inA in
                    second.withUnsafeBufferPointer { inB in
                        step(layer, a: a, b: b, o: o, inA: inA, inB: inB, w: w)
                    }
                }
            }
        }
        if keep.isEmpty { return tensors }
        return tensors.filter { keep.contains($0.key) }
    }

    static func step(_ layer: CallSegmentLayer, a: CallSegmentShape, b: CallSegmentShape, o: CallSegmentShape,
                     inA: UnsafeBufferPointer<Float>, inB: UnsafeBufferPointer<Float>, w: UnsafeBufferPointer<Float>) -> [Float] {
        var out = [Float](repeating: 0, count: o.count)
        func at(_ x: Int, _ y: Int, _ c: Int) -> Float {
            x < 0 || y < 0 || x >= a.w || y >= a.h ? 0 : inA[(y * a.w + x) * a.c + c]
        }
        func atB(_ x: Int, _ y: Int, _ c: Int) -> Float {
            x < 0 || y < 0 || x >= b.w || y >= b.h ? 0 : inB[(y * b.w + x) * b.c + c]
        }
        let kh = layer.kernel[0], kw = layer.kernel[1]
        let sh = layer.stride[0], sw = layer.stride[1]
        let pt = layer.pad[0], pl = layer.pad[1]
        let dh = layer.dilation[0], dwx = layer.dilation[1]
        let (scaleX, scaleY) = CallSegmentPlan.resizeScale(layer, a, o)
        out.withUnsafeMutableBufferPointer { result in
            for y in 0..<o.h {
                for x in 0..<o.w {
                    for c in 0..<o.c {
                        var v: Float = 0
                        switch layer.op {
                        case .conv, .tconv:
                            v = w[layer.bias + c]
                            for ky in 0..<kh {
                                for kx in 0..<kw {
                                    var ix = 0, iy = 0
                                    if layer.op == .conv {
                                        ix = x * sw + kx * dwx - pl
                                        iy = y * sh + ky * dh - pt
                                    } else {
                                        let nx = x + pl - kx
                                        let ny = y + pt - ky
                                        ix = Int((Double(nx) / Double(sw)).rounded(.down))
                                        iy = Int((Double(ny) / Double(sh)).rounded(.down))
                                        if ix * sw != nx || iy * sh != ny { continue }
                                    }
                                    if ix < 0 || iy < 0 || ix >= a.w || iy >= a.h { continue }
                                    let row = ((c * kh + ky) * kw + kx) * layer.cin + layer.weights
                                    let base = (iy * a.w + ix) * a.c
                                    var g = 0
                                    while g < layer.cin {
                                        var part: Float = w[row + g] * inA[base + g]
                                        var j = 1
                                        while j < 4 && g + j < layer.cin {
                                            part += w[row + g + j] * inA[base + g + j]
                                            j += 1
                                        }
                                        v += part
                                        g += 4
                                    }
                                }
                            }
                        case .dw:
                            v = w[layer.bias + c]
                            for ky in 0..<kh {
                                for kx in 0..<kw {
                                    let ix = x * sw + kx - pl
                                    let iy = y * sh + ky - pt
                                    v += w[layer.weights + (ky * kw + kx) * layer.cout + c] * at(ix, iy, c)
                                }
                            }
                        case .meanrow:
                            for i in 0..<a.w { v += at(i, y, c) }
                            v /= Float(a.w)
                        case .meancol:
                            for i in 0..<a.h { v += at(0, i, c) }
                            v /= Float(a.h)
                        case .add, .mul:
                            let other = b.h == 1 && b.w == 1 ? atB(0, 0, c) : atB(x, y, c)
                            v = layer.op == .add ? at(x, y, c) + other : at(x, y, c) * other
                        case .resize:
                            let fx = layer.half ? max((Float(x) + 0.5) * scaleX - 0.5, 0) : Float(x) * scaleX
                            let fy = layer.half ? max((Float(y) + 0.5) * scaleY - 0.5, 0) : Float(y) * scaleY
                            let x0 = Int(fx.rounded(.down))
                            let y0 = Int(fy.rounded(.down))
                            let x1 = min(x0 + 1, a.w - 1)
                            let y1 = min(y0 + 1, a.h - 1)
                            let tx = fx - Float(x0)
                            let ty = fy - Float(y0)
                            let top = at(x0, y0, c) * (1 - tx) + at(x1, y0, c) * tx
                            let bottom = at(x0, y1, c) * (1 - tx) + at(x1, y1, c) * tx
                            v = top * (1 - ty) + bottom * ty
                        case .act:
                            v = at(x, y, c)
                        case .argmax:
                            var best: Float = -65000
                            for k in 0..<4 {
                                let t = at(x * 2 + k % 2, y * 2 + k / 2, c)
                                if t > best { v = Float(k) }
                                best = max(best, t)
                            }
                        case .unpool:
                            let px = x / 2
                            let py = y / 2
                            let k = Float((y - py * 2) * 2 + (x - px * 2))
                            v = abs(atB(px, py, c) - k) < 0.5 ? at(px, py, c) : 0
                        case .concat:
                            v = c < a.c ? at(x, y, c) : atB(x, y, c - a.c)
                        case .pad:
                            v = c < a.c ? at(x, y, c) : 0
                        case .maxpool:
                            v = -65000
                            for ky in 0..<kh {
                                for kx in 0..<kw {
                                    let ix = x * sw + kx - pl
                                    let iy = y * sh + ky - pt
                                    if ix < 0 || iy < 0 || ix >= a.w || iy >= a.h { continue }
                                    v = max(v, at(ix, iy, c))
                                }
                            }
                        }
                        let alpha = layer.alpha >= 0 && c < layer.alphaCount ? w[layer.alpha + c] : 0
                        result[(y * o.w + x) * o.c + c] = CallSegmentHalf.round(activate(v, layer.act, alpha: alpha))
                    }
                }
            }
        }
        return out
    }
}
