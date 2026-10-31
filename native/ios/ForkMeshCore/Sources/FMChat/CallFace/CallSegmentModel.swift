import Foundation

public enum CallSegmentHalf {
    public static func toFloat(_ h: UInt16) -> Float {
        let sign: Float = h & 0x8000 != 0 ? -1 : 1
        let exponent = Int((h >> 10) & 0x1f)
        let fraction = Float(h & 0x3ff)
        if exponent == 0 { return sign * Float(pow(2.0, -14.0)) * (fraction / 1024) }
        if exponent == 31 { return fraction != 0 ? .nan : sign * .infinity }
        return sign * Float(pow(2.0, Double(exponent - 15))) * (1 + fraction / 1024)
    }

    public static func fromFloat(_ value: Float) -> UInt16 {
        let bits = value.bitPattern
        let sign = UInt16((bits >> 16) & 0x8000)
        let exponent = Int((bits >> 23) & 0xff)
        var mantissa = bits & 0x7fffff
        if exponent == 0xff { return sign | (mantissa != 0 ? 0x7e00 : 0x7c00) }
        let unbiased = exponent - 127 + 15
        if unbiased >= 31 { return sign | 0x7c00 }
        if unbiased <= 0 {
            if unbiased < -10 { return sign }
            mantissa |= 0x800000
            let shift = UInt32(14 - unbiased)
            var half = mantissa >> shift
            let rest = mantissa & ((1 << shift) - 1)
            let halfway = UInt32(1) << (shift - 1)
            if rest > halfway || (rest == halfway && half & 1 == 1) { half += 1 }
            return sign | UInt16(half)
        }
        var half = UInt32(unbiased) << 10 | (mantissa >> 13)
        let rest = mantissa & 0x1fff
        if rest > 0x1000 || (rest == 0x1000 && half & 1 == 1) { half += 1 }
        return sign | UInt16(half)
    }

    public static func round(_ value: Float) -> Float {
        toFloat(fromFloat(value))
    }
}

public struct CallSegmentLayer: Equatable, Sendable {
    public enum Op: Int32, Sendable {
        case conv = 0, tconv, dw, meanrow, meancol, add, mul, resize, act, argmax, unpool, concat, pad, maxpool
    }

    public enum Activation: Int32, Sendable {
        case none = 0, relu, prelu, hswish, sigmoid
    }

    public let op: Op
    public let inputs: [Int]
    public let output: Int
    public let act: Activation
    public let kernel: [Int]
    public let stride: [Int]
    public let pad: [Int]
    public let dilation: [Int]
    public let weights: Int
    public let bias: Int
    public let alpha: Int
    public let alphaCount: Int
    public let cin: Int
    public let cout: Int
    public let align: Bool
    public let half: Bool

    public init(op: Op, inputs: [Int], output: Int, act: Activation, kernel: [Int] = [1, 1], stride: [Int] = [1, 1], pad: [Int] = [0, 0],
                dilation: [Int] = [1, 1], weights: Int = -1, bias: Int = -1, alpha: Int = -1, alphaCount: Int = 0, cin: Int = 0, cout: Int = 0,
                align: Bool = false, half: Bool = false) {
        self.op = op
        self.inputs = inputs
        self.output = output
        self.act = act
        self.kernel = kernel
        self.stride = stride
        self.pad = pad
        self.dilation = dilation
        self.weights = weights
        self.bias = bias
        self.alpha = alpha
        self.alphaCount = alphaCount
        self.cin = cin
        self.cout = cout
        self.align = align
        self.half = half
    }
}

public struct CallSegmentShape: Equatable, Sendable {
    public let h: Int
    public let w: Int
    public let c: Int

    public init(_ h: Int, _ w: Int, _ c: Int) {
        self.h = h
        self.w = w
        self.c = c
    }

    public var count: Int { h * w * c }
}

public enum CallSegmentError: Error, Equatable {
    case notAModel
    case badHeader
    case unsupported(String)
}

public struct CallSegmentModel: Sendable {
    public static let magic: UInt32 = 0x4e4e_4d46

    public let version: Int
    public let input: Int
    public let output: Int
    public let outputs: [[Int]]
    public let shapes: [Int: CallSegmentShape]
    public let layers: [CallSegmentLayer]
    public let weights: [UInt16]

    public static func parse(_ data: Data) throws -> CallSegmentModel {
        let bytes = [UInt8](data)
        func u32(_ at: Int) -> UInt32 {
            UInt32(bytes[at]) | UInt32(bytes[at + 1]) << 8 | UInt32(bytes[at + 2]) << 16 | UInt32(bytes[at + 3]) << 24
        }
        guard bytes.count >= 12, u32(0) == magic else { throw CallSegmentError.notAModel }
        let length = Int(u32(8))
        guard 12 + length <= bytes.count,
            let header = try? JSONSerialization.jsonObject(with: Data(bytes[12..<(12 + length)])) as? [String: Any],
            let input = header["input"] as? Int,
            let output = header["output"] as? Int,
            let rawShapes = header["shapes"] as? [String: [Int]],
            let rawOps = header["ops"] as? [[String: Any]]
        else { throw CallSegmentError.badHeader }
        var shapes: [Int: CallSegmentShape] = [:]
        for (key, value) in rawShapes {
            guard let index = Int(key), value.count == 3 else { throw CallSegmentError.badHeader }
            shapes[index] = CallSegmentShape(value[0], value[1], value[2])
        }
        let start = 12 + length
        let count = (bytes.count - start) / 2
        var weights = [UInt16](repeating: 0, count: count)
        for i in 0..<count { weights[i] = UInt16(bytes[start + i * 2]) | UInt16(bytes[start + i * 2 + 1]) << 8 }
        var layers: [CallSegmentLayer] = []
        var next = (shapes.keys.max() ?? 0) + 1
        for raw in rawOps {
            guard let name = raw["op"] as? String, let inputs = raw["in"] as? [Int], let out = raw["out"] as? Int else { throw CallSegmentError.badHeader }
            let actName = raw["act"] as? String ?? "none"
            guard let act = activation(actName) else { throw CallSegmentError.unsupported(actName) }
            let w = raw["w"] as? [Int]
            let b = raw["b"] as? [Int]
            let a = raw["alpha"] as? [Int]
            func make(_ op: CallSegmentLayer.Op, inputs: [Int], output: Int, act: CallSegmentLayer.Activation, alpha: [Int]?) -> CallSegmentLayer {
                CallSegmentLayer(
                    op: op, inputs: inputs, output: output, act: act,
                    kernel: raw["kernel"] as? [Int] ?? [1, 1], stride: raw["stride"] as? [Int] ?? [1, 1],
                    pad: raw["pad"] as? [Int] ?? [0, 0], dilation: raw["dilation"] as? [Int] ?? [1, 1],
                    weights: w?.first ?? -1, bias: b?.first ?? -1, alpha: alpha?.first ?? -1, alphaCount: alpha?.last ?? 0,
                    cin: raw["cin"] as? Int ?? 0, cout: raw["cout"] as? Int ?? 0,
                    align: raw["align"] as? Bool ?? false, half: raw["half"] as? Bool ?? false)
            }
            if name == "mean" {
                guard let first = inputs.first, let shape = shapes[first] else { throw CallSegmentError.badHeader }
                let middle = next
                next += 1
                shapes[middle] = CallSegmentShape(shape.h, 1, shape.c)
                layers.append(make(.meanrow, inputs: [first], output: middle, act: .none, alpha: nil))
                layers.append(make(.meancol, inputs: [middle], output: out, act: act, alpha: nil))
                continue
            }
            guard let op = operation(name) else { throw CallSegmentError.unsupported(name) }
            layers.append(make(op, inputs: inputs, output: out, act: act, alpha: a))
        }
        for layer in layers {
            guard shapes[layer.output] != nil, layer.inputs.allSatisfy({ shapes[$0] != nil }) else { throw CallSegmentError.badHeader }
        }
        let outputs = header["outputs"] as? [[Int]] ?? [[output]]
        return CallSegmentModel(version: header["version"] as? Int ?? 0, input: input, output: output, outputs: outputs, shapes: shapes, layers: layers, weights: weights)
    }

    static func activation(_ name: String) -> CallSegmentLayer.Activation? {
        switch name {
        case "none": return CallSegmentLayer.Activation.none
        case "relu": return .relu
        case "prelu": return .prelu
        case "hswish": return .hswish
        case "sigmoid": return .sigmoid
        default: return nil
        }
    }

    static func operation(_ name: String) -> CallSegmentLayer.Op? {
        switch name {
        case "conv": return .conv
        case "tconv": return .tconv
        case "dw": return .dw
        case "add": return .add
        case "mul": return .mul
        case "resize": return .resize
        case "act": return .act
        case "argmax": return .argmax
        case "unpool": return .unpool
        case "concat": return .concat
        case "pad": return .pad
        case "maxpool": return .maxpool
        default: return nil
        }
    }

    public var inputShape: CallSegmentShape { shapes[input]! }
    public var outputShape: CallSegmentShape { shapes[output]! }

    public var multiplyAdds: Int {
        var total = 0
        for layer in layers {
            let out = shapes[layer.output]!
            switch layer.op {
            case .conv: total += out.count * layer.kernel[0] * layer.kernel[1] * layer.cin
            case .tconv: total += out.count * layer.kernel[0] * layer.kernel[1] * layer.cin / max(1, layer.stride[0] * layer.stride[1])
            case .dw: total += out.count * layer.kernel[0] * layer.kernel[1]
            default: total += out.count
            }
        }
        return total
    }
}

public struct CallSegmentStep: Equatable, Sendable {
    public static let fieldCount = 32

    public let layer: CallSegmentLayer
    public let input: Int
    public let second: Int
    public let output: Int
    public let count: Int
    public let fields: [Int32]
}

public struct CallSegmentPlan: Sendable {
    public static let alignment = 64

    public let model: CallSegmentModel
    public let offsets: [Int: Int]
    public let arenaCount: Int
    public let steps: [CallSegmentStep]

    public init(_ model: CallSegmentModel) {
        self.model = model
        var offsets: [Int: Int] = [:]
        var at = 0
        for index in model.shapes.keys.sorted() {
            offsets[index] = at
            let count = model.shapes[index]!.count
            at += (count + Self.alignment - 1) / Self.alignment * Self.alignment
        }
        self.offsets = offsets
        arenaCount = at
        steps = model.layers.map { layer in
            let a = model.shapes[layer.inputs[0]]!
            let b = layer.inputs.count > 1 ? model.shapes[layer.inputs[1]]! : CallSegmentShape(0, 0, 0)
            let o = model.shapes[layer.output]!
            let scale = Self.resizeScale(layer, a, o)
            let broadcast = layer.inputs.count > 1 && b.h == 1 && b.w == 1 ? 1 : 0
            var fields = [Int32](repeating: 0, count: CallSegmentStep.fieldCount)
            let values: [Int] = [
                Int(layer.op.rawValue), Int(layer.act.rawValue), a.h, a.w, a.c, o.h, o.w, o.c, b.h, b.w, b.c,
                layer.kernel[0], layer.kernel[1], layer.stride[0], layer.stride[1], layer.pad[0], layer.pad[1],
                layer.dilation[0], layer.dilation[1], layer.weights, layer.bias, layer.alpha, layer.cin,
                layer.align ? 1 : 0, layer.half ? 1 : 0, broadcast,
            ]
            for (i, value) in values.enumerated() { fields[i] = Int32(value) }
            fields[26] = Int32(bitPattern: scale.0.bitPattern)
            fields[27] = Int32(bitPattern: scale.1.bitPattern)
            fields[28] = Int32(o.count)
            return CallSegmentStep(
                layer: layer, input: offsets[layer.inputs[0]]!, second: layer.inputs.count > 1 ? offsets[layer.inputs[1]]! : 0,
                output: offsets[layer.output]!, count: o.count, fields: fields)
        }
    }

    public static func resizeScale(_ layer: CallSegmentLayer, _ a: CallSegmentShape, _ o: CallSegmentShape) -> (Float, Float) {
        guard layer.op == .resize else { return (0, 0) }
        func fixed(_ value: Double) -> Float { Float((value * 1e6).rounded() / 1e6) }
        return layer.align
            ? (fixed(Double(a.w - 1) / Double(max(1, o.w - 1))), fixed(Double(a.h - 1) / Double(max(1, o.h - 1))))
            : (fixed(Double(a.w) / Double(o.w)), fixed(Double(a.h) / Double(o.h)))
    }

    public var inputOffset: Int { offsets[model.input]! }
    public var outputOffset: Int { offsets[model.output]! }
}
