import Foundation

public struct NtlmChallenge: Equatable, Sendable {
    public var flags: UInt32
    public var serverChallenge: [UInt8]
    public var targetInfo: [UInt8]
    public var timestamp: UInt64?
}

public struct NtlmAuthentication: Equatable, Sendable {
    public var message: [UInt8]
    public var sessionKey: [UInt8]
}

public enum Ntlm {
    public static let signature: [UInt8] = Array("NTLMSSP".utf8) + [0]
    public static let negotiateUnicode: UInt32 = 0x0000_0001
    public static let requestTarget: UInt32 = 0x0000_0004
    public static let negotiateNtlm: UInt32 = 0x0000_0200
    public static let negotiateAnonymous: UInt32 = 0x0000_0800
    public static let alwaysSign: UInt32 = 0x0000_8000
    public static let extendedSessionSecurity: UInt32 = 0x0008_0000
    public static let targetInfoFlag: UInt32 = 0x0080_0000
    public static let negotiate128: UInt32 = 0x2000_0000
    public static let negotiate56: UInt32 = 0x8000_0000
    public static let clientFlags: UInt32 = negotiateUnicode | requestTarget | negotiateNtlm | alwaysSign
        | extendedSessionSecurity | targetInfoFlag | negotiate128 | negotiate56
    public static let workstation = "FORKMESH"

    public static func negotiateMessage(anonymous: Bool = false) -> [UInt8] {
        var writer = SmbWriter()
        writer.append(signature)
        writer.u32(1)
        writer.u32(clientFlags | (anonymous ? negotiateAnonymous : 0))
        writer.zeros(16)
        return writer.bytes
    }

    public static func find(_ buffer: [UInt8]) -> [UInt8]? {
        guard buffer.count >= signature.count else { return nil }
        for start in 0...(buffer.count - signature.count) where Array(buffer[start..<(start + signature.count)]) == signature {
            return Array(buffer[start...])
        }
        return nil
    }

    public static func parseChallenge(_ buffer: [UInt8]) -> NtlmChallenge? {
        guard let message = find(buffer) else { return nil }
        let reader = SmbReader(message)
        guard reader.u32(8) == 2, reader.has(24, 8) else { return nil }
        let infoLength = Int(reader.u16(40))
        let infoOffset = Int(reader.u32(44))
        let info = reader.has(infoOffset, infoLength) ? reader.slice(infoOffset, infoLength) : []
        return NtlmChallenge(flags: reader.u32(20), serverChallenge: reader.slice(24, 8), targetInfo: info, timestamp: timestamp(in: info))
    }

    public static func timestamp(in info: [UInt8]) -> UInt64? {
        let reader = SmbReader(info)
        var offset = 0
        while reader.has(offset, 4) {
            let id = reader.u16(offset)
            let length = Int(reader.u16(offset + 2))
            if id == 0 { return nil }
            if id == 7, length == 8 { return reader.u64(offset + 4) }
            offset += 4 + length
        }
        return nil
    }

    public static func ntHash(_ password: String) -> [UInt8] { Md4.hash(SmbText.utf16(password)) }

    public static func ntowfv2(password: String, user: String, domain: String) -> [UInt8] {
        HmacMd5.mac(key: ntHash(password), SmbText.utf16(user.uppercased() + domain))
    }

    public static func blob(timestamp: UInt64, clientChallenge: [UInt8], targetInfo: [UInt8]) -> [UInt8] {
        var writer = SmbWriter()
        writer.append([1, 1, 0, 0, 0, 0, 0, 0])
        writer.u64(timestamp)
        writer.append(clientChallenge)
        writer.zeros(4)
        writer.append(targetInfo)
        writer.zeros(4)
        return writer.bytes
    }

    public static func ntProof(ntowfv2: [UInt8], serverChallenge: [UInt8], blob: [UInt8]) -> [UInt8] {
        HmacMd5.mac(key: ntowfv2, serverChallenge, blob)
    }

    public static func fileTime(_ date: Date) -> UInt64 {
        UInt64(max(0, (date.timeIntervalSince1970 + 11_644_473_600) * 10_000_000))
    }

    public static func date(fileTime: UInt64) -> Date? {
        guard fileTime > 0 else { return nil }
        return Date(timeIntervalSince1970: Double(fileTime) / 10_000_000 - 11_644_473_600)
    }

    public static func splitUser(_ typed: String) -> (domain: String, user: String) {
        let trimmed = typed.trimmingCharacters(in: .whitespaces)
        if let slash = trimmed.firstIndex(of: "\\") {
            return (String(trimmed[..<slash]), String(trimmed[trimmed.index(after: slash)...]))
        }
        return ("", trimmed)
    }

    public static func authenticate(
        challenge: NtlmChallenge, user typed: String, password: String,
        clientChallenge: [UInt8], now: Date = Date(), anonymous: Bool = false
    ) -> NtlmAuthentication {
        let (domain, user) = splitUser(typed)
        var lm: [UInt8] = []
        var nt: [UInt8] = []
        var sessionKey: [UInt8] = []
        var flags = clientFlags
        if anonymous {
            flags |= negotiateAnonymous
        } else {
            let key = ntowfv2(password: password, user: user, domain: domain)
            let temp = blob(timestamp: challenge.timestamp ?? fileTime(now), clientChallenge: clientChallenge, targetInfo: challenge.targetInfo)
            let proof = ntProof(ntowfv2: key, serverChallenge: challenge.serverChallenge, blob: temp)
            nt = proof + temp
            lm = [UInt8](repeating: 0, count: 24)
            sessionKey = HmacMd5.mac(key: key, proof)
        }
        let fields: [[UInt8]] = [lm, nt, SmbText.utf16(anonymous ? "" : domain), SmbText.utf16(anonymous ? "" : user), SmbText.utf16(workstation), []]
        var writer = SmbWriter()
        writer.append(signature)
        writer.u32(3)
        var offset = 64
        for field in fields {
            writer.u16(UInt16(field.count))
            writer.u16(UInt16(field.count))
            writer.u32(UInt32(offset))
            offset += field.count
        }
        writer.u32(flags)
        for field in fields { writer.append(field) }
        return NtlmAuthentication(message: writer.bytes, sessionKey: sessionKey)
    }
}

public enum Spnego {
    static let spnegoOid: [UInt8] = [0x06, 0x06, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x02]
    static let ntlmOid: [UInt8] = [0x06, 0x0a, 0x2b, 0x06, 0x01, 0x04, 0x01, 0x82, 0x37, 0x02, 0x02, 0x0a]

    static func length(_ count: Int) -> [UInt8] {
        if count < 0x80 { return [UInt8(count)] }
        if count < 0x100 { return [0x81, UInt8(count)] }
        if count < 0x10000 { return [0x82, UInt8(count >> 8), UInt8(count & 0xff)] }
        return [0x83, UInt8(count >> 16), UInt8((count >> 8) & 0xff), UInt8(count & 0xff)]
    }

    static func tlv(_ tag: UInt8, _ body: [UInt8]) -> [UInt8] { [tag] + length(body.count) + body }

    public static func initToken(_ ntlm: [UInt8]) -> [UInt8] {
        let mechTypes = tlv(0xa0, tlv(0x30, ntlmOid))
        let mechToken = tlv(0xa2, tlv(0x04, ntlm))
        return tlv(0x60, spnegoOid + tlv(0xa0, tlv(0x30, mechTypes + mechToken)))
    }

    public static func responseToken(_ ntlm: [UInt8]) -> [UInt8] {
        tlv(0xa1, tlv(0x30, tlv(0xa2, tlv(0x04, ntlm))))
    }
}
