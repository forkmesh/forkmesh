import FMApi
import FMBase
import Foundation

public struct ShareHandoff: Codable, Hashable, Sendable, Identifiable {
    public var id: String
    public var createdAtMs: Int64
    public var text: String
    public var urls: [String]
    public var images: [String]
    public var repo: String

    public init(id: String, createdAtMs: Int64, text: String, urls: [String], images: [String], repo: String = "") {
        self.id = id
        self.createdAtMs = createdAtMs
        self.text = text
        self.urls = urls
        self.images = images
        self.repo = repo
    }

    public var prompt: String {
        var parts: [String] = []
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        if !trimmed.isEmpty { parts.append(trimmed) }
        let links = urls.filter { !trimmed.contains($0) }
        if !links.isEmpty { parts.append(links.joined(separator: "\n")) }
        return parts.joined(separator: "\n\n")
    }

    public var composeTarget: ComposeTarget {
        let prompt = self.prompt
        return ComposeTarget(repo: RepoRef(slug: repo), text: prompt.isEmpty ? nil : prompt)
    }

    public var route: Route { .compose(composeTarget) }
}

public struct ShareHandoffError: Error, Equatable, CustomStringConvertible, Sendable {
    public let message: String

    public init(_ message: String) {
        self.message = message
    }

    public var description: String { message }
}

public final class ShareHandoffStore: @unchecked Sendable {
    public static let folder = "share"
    public static let manifestName = "handoff.json"
    public static let maxImages = 10
    public static let maxImageBytes = 20 * 1_024 * 1_024
    public static let maxTextLength = 20_000
    public static let maxURLs = 10
    public static let maxURLLength = 2_048
    public static let maxPending = 5
    public static let maxAgeMs: Int64 = 24 * 60 * 60 * 1_000
    static let manifestMaxBytes = 128 * 1_024

    public let directory: URL
    private let clock: Clock
    private let makeId: () -> String
    private let lock = NSLock()

    public init(directory: URL, clock: Clock = SystemClock(), makeId: @escaping () -> String = { UUID().uuidString }) {
        self.directory = directory.appendingPathComponent(ShareHandoffStore.folder, isDirectory: true)
        self.clock = clock
        self.makeId = makeId
    }

    public static func appGroup() -> ShareHandoffStore? {
        SurfaceContainer.groupDirectory().map { ShareHandoffStore(directory: $0) }
    }

    public static func cleanURLs(_ urls: [String]) -> [String] {
        var seen: Set<String> = []
        var out: [String] = []
        for raw in urls {
            let text = raw.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !text.isEmpty, text.count <= maxURLLength, let url = URL(string: text),
                  let scheme = url.scheme?.lowercased(), scheme == "https" || scheme == "http",
                  url.host?.isEmpty == false, !seen.contains(text)
            else { continue }
            seen.insert(text)
            out.append(text)
            if out.count == maxURLs { break }
        }
        return out
    }

    private func folder(for id: String) -> URL { directory.appendingPathComponent(id, isDirectory: true) }

    static func validId(_ id: String) -> Bool {
        !id.isEmpty && id.count <= 64 && id.allSatisfy { $0.isLetter || $0.isNumber || $0 == "-" }
            && id.unicodeScalars.allSatisfy(\.isASCII)
    }

    public static func clean(
        text: String, urls: [String], images: [Data], repo: String
    ) throws -> (text: String, urls: [String], images: [Data], repo: String) {
        let cleanText = String(text.trimmingCharacters(in: .whitespacesAndNewlines).prefix(maxTextLength))
        let cleanURLs = cleanURLs(urls)
        let kept = images.filter { !$0.isEmpty }
        if kept.count > maxImages {
            throw ShareHandoffError("Share at most \(maxImages) images at a time.")
        }
        if kept.contains(where: { $0.count > maxImageBytes }) {
            throw ShareHandoffError("One of those images is too large to send.")
        }
        if cleanText.isEmpty && cleanURLs.isEmpty && kept.isEmpty {
            throw ShareHandoffError("There is nothing to send.")
        }
        return (cleanText, cleanURLs, kept, RepoRef(slug: repo)?.slug ?? "")
    }

    @discardableResult
    public func save(text: String, urls: [String], images: [Data], repo: String = "") throws -> ShareHandoff {
        let (cleanText, cleanURLs, kept, cleanRepo) = try ShareHandoffStore.clean(
            text: text, urls: urls, images: images, repo: repo)
        lock.lock()
        defer { lock.unlock() }
        pruneUnlocked(keepingRoomForOne: true)
        let id = makeId()
        guard ShareHandoffStore.validId(id) else { throw ShareHandoffError("Could not save what you shared.") }
        let target = folder(for: id)
        do {
            try FileManager.default.createDirectory(at: target, withIntermediateDirectories: true)
        } catch {
            throw ShareHandoffError("Could not save what you shared.")
        }
        var names: [String] = []
        for (index, data) in kept.enumerated() {
            let name = "image-\(index).bin"
            if !SurfaceFiles.writeData(data, to: target.appendingPathComponent(name)) {
                SurfaceFiles.remove(target)
                throw ShareHandoffError("Could not save what you shared.")
            }
            names.append(name)
        }
        let handoff = ShareHandoff(
            id: id, createdAtMs: clock.nowMillis, text: cleanText, urls: cleanURLs, images: names, repo: cleanRepo)
        if !SurfaceFiles.write(
            handoff, to: target.appendingPathComponent(ShareHandoffStore.manifestName),
            maxBytes: ShareHandoffStore.manifestMaxBytes)
        {
            SurfaceFiles.remove(target)
            throw ShareHandoffError("Could not save what you shared.")
        }
        return handoff
    }

    public func pending() -> [ShareHandoff] {
        lock.lock()
        defer { lock.unlock() }
        pruneUnlocked(keepingRoomForOne: false)
        return listUnlocked()
    }

    private func listUnlocked() -> [ShareHandoff] {
        let ids = (try? FileManager.default.contentsOfDirectory(atPath: directory.path)) ?? []
        return ids.filter(ShareHandoffStore.validId).compactMap { id -> ShareHandoff? in
            guard let handoff = SurfaceFiles.read(
                ShareHandoff.self, at: folder(for: id).appendingPathComponent(ShareHandoffStore.manifestName),
                maxBytes: ShareHandoffStore.manifestMaxBytes), handoff.id == id
            else { return nil }
            return handoff
        }.sorted { $0.createdAtMs < $1.createdAtMs }
    }

    public func imageData(_ handoff: ShareHandoff) -> [Data] {
        guard ShareHandoffStore.validId(handoff.id) else { return [] }
        let base = folder(for: handoff.id)
        return handoff.images.prefix(ShareHandoffStore.maxImages).compactMap { name -> Data? in
            guard name.hasPrefix("image-"), !name.contains("/"), !name.contains("..") else { return nil }
            let url = base.appendingPathComponent(name)
            guard let attributes = try? FileManager.default.attributesOfItem(atPath: url.path),
                  let size = (attributes[.size] as? NSNumber)?.intValue, size > 0,
                  size <= ShareHandoffStore.maxImageBytes
            else { return nil }
            return try? Data(contentsOf: url)
        }
    }

    public func remove(_ id: String) {
        guard ShareHandoffStore.validId(id) else { return }
        lock.lock()
        defer { lock.unlock() }
        SurfaceFiles.remove(folder(for: id))
    }

    public func removeAll() {
        lock.lock()
        defer { lock.unlock() }
        SurfaceFiles.remove(directory)
    }

    private func pruneUnlocked(keepingRoomForOne: Bool) {
        let now = clock.nowMillis
        let ids = (try? FileManager.default.contentsOfDirectory(atPath: directory.path)) ?? []
        let live = Set(listUnlocked().filter { now - $0.createdAtMs < ShareHandoffStore.maxAgeMs }.map(\.id))
        for id in ids where !live.contains(id) {
            SurfaceFiles.remove(directory.appendingPathComponent(id))
        }
        let limit = ShareHandoffStore.maxPending - (keepingRoomForOne ? 1 : 0)
        let kept = listUnlocked()
        if kept.count > limit {
            for handoff in kept.prefix(kept.count - limit) {
                SurfaceFiles.remove(folder(for: handoff.id))
            }
        }
    }
}
