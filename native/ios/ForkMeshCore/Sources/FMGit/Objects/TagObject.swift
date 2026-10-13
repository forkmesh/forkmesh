public struct GitAnnotatedTag: Sendable {
    public let target: GitObjectId
    public let targetType: GitObjectType
    public let name: String
    public let message: [UInt8]

    public static func parse(_ body: [UInt8]) throws -> GitAnnotatedTag {
        let separator = findHeaderBodySeparator(body)
        let headerBytes = separator == -1 ? body : Array(body[0..<separator])
        let message = separator == -1 ? [UInt8]() : Array(body[(separator + 2)...])
        // Keyed by UTF-8 so two spellings Swift would call equal stay distinct.
        var values = [[UInt8]: String]()
        for line in try decodeUtf8Headers(headerBytes).u8Split(0x0a) {
            let index = line.u8IndexOf(" ")
            if index <= 0 || values[Array(line.u8Substring(0, index).utf8)] != nil {
                throw GitException(.invalidObject, "tag header is invalid")
            }
            values[Array(line.u8Substring(0, index).utf8)] = line.u8Substring(index + 1)
        }
        guard let target = values[Array("object".utf8)],
              let type = values[Array("type".utf8)],
              let name = values[Array("tag".utf8)],
              !name.utf8.isEmpty,
              !name.utf8.contains(where: { $0 == 0 || $0 == 0x0d || $0 == 0x0a })
        else {
            throw GitException(.invalidObject, "tag headers are incomplete")
        }
        let targetId = try GitObjectId.parseSha1(target)
        guard let targetType = GitObjectType.parse(type) else {
            throw GitException(.invalidObject, "tag target type is invalid")
        }
        return GitAnnotatedTag(target: targetId, targetType: targetType, name: name, message: message)
    }
}
