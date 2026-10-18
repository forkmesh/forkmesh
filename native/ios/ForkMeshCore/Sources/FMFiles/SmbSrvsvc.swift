import Foundation

public struct SmbShareInfo: Equatable, Sendable, Hashable {
    public var name: String
    public var type: UInt32
    public var remark: String

    public init(name: String, type: UInt32, remark: String) {
        self.name = name
        self.type = type
        self.remark = remark
    }

    public var isBrowsableDisk: Bool { type & 0x0fff_ffff == 0 && type & 0x8000_0000 == 0 && !name.hasSuffix("$") }
}

public enum SmbSrvsvc {
    static let srvsvcSyntax: [UInt8] = [0xc8, 0x4f, 0x32, 0x4b, 0x70, 0x16, 0xd3, 0x01, 0x12, 0x78, 0x5a, 0x47, 0xbf, 0x6e, 0xe1, 0x88]
    static let ndrSyntax: [UInt8] = [0x04, 0x5d, 0x88, 0x8a, 0xeb, 0x1c, 0xc9, 0x11, 0x9f, 0xe8, 0x08, 0x00, 0x2b, 0x10, 0x48, 0x60]

    static func pdu(type: UInt8, callId: UInt32, body: [UInt8]) -> [UInt8] {
        var writer = SmbWriter()
        writer.u8(5)
        writer.u8(0)
        writer.u8(type)
        writer.u8(3)
        writer.append([0x10, 0, 0, 0])
        writer.u16(UInt16(16 + body.count))
        writer.u16(0)
        writer.u32(callId)
        writer.append(body)
        return writer.bytes
    }

    public static func bind() -> [UInt8] {
        var body = SmbWriter()
        body.u16(4280)
        body.u16(4280)
        body.u32(0)
        body.u8(1)
        body.zeros(3)
        body.u16(0)
        body.u8(1)
        body.u8(0)
        body.append(srvsvcSyntax)
        body.u16(3)
        body.u16(0)
        body.append(ndrSyntax)
        body.u32(2)
        return pdu(type: 11, callId: 1, body: body.bytes)
    }

    public static func bindAccepted(_ reply: [UInt8]) -> Bool {
        let reader = SmbReader(reply)
        guard reader.u8(0) == 5, reader.u8(2) == 12 else { return false }
        var offset = 24
        let secondary = Int(reader.u16(offset))
        offset += 2 + secondary
        offset = (offset + 3) & ~3
        guard reader.u8(offset) >= 1 else { return false }
        return reader.u16(offset + 4) == 0
    }

    static func ndrString(_ writer: inout SmbWriter, _ text: String) {
        let units = SmbText.utf16(text) + [0, 0]
        let count = UInt32(units.count / 2)
        writer.u32(count)
        writer.u32(0)
        writer.u32(count)
        writer.append(units)
        writer.align(4)
    }

    public static func shareEnumRequest(host: String) -> [UInt8] {
        var stub = SmbWriter()
        stub.u32(0x0002_0000)
        ndrString(&stub, "\\\\" + host)
        stub.u32(1)
        stub.u32(1)
        stub.u32(0x0002_0004)
        stub.u32(0)
        stub.u32(0)
        stub.u32(0xffff_ffff)
        stub.u32(0x0002_0008)
        stub.u32(0)
        var body = SmbWriter()
        body.u32(UInt32(stub.count))
        body.u16(0)
        body.u16(15)
        body.append(stub.bytes)
        return pdu(type: 0, callId: 2, body: body.bytes)
    }

    public static func fragments(_ data: [UInt8]) -> (stub: [UInt8], complete: Bool, fault: Bool) {
        let reader = SmbReader(data)
        var stub: [UInt8] = []
        var offset = 0
        while reader.has(offset, 24) {
            let length = Int(reader.u16(offset + 8))
            guard length >= 24, reader.has(offset, length) else { return (stub, false, false) }
            let type = reader.u8(offset + 2)
            if type == 3 { return (stub, true, true) }
            let authLength = Int(reader.u16(offset + 10))
            let end = length - (authLength > 0 ? authLength + 8 : 0)
            stub += reader.slice(offset + 24, max(0, end - 24))
            if reader.u8(offset + 3) & 0x2 != 0 { return (stub, true, false) }
            offset += length
        }
        return (stub, false, false)
    }

    public static func parseShareEnum(_ stub: [UInt8]) -> [SmbShareInfo]? {
        let reader = SmbReader(stub)
        var offset = 0
        func next() -> UInt32 {
            let value = reader.u32(offset)
            offset += 4
            return value
        }
        func string() -> String {
            let maxCount = Int(next())
            _ = next()
            let actual = Int(next())
            guard maxCount >= 0, reader.has(offset, actual * 2) else { return "" }
            let text = SmbText.string(reader.slice(offset, actual * 2))
            offset += actual * 2
            offset = (offset + 3) & ~3
            return text
        }
        guard reader.has(0, 20) else { return nil }
        _ = next()
        _ = next()
        guard next() != 0 else { return [] }
        let count = Int(next())
        guard next() != 0, count >= 0, count < 10_000 else { return [] }
        _ = next()
        var raw: [(name: UInt32, type: UInt32, remark: UInt32)] = []
        for _ in 0..<count {
            guard reader.has(offset, 12) else { return nil }
            raw.append((next(), next(), next()))
        }
        var shares: [SmbShareInfo] = []
        for entry in raw {
            let name = entry.name != 0 ? string() : ""
            let remark = entry.remark != 0 ? string() : ""
            shares.append(SmbShareInfo(name: name, type: entry.type, remark: remark))
        }
        return shares
    }
}
