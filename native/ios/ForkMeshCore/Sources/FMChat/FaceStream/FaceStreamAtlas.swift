import Foundation

public struct FaceStreamAtlasTile: Equatable, Sendable {
    public var state: Int
    public var mouth: Double
    public var smile: Double
    public var points: [CallFacePoint]

    public init(state: Int, mouth: Double, smile: Double, points: [CallFacePoint]) {
        self.state = state
        self.mouth = mouth
        self.smile = smile
        self.points = points
    }
}

public struct FaceStreamAtlas: Equatable, Sendable {
    public static let closed = 0
    public static let open = 1
    public static let wide = 2
    public static let round = 3
    public static let minSide = 8
    public static let maxSide = 128
    public static let maxTiles = 8
    public static let maxPacket = 64 * 1024
    public static let pointCount = 20
    static let fixed = 9
    static let tileBytes = 3 + pointCount * 4
    static let steps = 16.0

    public var id: Int
    public var time: UInt32
    public var tileW: Int
    public var tileH: Int
    public var format: Int
    public var tiles: [FaceStreamAtlasTile]
    public var image: [UInt8]

    public init(id: Int, time: UInt32, tileW: Int, tileH: Int, format: Int, tiles: [FaceStreamAtlasTile], image: [UInt8]) {
        self.id = id
        self.time = time
        self.tileW = tileW
        self.tileH = tileH
        self.format = format
        self.tiles = tiles
        self.image = image
    }

    var valid: Bool {
        guard (0...15).contains(id), (0...2).contains(format), (Self.minSide...Self.maxSide).contains(tileW), (Self.minSide...Self.maxSide).contains(tileH) else { return false }
        guard (1...Self.maxTiles).contains(tiles.count), tiles.allSatisfy({ (Self.closed...Self.round).contains($0.state) }) else { return false }
        return !image.isEmpty
    }

    public func encoded() throws -> [UInt8] {
        guard valid else { throw FaceStreamError.input }
        var out: [UInt8] = [FaceStream.header(FaceStream.kindAtlas, id)]
        out += [UInt8(time >> 24), UInt8((time >> 16) & 255), UInt8((time >> 8) & 255), UInt8(time & 255)]
        out += [UInt8(tileW), UInt8(tileH), UInt8(tiles.count), UInt8(format)]
        let top = Int(Self.steps)
        for tile in tiles {
            guard tile.mouth.isFinite, tile.smile.isFinite, tile.points.count >= Self.pointCount else { throw FaceStreamError.input }
            out += [UInt8(tile.state), UInt8(faceStreamClamp(FaceStream.floorHalf(tile.mouth * Self.steps), 0, top)), UInt8(faceStreamClamp(FaceStream.floorHalf(tile.smile * Self.steps), 0, top))]
            for p in tile.points.prefix(Self.pointCount) {
                for v in [p.x, p.y] {
                    guard v.isFinite else { throw FaceStreamError.input }
                    let q = UInt16(bitPattern: Int16(faceStreamClamp(FaceStream.floorHalf(v * Self.steps), -32768, 32767)))
                    out += [UInt8(q >> 8), UInt8(q & 255)]
                }
            }
        }
        out += image
        guard out.count <= Self.maxPacket else { throw FaceStreamError.long }
        return out
    }

    public static func decode(_ packet: [UInt8]) throws -> FaceStreamAtlas {
        guard packet.count >= fixed + tileBytes + 1 else { throw FaceStreamError.short }
        guard packet.count <= maxPacket else { throw FaceStreamError.long }
        guard Int(packet[0] >> 4) & 3 == FaceStream.version else { throw FaceStreamError.version }
        guard Int(packet[0] >> 6) == FaceStream.kindAtlas else { throw FaceStreamError.notAnAtlas }
        let count = Int(packet[7])
        guard (1...maxTiles).contains(count), packet.count > fixed + count * tileBytes else { throw FaceStreamError.corrupt }
        var at = fixed
        var tiles: [FaceStreamAtlasTile] = []
        for _ in 0..<count {
            let state = Int(packet[at])
            let mouth = Int(packet[at + 1])
            let smile = Int(packet[at + 2])
            guard Double(mouth) <= steps, Double(smile) <= steps else { throw FaceStreamError.corrupt }
            at += 3
            var points: [CallFacePoint] = []
            for _ in 0..<pointCount {
                let x = Double(Int16(bitPattern: UInt16(packet[at]) << 8 | UInt16(packet[at + 1]))) / steps
                let y = Double(Int16(bitPattern: UInt16(packet[at + 2]) << 8 | UInt16(packet[at + 3]))) / steps
                at += 4
                points.append(CallFacePoint(x, y))
            }
            tiles.append(FaceStreamAtlasTile(state: state, mouth: Double(mouth) / steps, smile: Double(smile) / steps, points: points))
        }
        let time = UInt32(packet[1]) << 24 | UInt32(packet[2]) << 16 | UInt32(packet[3]) << 8 | UInt32(packet[4])
        let atlas = FaceStreamAtlas(id: Int(packet[0] & 15), time: time, tileW: Int(packet[5]), tileH: Int(packet[6]), format: Int(packet[8]), tiles: tiles,
            image: Array(packet[at...]))
        guard atlas.valid else { throw FaceStreamError.corrupt }
        return atlas
    }
}
