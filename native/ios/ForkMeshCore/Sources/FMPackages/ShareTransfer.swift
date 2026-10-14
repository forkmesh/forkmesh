import FMBase
import Foundation

public enum ShareTransfer {
    public static let maxBytes = 512 * 1024 * 1024
    public static let mediaTypes = ["video/", "image/", "audio/"]

    public static func safeName(_ name: String, fallback: String = "face-changer.mp4") -> String {
        let base = String(name.split(separator: "/").last ?? "")
        let kept = String(base.unicodeScalars.filter {
            CharacterSet.alphanumerics.contains($0) && $0.isASCII || $0 == "." || $0 == "-" || $0 == "_"
        }.prefix(80))
        let trimmed = kept.trimmingCharacters(in: CharacterSet(charactersIn: "."))
        return trimmed.isEmpty ? fallback : trimmed
    }

    public static func allows(mime: String) -> Bool {
        let type = mime.lowercased()
        return mediaTypes.contains { type.hasPrefix($0) }
    }
}

public final class ShareTransferWriter {
    public struct Finished: Equatable, Sendable {
        public let url: URL
        public let mime: String
        public let title: String
    }

    private let folder: URL
    private var id = ""
    private var url: URL?
    private var mime = ""
    private var title = ""
    private var expected = 0
    private var written = 0
    private var handle: FileHandle?

    public init(folder: URL) {
        self.folder = folder
    }

    deinit {
        try? handle?.close()
    }

    public func receive(_ text: String) -> Finished? {
        let message: JSONObject = JSON.parseObject(text)
        switch message.str("event") {
        case "begin":
            begin(message)
            return nil
        case "chunk":
            chunk(message)
            return nil
        case "end":
            return end(message)
        default:
            return nil
        }
    }

    public func cancel() {
        try? handle?.close()
        handle = nil
        if let url { try? FileManager.default.removeItem(at: url) }
        reset()
    }

    private func reset() {
        id = ""
        url = nil
        mime = ""
        title = ""
        expected = 0
        written = 0
    }

    private func begin(_ message: JSONObject) {
        cancel()
        let transfer = message.str("id")
        let type = message.str("mime")
        let size = message.int("size")
        guard !transfer.isEmpty, transfer.count <= 64, ShareTransfer.allows(mime: type), size > 0, size <= ShareTransfer.maxBytes
        else { return }
        let manager = FileManager.default
        try? manager.createDirectory(at: folder, withIntermediateDirectories: true)
        let olds = (try? manager.contentsOfDirectory(at: folder, includingPropertiesForKeys: [.isRegularFileKey])) ?? []
        for old in olds where (try? old.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true {
            try? manager.removeItem(at: old)
        }
        let target = folder.appendingPathComponent(ShareTransfer.safeName(message.str("name")))
        guard manager.createFile(atPath: target.path, contents: nil),
              let opened = try? FileHandle(forWritingTo: target) else { return }
        id = transfer
        url = target
        mime = type
        title = String(message.str("title").prefix(120))
        expected = size
        handle = opened
    }

    private func chunk(_ message: JSONObject) {
        guard !id.isEmpty, message.str("id") == id, let handle else { return }
        guard let data = Data(base64Encoded: message.str("data")), written + data.count <= expected else {
            cancel()
            return
        }
        do {
            try handle.write(contentsOf: data)
            written += data.count
        } catch {
            cancel()
        }
    }

    private func end(_ message: JSONObject) -> Finished? {
        guard !id.isEmpty, message.str("id") == id, let url else { return nil }
        guard written == expected else {
            cancel()
            return nil
        }
        try? handle?.close()
        handle = nil
        let finished = Finished(url: url, mime: mime, title: title)
        reset()
        return finished
    }
}
