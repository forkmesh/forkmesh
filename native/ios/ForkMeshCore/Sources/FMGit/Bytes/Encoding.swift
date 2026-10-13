import Foundation

private let hexDigits: [UInt8] = Array("0123456789abcdef".utf8)

public func decodeLowerHex(_ value: String) throws -> [UInt8] {
    let text = Array(value.utf8)
    func nibble(_ byte: UInt8) -> UInt8? {
        switch byte {
        case 0x30...0x39: return byte - 0x30
        case 0x61...0x66: return byte - 0x61 + 10
        default: return nil
        }
    }
    if text.count % 2 != 0 {
        throw GitException(.invalidObject, "expected canonical lowercase hexadecimal")
    }
    var output = [UInt8]()
    output.reserveCapacity(text.count / 2)
    var index = 0
    while index < text.count {
        guard let high = nibble(text[index]), let low = nibble(text[index + 1]) else {
            throw GitException(.invalidObject, "expected canonical lowercase hexadecimal")
        }
        output.append((high << 4) | low)
        index += 2
    }
    return output
}

public func encodeLowerHex(_ value: [UInt8]) -> String {
    var output = [UInt8]()
    output.reserveCapacity(value.count * 2)
    for byte in value {
        output.append(hexDigits[Int(byte >> 4)])
        output.append(hexDigits[Int(byte & 0x0f)])
    }
    return String(decoding: output, as: UTF8.self)
}

public func encodeCanonicalBase64Url(_ value: [UInt8]) -> String {
    Data(value).base64EncodedString()
        .replacingOccurrences(of: "+", with: "-")
        .replacingOccurrences(of: "/", with: "_")
        .replacingOccurrences(of: "=", with: "")
}

public func decodeCanonicalBase64Url(_ value: String) throws -> [UInt8] {
    for byte in value.utf8 {
        let ok = (byte >= 0x30 && byte <= 0x39) || (byte >= 0x41 && byte <= 0x5a)
            || (byte >= 0x61 && byte <= 0x7a) || byte == 0x5f || byte == 0x2d
        if !ok {
            throw GitException(.invalidObject, "expected unpadded canonical base64url")
        }
    }
    let padding = String(repeating: "=", count: (4 - value.utf8.count % 4) % 4)
    let standard = value.replacingOccurrences(of: "-", with: "+").replacingOccurrences(of: "_", with: "/")
    guard let data = Data(base64Encoded: standard + padding) else {
        throw GitFormatError("invalid base64url length")
    }
    let decoded = [UInt8](data)
    if encodeCanonicalBase64Url(decoded) != value {
        throw GitException(.invalidObject, "base64url spelling is not canonical")
    }
    return decoded
}
