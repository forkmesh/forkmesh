import Foundation

public struct FaceStreamReference: Equatable, Sendable {
    public static let jpeg = 0
    public static let heic = 1
    public static let webp = 2
    public static let maxSide = 1024
    public static let maxMaskSide = 128
    public static let maxPacket = 256 * 1024
    static let fixed = 1 + 4 + 12 + 68 * 4 + 3 + 2
    static let pointSteps = 16.0

    public var id: Int
    public var format: Int
    public var time: UInt32
    public var cropX: Int
    public var cropY: Int
    public var cropW: Int
    public var cropH: Int
    public var width: Int
    public var height: Int
    public var points: [CallFacePoint]
    public var maskW: Int
    public var maskH: Int
    public var mask: [UInt8]?
    public var image: [UInt8]

    public init(id: Int, format: Int, time: UInt32, cropX: Int, cropY: Int, cropW: Int, cropH: Int, width: Int, height: Int,
                points: [CallFacePoint], maskW: Int = 0, maskH: Int = 0, mask: [UInt8]? = nil, image: [UInt8]) {
        self.id = id
        self.format = format
        self.time = time
        self.cropX = cropX
        self.cropY = cropY
        self.cropW = cropW
        self.cropH = cropH
        self.width = width
        self.height = height
        self.points = points
        self.maskW = maskW
        self.maskH = maskH
        self.mask = mask
        self.image = image
    }

    var valid: Bool {
        guard (0...15).contains(id), (0...2).contains(format), (1...Self.maxSide).contains(width), (1...Self.maxSide).contains(height) else { return false }
        guard cropW >= 1, cropH >= 1, cropX >= 0, cropY >= 0, cropX + cropW <= 65535, cropY + cropH <= 65535 else { return false }
        guard (maskW == 0) == (maskH == 0), (0...Self.maxMaskSide).contains(maskW), (0...Self.maxMaskSide).contains(maskH) else { return false }
        return !image.isEmpty
    }

    static func context(_ bits: [UInt8], _ w: Int, _ x: Int, _ y: Int) -> Int {
        func at(_ px: Int, _ py: Int) -> Int { px < 0 || py < 0 || px >= w ? 0 : Int(bits[py * w + px]) }
        return at(x - 1, y) | at(x, y - 1) << 1 | at(x - 1, y - 1) << 2 | at(x + 1, y - 1) << 3
    }

    static func encodeMask(_ mask: [UInt8], _ w: Int, _ h: Int) -> [UInt8] {
        var models = [UInt16](repeating: FaceStreamBits.probHalf, count: 16)
        let bits = mask.prefix(w * h).map { $0 != 0 ? UInt8(1) : 0 }
        var enc = FaceStreamRangeEncoder()
        for y in 0..<h {
            for x in 0..<w { enc.bit(&models, context(bits, w, x, y), Int(bits[y * w + x])) }
        }
        return enc.finish()
    }

    static func decodeMask(_ data: [UInt8], _ w: Int, _ h: Int) -> [UInt8] {
        var models = [UInt16](repeating: FaceStreamBits.probHalf, count: 16)
        var out = [UInt8](repeating: 0, count: w * h)
        var dec = FaceStreamRangeDecoder(data)
        for y in 0..<h {
            for x in 0..<w { out[y * w + x] = UInt8(dec.bit(&models, context(out, w, x, y))) }
        }
        return out
    }

    public func encoded() throws -> [UInt8] {
        guard valid, points.count >= 68, maskW == 0 || (mask?.count ?? 0) >= maskW * maskH else { throw FaceStreamError.input }
        var out: [UInt8] = [FaceStream.header(FaceStream.kindReference, id)]
        out += [UInt8(time >> 24), UInt8((time >> 16) & 255), UInt8((time >> 8) & 255), UInt8(time & 255)]
        for v in [cropX, cropY, cropW, cropH, width, height] { out += [UInt8((v >> 8) & 255), UInt8(v & 255)] }
        for p in points.prefix(68) {
            for v in [p.x, p.y] {
                guard v.isFinite else { throw FaceStreamError.input }
                let q = UInt16(bitPattern: Int16(faceStreamClamp(FaceStream.floorHalf(v * Self.pointSteps), -32768, 32767)))
                out += [UInt8(q >> 8), UInt8(q & 255)]
            }
        }
        out += [UInt8(format), UInt8(maskW), UInt8(maskH)]
        let coded = maskW > 0 ? Self.encodeMask(mask ?? [], maskW, maskH) : []
        out += [UInt8(coded.count >> 8), UInt8(coded.count & 255)]
        out += coded
        out += image
        guard out.count <= Self.maxPacket else { throw FaceStreamError.long }
        return out
    }

    public static func decode(_ packet: [UInt8]) throws -> FaceStreamReference {
        guard packet.count >= fixed + 1 else { throw FaceStreamError.short }
        guard packet.count <= maxPacket else { throw FaceStreamError.long }
        guard Int(packet[0] >> 4) & 3 == FaceStream.version else { throw FaceStreamError.version }
        guard Int(packet[0] >> 6) == FaceStream.kindReference else { throw FaceStreamError.notAReference }
        var at = 5
        func u16() -> Int {
            let v = Int(packet[at]) << 8 | Int(packet[at + 1])
            at += 2
            return v
        }
        let time = UInt32(packet[1]) << 24 | UInt32(packet[2]) << 16 | UInt32(packet[3]) << 8 | UInt32(packet[4])
        let cropX = u16(), cropY = u16(), cropW = u16(), cropH = u16(), width = u16(), height = u16()
        var points: [CallFacePoint] = []
        for _ in 0..<68 {
            let x = Double(Int16(bitPattern: UInt16(u16()))) / pointSteps
            let y = Double(Int16(bitPattern: UInt16(u16()))) / pointSteps
            points.append(CallFacePoint(x, y))
        }
        let format = Int(packet[at])
        let maskW = Int(packet[at + 1])
        let maskH = Int(packet[at + 2])
        at += 3
        let maskLength = u16()
        guard at + maskLength < packet.count else { throw FaceStreamError.corrupt }
        var mask: [UInt8]?
        if maskW > 0 && maskH > 0 {
            mask = decodeMask(Array(packet[at..<(at + maskLength)]), maskW, maskH)
        } else if maskLength != 0 {
            throw FaceStreamError.corrupt
        }
        at += maskLength
        let reference = FaceStreamReference(id: Int(packet[0] & 15), format: format, time: time, cropX: cropX, cropY: cropY, cropW: cropW, cropH: cropH,
            width: width, height: height, points: points, maskW: maskW, maskH: maskH, mask: mask, image: Array(packet[at...]))
        guard reference.valid else { throw FaceStreamError.corrupt }
        return reference
    }
}
