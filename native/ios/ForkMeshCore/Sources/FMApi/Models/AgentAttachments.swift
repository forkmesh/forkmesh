import FMBase
import Foundation

public struct AgentAttachmentMeta: Equatable, Hashable, Sendable {
    public let id: String
    public let name: String
    public let mediaType: String
    public let size: Int

    public init(id: String, name: String, mediaType: String, size: Int = -1) {
        self.id = id
        self.name = name
        self.mediaType = mediaType
        self.size = size
    }

    public static func list(
        _ value: JSONValue?, idPattern: String = "^[0-9a-f]{32}$", limit: Int = 20
    ) -> [AgentAttachmentMeta] {
        var result: [AgentAttachmentMeta] = []
        var seen = Set<String>()
        for item in value?.arrayValue ?? [] {
            guard let object = item.objectValue else { continue }
            let id = object.str("id").lowercased()
            if id.range(of: idPattern, options: .regularExpression) == nil || seen.contains(id) { continue }
            seen.insert(id)
            let name = AgentAttachmentMedia.displayName(object.str("name"), fallback: "attachment")
            var mediaType = object.str("mediaType").lowercased().dartTrimmed
            if mediaType.isEmpty { mediaType = AgentAttachmentMedia.type(forName: name) }
            let size = object["size"].map { ModelJSON.int($0, fallback: -1) } ?? -1
            result.append(AgentAttachmentMeta(id: id, name: name, mediaType: mediaType, size: max(-1, size)))
            if result.count >= limit { break }
        }
        return result
    }

    public var wire: JSONValue {
        .object([
            "id": .string(id), "name": .string(name), "mediaType": .string(mediaType), "size": .int(Int64(size)),
        ])
    }
}

public struct AgentAttachmentRef: Equatable, Hashable, Sendable, Identifiable {
    public enum Origin: Hashable, Sendable {
        case relay(owner: String, repo: String, sessionId: String, orgScope: Bool)
        case desktop(sessionId: Int)
        case elsewhere
        case local
    }

    public let meta: AgentAttachmentMeta
    public let origin: Origin

    public init(meta: AgentAttachmentMeta, origin: Origin) {
        self.meta = meta
        self.origin = origin
    }

    public var id: String { cacheKey }
    public var name: String { meta.name }
    public var mediaType: String { meta.mediaType }
    public var size: Int { meta.size }

    public var previewCacheKey: String { cacheKey + "#preview" }

    public var cacheKey: String {
        switch origin {
        case .relay(let owner, let repo, let sessionId, let orgScope):
            return "relay:\(orgScope ? "org" : "owner"):\(owner.lowercased())/\(repo.lowercased()):\(sessionId):\(meta.id)"
        case .desktop(let sessionId):
            return "desktop:\(sessionId):\(meta.id)"
        case .elsewhere:
            return "path:\(meta.id)"
        case .local:
            return "local:\(meta.id)"
        }
    }

    public var isImage: Bool { AgentAttachmentMedia.isImage(mediaType) }
    public var isVideo: Bool { mediaType.hasPrefix("video/") }

    public var canFetch: Bool { origin != .elsewhere }

    public var showsThumbnail: Bool { isImage && canFetch }

    public var sizeLabel: String { size < 0 ? "" : AgentAttachmentMedia.sizeLabel(size) }

    public var rowDetail: String {
        var parts: [String] = []
        if !sizeLabel.isEmpty { parts.append(sizeLabel) }
        if origin == .elsewhere { parts.append("on the machine that ran the agent") }
        return parts.joined(separator: " · ")
    }

    public static func local(_ photo: PromptAttachment) -> AgentAttachmentRef {
        AgentAttachmentRef(
            meta: AgentAttachmentMeta(
                id: photo.id, name: AgentAttachmentMedia.displayName(photo.name, fallback: "photo.jpg"),
                mediaType: photo.mediaType, size: photo.size),
            origin: .local)
    }
}

public struct AgentAttachmentPayload: Equatable, Sendable {
    public let data: Data
    public let preview: Bool
    public let originalSize: Int
    public let width: Int
    public let height: Int

    public init(data: Data, preview: Bool = false, originalSize: Int = -1, width: Int = 0, height: Int = 0) {
        self.data = data
        self.preview = preview
        self.originalSize = originalSize
        self.width = width
        self.height = height
    }

    public static func full(_ data: Data) -> AgentAttachmentPayload { AgentAttachmentPayload(data: data) }

    public var withoutData: AgentAttachmentPayload {
        AgentAttachmentPayload(data: Data(), preview: preview, originalSize: originalSize, width: width, height: height)
    }

    public var previewNote: String {
        preview ? "Preview — open on the desktop's network for the full image." : ""
    }

    public var previewDetail: String {
        if !preview { return "" }
        var parts: [String] = []
        if width > 0 && height > 0 { parts.append("\(width) × \(height)") }
        if originalSize > 0 { parts.append("full image \(AgentAttachmentMedia.sizeLabel(originalSize))") }
        return parts.joined(separator: " · ")
    }
}

public enum AgentAttachmentMedia {
    public static let imageTypes: Set<String> = [
        "image/jpeg", "image/png", "image/gif", "image/webp", "image/heic",
    ]

    public static let maxImageBytes = 5 * 1024 * 1024

    public static func isImage(_ mediaType: String) -> Bool { imageTypes.contains(mediaType.lowercased()) }

    public static func type(forName name: String) -> String {
        switch (name as NSString).pathExtension.lowercased() {
        case "jpg", "jpeg": return "image/jpeg"
        case "png": return "image/png"
        case "gif": return "image/gif"
        case "webp": return "image/webp"
        case "heic", "heif": return "image/heic"
        case "mp4", "m4v": return "video/mp4"
        case "mov": return "video/quicktime"
        case "webm": return "video/webm"
        case "pdf": return "application/pdf"
        case "txt", "log", "md": return "text/plain"
        case "json": return "application/json"
        case "zip": return "application/zip"
        default: return "application/octet-stream"
        }
    }

    public static func displayName(_ raw: String, fallback: String) -> String {
        let last = raw.replacingOccurrences(of: "\\", with: "/").split(separator: "/").last.map(String.init) ?? ""
        let printable = String(last.unicodeScalars.filter { !CharacterSet.controlCharacters.contains($0) })
        let trimmed = String(printable.dartTrimmed.prefix(120))
        return trimmed.isEmpty ? fallback : trimmed
    }

    public static func sizeLabel(_ bytes: Int) -> String {
        if bytes < 1024 { return "\(bytes) B" }
        if bytes < 1024 * 1024 { return String(format: "%.1f KB", Double(bytes) / 1024) }
        return String(format: "%.1f MB", Double(bytes) / (1024 * 1024))
    }

    public static func sniff(_ data: Data) -> String? {
        let bytes = [UInt8](data.prefix(16))
        if bytes.count >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF { return "image/jpeg" }
        if bytes.starts(with: [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]) { return "image/png" }
        if bytes.starts(with: Array("GIF87a".utf8)) || bytes.starts(with: Array("GIF89a".utf8)) { return "image/gif" }
        if bytes.count >= 12 && bytes.starts(with: Array("RIFF".utf8)) && Array(bytes[8..<12]) == Array("WEBP".utf8) {
            return "image/webp"
        }
        if bytes.count >= 12 && Array(bytes[4..<8]) == Array("ftyp".utf8) {
            let brand = String(decoding: bytes[8..<12], as: UTF8.self)
            if ["heic", "heix", "hevc", "heim", "heis", "mif1", "msf1"].contains(brand) { return "image/heic" }
        }
        return nil
    }

    public static func checkImage(_ data: Data, maxBytes: Int = maxImageBytes) throws -> String {
        if data.isEmpty { throw AgentAttachmentError("The attachment is empty.") }
        if data.count > maxBytes { throw AgentAttachmentError("The attachment is larger than \(sizeLabel(maxBytes)).") }
        guard let type = sniff(data) else { throw AgentAttachmentError("The attachment is not an image.") }
        return type
    }
}

public struct AgentAttachmentError: Error, Equatable, Sendable, CustomStringConvertible {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var description: String { message }
}

public enum AgentTranscriptAttachments {
    public static let blockHeader = "Attached images (open them with your file-reading tool):"
    public static let limit = 50

    public static func paths(in text: String) -> [String] {
        if !text.contains("Attached ") { return [] }
        var paths: [String] = []
        func add(_ raw: Substring) {
            let path = String(raw).dartTrimmed
            if path.isEmpty || path.count > 1024 || paths.contains(path) || paths.count >= limit { return }
            paths.append(path)
        }
        var inBlock = false
        for line in text.split(omittingEmptySubsequences: false, whereSeparator: { $0 == "\n" || $0 == "\r\n" }) {
            if inBlock {
                if line.hasPrefix("- ") {
                    add(line.dropFirst(2))
                    continue
                }
                inBlock = false
            }
            if String(line).dartTrimmed == blockHeader {
                inBlock = true
                continue
            }
            for marker in ["Attached image:", "Attached video:"] {
                var rest = line[...]
                while let range = rest.range(of: marker) {
                    let tail = rest[range.upperBound...]
                    let end = tail.firstIndex { $0 == "\"" || $0 == "\\" } ?? tail.endIndex
                    add(tail[..<end])
                    rest = tail[end...]
                }
            }
        }
        return paths
    }

    public static func references(in text: String) -> [AgentAttachmentRef] {
        paths(in: text).map { path in
            let name = AgentAttachmentMedia.displayName(path, fallback: "attachment")
            return AgentAttachmentRef(
                meta: AgentAttachmentMeta(id: path, name: name, mediaType: AgentAttachmentMedia.type(forName: name)),
                origin: .elsewhere)
        }
    }
}

public enum AgentAttachmentReconcile {
    public static func pair(
        sent: [PromptAttachment], listed: [AgentAttachmentMeta]
    ) -> [(meta: AgentAttachmentMeta, photo: PromptAttachment)]? {
        if sent.isEmpty || sent.count != listed.count { return nil }
        var pairs: [(meta: AgentAttachmentMeta, photo: PromptAttachment)] = []
        for (meta, photo) in zip(listed, sent) {
            if meta.size >= 0 && meta.size != photo.size { return nil }
            if !meta.mediaType.isEmpty && meta.mediaType != photo.mediaType.lowercased() { return nil }
            pairs.append((meta, photo))
        }
        return pairs
    }

    public static func turn(for sent: [PromptAttachment], in history: [AgentLaunchMessage]) -> Int? {
        for index in history.indices.reversed() where history[index].isUser {
            if pair(sent: sent, listed: history[index].attachments) != nil { return index }
        }
        return nil
    }
}

@MainActor
public final class AgentSentPhotos {
    public static let shared = AgentSentPhotos()

    public let sessionLimit: Int
    private var turns: [(sessionId: String, text: String, photos: [PromptAttachment])] = []

    public init(sessionLimit: Int = 12) {
        self.sessionLimit = max(1, sessionLimit)
    }

    public func record(sessionId: String, text: String, photos: [PromptAttachment]) {
        if sessionId.isEmpty || photos.isEmpty { return }
        turns.removeAll { $0.sessionId == sessionId && $0.text == text }
        turns.append((sessionId, text, photos))
        var sessions: [String] = []
        for turn in turns.reversed() where !sessions.contains(turn.sessionId) { sessions.append(turn.sessionId) }
        if sessions.count > sessionLimit {
            let keep = Set(sessions.prefix(sessionLimit))
            turns.removeAll { !keep.contains($0.sessionId) }
        }
    }

    public func photos(sessionId: String, text: String) -> [PromptAttachment]? {
        turns.last { $0.sessionId == sessionId && $0.text == text }?.photos
    }

    public func rename(_ from: String, to: String) {
        for index in turns.indices where turns[index].sessionId == from { turns[index].sessionId = to }
    }
}
