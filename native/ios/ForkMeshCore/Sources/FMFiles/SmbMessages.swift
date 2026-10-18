import Foundation

public enum SmbCommand: UInt16, Sendable {
    case negotiate = 0, sessionSetup = 1, logoff = 2, treeConnect = 3, treeDisconnect = 4
    case create = 5, close = 6, flush = 7, read = 8, write = 9, lock = 10, ioctl = 11
    case cancel = 12, echo = 13, queryDirectory = 14, changeNotify = 15, queryInfo = 16, setInfo = 17
}

public enum SmbFlags {
    public static let serverToRedirector: UInt32 = 0x1
    public static let async: UInt32 = 0x2
    public static let signed: UInt32 = 0x8
}

public enum SmbAccess {
    public static let readData: UInt32 = 0x1
    public static let writeData: UInt32 = 0x2
    public static let appendData: UInt32 = 0x4
    public static let readEa: UInt32 = 0x8
    public static let writeEa: UInt32 = 0x10
    public static let readAttributes: UInt32 = 0x80
    public static let writeAttributes: UInt32 = 0x100
    public static let delete: UInt32 = 0x10000
    public static let readControl: UInt32 = 0x20000
    public static let synchronize: UInt32 = 0x100000
    public static let listFolder: UInt32 = readData | readAttributes | synchronize
    public static let readFile: UInt32 = readData | readEa | readAttributes | readControl | synchronize
    public static let writeFile: UInt32 = writeData | appendData | writeEa | readAttributes | writeAttributes | readControl | synchronize
    public static let pipe: UInt32 = readData | writeData | readEa | writeEa | readAttributes | writeAttributes | readControl | synchronize
}

public enum SmbDisposition {
    public static let open: UInt32 = 1
    public static let create: UInt32 = 2
    public static let openIf: UInt32 = 3
    public static let overwriteIf: UInt32 = 5
}

public enum SmbOptions {
    public static let directory: UInt32 = 0x1
    public static let nonDirectory: UInt32 = 0x40
    public static let deleteOnClose: UInt32 = 0x1000
}

public enum SmbAttributes {
    public static let readOnly: UInt32 = 0x1
    public static let hidden: UInt32 = 0x2
    public static let directory: UInt32 = 0x10
    public static let normal: UInt32 = 0x80
}

public struct SmbHeader: Equatable, Sendable {
    public var command: UInt16
    public var status: UInt32
    public var flags: UInt32
    public var messageId: UInt64
    public var treeId: UInt32
    public var sessionId: UInt64
    public var creditCharge: UInt16
    public var credits: UInt16

    public static let size = 64

    public static func parse(_ message: [UInt8]) -> SmbHeader? {
        let reader = SmbReader(message)
        guard message.count >= size, reader.slice(0, 4) == [0xfe, 0x53, 0x4d, 0x42] else { return nil }
        return SmbHeader(
            command: reader.u16(12), status: reader.u32(8), flags: reader.u32(16), messageId: reader.u64(24),
            treeId: reader.u32(36), sessionId: reader.u64(40), creditCharge: reader.u16(6), credits: reader.u16(14))
    }

    public var isAsync: Bool { flags & SmbFlags.async != 0 }
}

public struct SmbFileId: Equatable, Hashable, Sendable {
    public var bytes: [UInt8]
    public init(_ bytes: [UInt8]) { self.bytes = bytes.count == 16 ? bytes : [UInt8](repeating: 0, count: 16) }
}

public struct SmbCreated: Equatable, Sendable {
    public var fileId: SmbFileId
    public var attributes: UInt32
    public var size: UInt64
    public var modified: UInt64
}

public struct SmbNegotiated: Equatable, Sendable {
    public var dialect: UInt16
    public var signingRequired: Bool
    public var maxRead: UInt32
    public var maxWrite: UInt32
    public var maxTransact: UInt32
    public var securityBuffer: [UInt8]
}

public struct SmbDirectoryEntry: Equatable, Sendable {
    public var name: String
    public var attributes: UInt32
    public var size: UInt64
    public var modified: UInt64
}

public enum SmbMessages {
    public static let dialects: [UInt16] = [0x0202, 0x0210]

    public static func header(
        _ command: SmbCommand, messageId: UInt64, sessionId: UInt64, treeId: UInt32, creditCharge: UInt16, credits: UInt16 = 64
    ) -> SmbWriter {
        var writer = SmbWriter()
        writer.append([0xfe, 0x53, 0x4d, 0x42])
        writer.u16(64)
        writer.u16(creditCharge)
        writer.u32(0)
        writer.u16(command.rawValue)
        writer.u16(credits)
        writer.u32(0)
        writer.u32(0)
        writer.u64(messageId)
        writer.u32(0)
        writer.u32(treeId)
        writer.u64(sessionId)
        writer.zeros(16)
        return writer
    }

    public static func negotiateBody(clientGuid: [UInt8]) -> [UInt8] {
        var writer = SmbWriter()
        writer.u16(36)
        writer.u16(UInt16(dialects.count))
        writer.u16(1)
        writer.u16(0)
        writer.u32(0)
        writer.append(clientGuid.count == 16 ? clientGuid : [UInt8](repeating: 0, count: 16))
        writer.u64(0)
        for dialect in dialects { writer.u16(dialect) }
        return writer.bytes
    }

    public static func parseNegotiate(_ message: [UInt8]) -> SmbNegotiated? {
        let reader = SmbReader(message)
        let body = SmbHeader.size
        guard reader.u16(body) == 65 else { return nil }
        let offset = Int(reader.u16(body + 56))
        let length = Int(reader.u16(body + 58))
        return SmbNegotiated(
            dialect: reader.u16(body + 4), signingRequired: reader.u16(body + 2) & 0x2 != 0,
            maxRead: reader.u32(body + 32), maxWrite: reader.u32(body + 36), maxTransact: reader.u32(body + 28),
            securityBuffer: reader.slice(offset, length))
    }

    public static func sessionSetupBody(_ token: [UInt8]) -> [UInt8] {
        var writer = SmbWriter()
        writer.u16(25)
        writer.u8(0)
        writer.u8(1)
        writer.u32(0)
        writer.u32(0)
        writer.u16(UInt16(SmbHeader.size + 24))
        writer.u16(UInt16(token.count))
        writer.u64(0)
        writer.append(token)
        return writer.bytes
    }

    public static func parseSessionSetup(_ message: [UInt8]) -> (flags: UInt16, token: [UInt8]) {
        let reader = SmbReader(message)
        let body = SmbHeader.size
        return (reader.u16(body + 2), reader.slice(Int(reader.u16(body + 4)), Int(reader.u16(body + 6))))
    }

    public static func treeConnectBody(_ path: String) -> [UInt8] {
        let name = SmbText.utf16(path)
        var writer = SmbWriter()
        writer.u16(9)
        writer.u16(0)
        writer.u16(UInt16(SmbHeader.size + 8))
        writer.u16(UInt16(name.count))
        writer.append(name)
        return writer.bytes
    }

    public static func createBody(_ path: String, access: UInt32, attributes: UInt32 = 0, share: UInt32 = 7, disposition: UInt32, options: UInt32) -> [UInt8] {
        let name = SmbText.utf16(path)
        var writer = SmbWriter()
        writer.u16(57)
        writer.u8(0)
        writer.u8(0)
        writer.u32(2)
        writer.u64(0)
        writer.u64(0)
        writer.u32(access)
        writer.u32(attributes)
        writer.u32(share)
        writer.u32(disposition)
        writer.u32(options)
        writer.u16(UInt16(SmbHeader.size + 56))
        writer.u16(UInt16(name.count))
        writer.u32(0)
        writer.u32(0)
        writer.append(name.isEmpty ? [0] : name)
        return writer.bytes
    }

    public static func parseCreate(_ message: [UInt8]) -> SmbCreated {
        let reader = SmbReader(message)
        let body = SmbHeader.size
        return SmbCreated(
            fileId: SmbFileId(reader.slice(body + 64, 16)), attributes: reader.u32(body + 56),
            size: reader.u64(body + 48), modified: reader.u64(body + 24))
    }

    public static func closeBody(_ file: SmbFileId) -> [UInt8] {
        var writer = SmbWriter()
        writer.u16(24)
        writer.u16(0)
        writer.u32(0)
        writer.append(file.bytes)
        return writer.bytes
    }

    public static func readBody(_ file: SmbFileId, offset: UInt64, length: UInt32) -> [UInt8] {
        var writer = SmbWriter()
        writer.u16(49)
        writer.u8(0x50)
        writer.u8(0)
        writer.u32(length)
        writer.u64(offset)
        writer.append(file.bytes)
        writer.u32(0)
        writer.u32(0)
        writer.u32(0)
        writer.u16(0)
        writer.u16(0)
        writer.u8(0)
        return writer.bytes
    }

    public static func parseRead(_ message: [UInt8]) -> [UInt8] {
        let reader = SmbReader(message)
        let body = SmbHeader.size
        return reader.slice(Int(reader.u8(body + 2)), Int(reader.u32(body + 4)))
    }

    public static func writeBody(_ file: SmbFileId, offset: UInt64, data: [UInt8]) -> [UInt8] {
        var writer = SmbWriter()
        writer.u16(49)
        writer.u16(UInt16(SmbHeader.size + 48))
        writer.u32(UInt32(data.count))
        writer.u64(offset)
        writer.append(file.bytes)
        writer.u32(0)
        writer.u32(0)
        writer.u16(0)
        writer.u16(0)
        writer.u32(0)
        writer.append(data)
        return writer.bytes
    }

    public static func parseWrite(_ message: [UInt8]) -> UInt32 { SmbReader(message).u32(SmbHeader.size + 4) }

    public static func queryDirectoryBody(_ file: SmbFileId, restart: Bool, pattern: String = "*", outputLength: UInt32 = 65536) -> [UInt8] {
        let name = SmbText.utf16(pattern)
        var writer = SmbWriter()
        writer.u16(33)
        writer.u8(1)
        writer.u8(restart ? 1 : 0)
        writer.u32(0)
        writer.append(file.bytes)
        writer.u16(UInt16(SmbHeader.size + 32))
        writer.u16(UInt16(name.count))
        writer.u32(outputLength)
        writer.append(name)
        return writer.bytes
    }

    public static func parseQueryDirectory(_ message: [UInt8]) -> [SmbDirectoryEntry] {
        let reader = SmbReader(message)
        let body = SmbHeader.size
        let buffer = SmbReader(reader.slice(Int(reader.u16(body + 2)), Int(reader.u32(body + 4))))
        var entries: [SmbDirectoryEntry] = []
        var offset = 0
        while buffer.has(offset, 64) {
            let next = Int(buffer.u32(offset))
            let nameLength = Int(buffer.u32(offset + 60))
            let name = SmbText.string(buffer.slice(offset + 64, nameLength))
            entries.append(SmbDirectoryEntry(name: name, attributes: buffer.u32(offset + 56), size: buffer.u64(offset + 40), modified: buffer.u64(offset + 24)))
            if next == 0 { break }
            offset += next
        }
        return entries
    }

    public static func renameBody(_ file: SmbFileId, to path: String) -> [UInt8] {
        let name = SmbText.utf16(path)
        var info = SmbWriter()
        info.u8(0)
        info.zeros(7)
        info.u64(0)
        info.u32(UInt32(name.count))
        info.append(name)
        var writer = SmbWriter()
        writer.u16(33)
        writer.u8(1)
        writer.u8(10)
        writer.u32(UInt32(info.count))
        writer.u16(UInt16(SmbHeader.size + 32))
        writer.u16(0)
        writer.u32(0)
        writer.append(file.bytes)
        writer.append(info.bytes)
        return writer.bytes
    }

    public static func frame(_ message: [UInt8]) -> [UInt8] {
        let length = message.count
        return [0, UInt8((length >> 16) & 0xff), UInt8((length >> 8) & 0xff), UInt8(length & 0xff)] + message
    }

    public static func sign(_ message: [UInt8], key: [UInt8]) -> [UInt8] {
        var signed = message
        let flags = SmbReader(signed).u32(16) | SmbFlags.signed
        for index in 0..<4 { signed[16 + index] = UInt8((flags >> UInt32(index * 8)) & 0xff) }
        for index in 48..<64 { signed[index] = 0 }
        let mac = SmbSigning.hmac(key: key, signed)
        for index in 0..<16 { signed[48 + index] = mac[index] }
        return signed
    }
}
