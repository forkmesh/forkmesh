func findHeaderBodySeparator(_ bytes: [UInt8]) -> Int {
    var index = 0
    while index + 1 < bytes.count {
        if bytes[index] == 0x0a && bytes[index + 1] == 0x0a { return index }
        index += 1
    }
    return -1
}

func decodeAsciiHeaders(_ bytes: [UInt8]) throws -> String {
    for byte in bytes where byte == 0 || byte > 0x7f || byte == 0x0d {
        throw GitException(.invalidObject, "object headers are not ASCII")
    }
    return String(decoding: bytes, as: UTF8.self)
}

/// Decodes object headers as UTF-8, rejecting NUL, carriage returns, and
/// invalid byte sequences. Commit and tag identity fields are UTF-8 per the
/// engine specification; every structured value inside a header (OIDs,
/// timestamps, types) is still validated by its own parser afterwards.
func decodeUtf8Headers(_ bytes: [UInt8]) throws -> String {
    for byte in bytes where byte == 0 || byte == 0x0d {
        throw GitException(.invalidObject, "object headers contain forbidden control bytes")
    }
    guard let text = utf8DecodeStrict(bytes) else {
        throw GitException(.invalidObject, "object headers are not valid UTF-8")
    }
    return text
}
