import FMApi
import FMBase
import FMPlanning
import Foundation

public enum SurfaceSection: String, Codable, Hashable, Sendable, CaseIterable {
    case agents
    case inbox
    case timer
    case overview

    public var widgetKind: String { "com.forkmesh.widget.\(rawValue)" }
}

public struct SurfaceSnapshot: Codable, Hashable, Sendable {
    public static let currentVersion = 2

    public var version: Int
    public var generatedAtMs: Int64
    public var signedIn: Bool
    public var agents: AgentsSurface
    public var inbox: InboxSurface
    public var work: WorkSurface
    public var timer: TimerActivitySnapshot?

    public init(
        generatedAtMs: Int64, signedIn: Bool, agents: AgentsSurface = .empty, inbox: InboxSurface = .empty,
        work: WorkSurface = .empty, timer: TimerActivitySnapshot? = nil
    ) {
        self.version = SurfaceSnapshot.currentVersion
        self.generatedAtMs = generatedAtMs
        self.signedIn = signedIn
        self.agents = agents
        self.inbox = inbox
        self.work = work
        self.timer = timer
    }

    var overviewCounts: [Int?] {
        [agents.total, agents.running, agents.needsYou, inbox.unread, work.openIssues, work.openPulls, work.openTasks,
         work.moreTasks ? 1 : 0]
    }

    public static func signedOut(at ms: Int64) -> SurfaceSnapshot {
        SurfaceSnapshot(generatedAtMs: ms, signedIn: false)
    }

    public static let placeholder = SurfaceSnapshot(
        generatedAtMs: 0, signedIn: true,
        agents: AgentsSurface(needsYou: 1, running: 2, queued: 0, total: 14, runs: [
            AgentRunCard(
                id: "desktop:1", title: "Fix the flaky sync test", repo: "forkmesh/app",
                detail: "Allow `swift test`?", provider: "Claude", state: .needsApproval, approvalId: "sample"),
            AgentRunCard(
                id: "desktop:2", title: "Add dark mode to settings", repo: "forkmesh/app",
                detail: "Editing SettingsScreen.swift", provider: "Codex", state: .running, canStop: true),
        ]),
        inbox: InboxSurface(unread: 3, items: [
            InboxSurfaceItem(id: "1", title: "Review requested", subtitle: "forkmesh/app", kind: "review", atMs: 0, route: nil),
        ]),
        work: WorkSurface(openIssues: 12, openPulls: 4, openTasks: 7))

    public func changedSections(from old: SurfaceSnapshot?) -> Set<SurfaceSection> {
        guard let old, old.version == version, old.signedIn == signedIn else { return Set(SurfaceSection.allCases) }
        var changed: Set<SurfaceSection> = []
        if old.agents != agents { changed.insert(.agents) }
        if old.inbox != inbox { changed.insert(.inbox) }
        if old.timer != timer { changed.insert(.timer) }
        if old.overviewCounts != overviewCounts { changed.insert(.overview) }
        return changed
    }
}

public enum SurfaceContainer {
    public static let appGroup = "group.com.forkmesh.forkmesh"
    public static let folder = "ForkMeshSurfaces"
    public static let keychainService = "com.forkmesh.forkmesh.surfaces"
    public static let keychainGroup = "742ZANCB57.com.forkmesh.forkmesh"

    public static func sharedItems() -> SharedItemStore? {
        #if canImport(Security)
        return KeychainSharedItemStore(service: keychainService, accessGroup: keychainGroup)
        #else
        return nil
        #endif
    }

    public static func groupDirectory(fileManager: FileManager = .default) -> URL? {
        #if os(iOS) || os(macOS) || os(watchOS)
        guard let base = fileManager.containerURL(forSecurityApplicationGroupIdentifier: appGroup) else { return nil }
        let directory = base.appendingPathComponent("Library", isDirectory: true)
            .appendingPathComponent("Application Support", isDirectory: true)
            .appendingPathComponent(folder, isDirectory: true)
        return prepared(directory, fileManager: fileManager)
        #else
        return nil
        #endif
    }

    public static func appDirectory(fileManager: FileManager = .default) -> URL? {
        guard let base = fileManager.urls(for: .applicationSupportDirectory, in: .userDomainMask).first else {
            return nil
        }
        return prepared(base.appendingPathComponent(folder, isDirectory: true), fileManager: fileManager)
    }

    static func prepared(_ directory: URL, fileManager: FileManager) -> URL? {
        do {
            try fileManager.createDirectory(at: directory, withIntermediateDirectories: true)
            return directory
        } catch {
            return nil
        }
    }
}

enum SurfaceFiles {
    static func read<T: Decodable>(_ type: T.Type, at url: URL, maxBytes: Int) -> T? {
        guard let attributes = try? FileManager.default.attributesOfItem(atPath: url.path),
              let size = (attributes[.size] as? NSNumber)?.intValue, size > 0, size <= maxBytes,
              let data = try? Data(contentsOf: url)
        else { return nil }
        return try? JSONDecoder().decode(T.self, from: data)
    }

    @discardableResult
    static func write<T: Encodable>(_ value: T, to url: URL, maxBytes: Int) -> Bool {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        guard let data = try? encoder.encode(value), data.count <= maxBytes else { return false }
        return writeData(data, to: url)
    }

    @discardableResult
    static func writeData(_ data: Data, to url: URL) -> Bool {
        do {
            #if os(iOS) || os(watchOS)
            try data.write(to: url, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
            #else
            try data.write(to: url, options: [.atomic])
            #endif
            return true
        } catch {
            return false
        }
    }

    static func remove(_ url: URL) {
        try? FileManager.default.removeItem(at: url)
    }

    static func decode<T: Decodable>(_ type: T.Type, from data: Data?, maxBytes: Int) -> T? {
        guard let data, !data.isEmpty, data.count <= maxBytes else { return nil }
        return try? JSONDecoder().decode(T.self, from: data)
    }

    static func encode<T: Encodable>(_ value: T, maxBytes: Int) -> Data? {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        guard let data = try? encoder.encode(value), data.count <= maxBytes else { return nil }
        return data
    }
}

public final class SurfaceSnapshotStore: @unchecked Sendable {
    public static let fileName = "snapshot.json"
    public static let maxBytes = 64 * 1_024

    public let items: SharedItemStore
    private let lock = NSLock()

    public init(items: SharedItemStore) {
        self.items = items
    }

    public convenience init(directory: URL) {
        self.init(items: DirectorySharedItemStore(directory: directory))
    }

    public static func shared() -> SurfaceSnapshotStore? {
        SurfaceContainer.sharedItems().map(SurfaceSnapshotStore.init(items:))
    }

    public func read() -> SurfaceSnapshot? {
        lock.lock()
        defer { lock.unlock() }
        return readUnlocked()
    }

    private func readUnlocked() -> SurfaceSnapshot? {
        guard let snapshot = SurfaceFiles.decode(
                SurfaceSnapshot.self, from: items.data(SurfaceSnapshotStore.fileName), maxBytes: SurfaceSnapshotStore.maxBytes),
              snapshot.version == SurfaceSnapshot.currentVersion
        else { return nil }
        return snapshot
    }

    @discardableResult
    public func write(_ snapshot: SurfaceSnapshot) -> Set<SurfaceSection> {
        lock.lock()
        defer { lock.unlock() }
        let changed = snapshot.changedSections(from: readUnlocked())
        if changed.isEmpty { return [] }
        guard let data = SurfaceFiles.encode(snapshot, maxBytes: SurfaceSnapshotStore.maxBytes) else { return [] }
        return items.set(data, SurfaceSnapshotStore.fileName) ? changed : []
    }

    public func clear() {
        lock.lock()
        defer { lock.unlock() }
        items.remove(SurfaceSnapshotStore.fileName)
    }
}
