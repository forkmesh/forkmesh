import FMBase
import Foundation
import Observation

@MainActor
@Observable
final class MessagesPeoplePictures {
    static let shared: MessagesPeoplePictures = MessagesPeoplePictures()
    nonisolated static let maxBytes: Int = 160 * 1024

    private var pictures: [String: String] = [:]

    init() {
        pictures = MessagesPeoplePictures.load()
    }

    nonisolated static var directory: URL? {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first?
            .appendingPathComponent("chat-people-pictures", isDirectory: true)
    }

    nonisolated static func key(_ name: String) -> String {
        name.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    }

    nonisolated static func fileName(_ key: String) -> String {
        Data(key.utf8).base64EncodedString()
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "=", with: "") + ".jpg"
    }

    nonisolated static func key(fromFile name: String) -> String? {
        guard name.hasSuffix(".jpg") else { return nil }
        var encoded: String = String(name.dropLast(4))
            .replacingOccurrences(of: "_", with: "/")
            .replacingOccurrences(of: "-", with: "+")
        while encoded.count % 4 != 0 { encoded += "=" }
        guard let data = Data(base64Encoded: encoded) else { return nil }
        return String(data: data, encoding: .utf8)
    }

    func picture(for name: String) -> String {
        pictures[MessagesPeoplePictures.key(name)] ?? ""
    }

    func has(_ name: String) -> Bool {
        !picture(for: name).isEmpty
    }

    @discardableResult
    func set(_ jpeg: Data?, for name: String) -> Bool {
        let key: String = MessagesPeoplePictures.key(name)
        guard !key.isEmpty, let directory = MessagesPeoplePictures.directory else { return false }
        let url: URL = directory.appendingPathComponent(MessagesPeoplePictures.fileName(key))
        guard let jpeg = jpeg, !jpeg.isEmpty, jpeg.count <= MessagesPeoplePictures.maxBytes else {
            _ = unlink(url.path)
            pictures[key] = nil
            return jpeg == nil
        }
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            try jpeg.write(to: url, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
        } catch {
            Log.warning("chat", "could not save the picture for a person: \(error)")
            return false
        }
        pictures[key] = jpeg.base64EncodedString()
        return true
    }

    nonisolated static func load() -> [String: String] {
        guard let directory = MessagesPeoplePictures.directory,
            let names = try? FileManager.default.contentsOfDirectory(atPath: directory.path)
        else { return [:] }
        var found: [String: String] = [:]
        for name in names {
            guard let key = MessagesPeoplePictures.key(fromFile: name),
                let data = FileManager.default.contents(atPath: directory.appendingPathComponent(name).path),
                !data.isEmpty, data.count <= MessagesPeoplePictures.maxBytes
            else { continue }
            found[key] = data.base64EncodedString()
        }
        return found
    }
}
