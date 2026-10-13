import FMBase
import FMCrypto
import Foundation

public struct NextRetirementReport: Equatable, Sendable {
    public var alreadyDone: Bool = false
    public var deferred: Bool = false
    public var dropped: [String] = []
    public var adopted: String?
    public var kept: [String] = []

    public init(
        alreadyDone: Bool = false, deferred: Bool = false, dropped: [String] = [], adopted: String? = nil,
        kept: [String] = []
    ) {
        self.alreadyDone = alreadyDone
        self.deferred = deferred
        self.dropped = dropped
        self.adopted = adopted
        self.kept = kept
    }
}

public struct NextProjectRetirement {
    public static let doneKey = "relays/nextRetired"
    public static let autoCreatedSecureKeys: Set<String> = [Identity.seedKey, OwnerCrypto.seedKey]
    static let carriedKey = "nodeSync/carried"

    let secrets: ThrowingSecureBackend
    let prefs: KeyValueStore
    let roots: [URL]
    let fileManager: FileManager

    public init(
        secrets: ThrowingSecureBackend, prefs: KeyValueStore, roots: [URL] = [], fileManager: FileManager = .default
    ) {
        self.secrets = secrets
        self.prefs = prefs
        self.roots = roots
        self.fileManager = fileManager
    }

    @discardableResult
    public func run() -> NextRetirementReport {
        if prefs.bool(forKey: NextProjectRetirement.doneKey) == true { return NextRetirementReport(alreadyDone: true) }
        var report = NextRetirementReport()
        do {
            try retire(into: &report)
        } catch {
            Log.warning("projects", "ForkMesh Next retirement deferred: \(error)")
            report.deferred = true
            return report
        }
        prefs.set(true, forKey: NextProjectRetirement.doneKey)
        return report
    }

    private func retire(into report: inout NextRetirementReport) throws {
        var rows = loadRows()
        var active = prefs.string(forKey: RelayDirectory.activeKey) ?? ""
        let aliases = rows.filter { AppEnv.isProdAlias($0.id) }
        for alias in aliases {
            if try !used(alias) {
                try erase(alias)
                rows.removeAll { $0.id == alias.id }
                if active == alias.id { active = AppEnv.prodHost }
                persist(rows, active)
                report.dropped.append(alias.id)
                continue
            }
            let production = rows.first { $0.id == AppEnv.prodHost }
            if report.adopted == nil,
                try productionAdoptable(production, sharingLegacy: alias.namespace.isEmpty),
                try adopt(alias)
            {
                let merged = adoptedProduction(production, from: alias)
                rows.removeAll { $0.id == alias.id }
                if let index = rows.firstIndex(where: { $0.id == AppEnv.prodHost }) {
                    rows[index] = merged
                } else {
                    rows.insert(merged, at: 0)
                }
                if active == alias.id { active = AppEnv.prodHost }
                persist(rows, active)
                report.adopted = alias.id
                continue
            }
            rows = rows.map { $0.id == alias.id ? RelayDirectory.retargetAlias($0) : $0 }
            persist(rows, active)
            report.kept.append(alias.id)
        }
    }

    private func persist(_ rows: [RelayProfile], _ active: String) {
        prefs.set(JSON.encode(.array(rows.map { .object($0.toJSON()) })), forKey: RelayDirectory.listKey)
        prefs.set(active, forKey: RelayDirectory.activeKey)
    }

    private func loadRows() -> [RelayProfile] {
        let raw = prefs.string(forKey: RelayDirectory.listKey) ?? ""
        guard !raw.isEmpty, let decoded = try? JSON.parse(raw), let items = decoded.arrayValue else { return [] }
        return items.compactMap { item in
            guard let object = item.objectValue else { return nil }
            let relay = RelayProfile.fromJSON(object)
            return relay.id.isEmpty ? nil : relay
        }
    }

    private func hasAccountState(_ id: String) throws -> Bool {
        if !(try secrets.read(AuthService.sessionKeyFor(id)) ?? "").isEmpty { return true }
        if prefs.bool(forKey: AuthService.guestModeKeyFor(id)) == true { return true }
        return prefs.contains("\(NextProjectRetirement.carriedKey)/\(id)")
    }

    func used(_ relay: RelayProfile) throws -> Bool {
        if relay.namespace.isEmpty { return true }
        if try hasAccountState(relay.id) { return true }
        let namespace = relay.namespace
        for key in try secrets.readAll().keys where key.hasPrefix(namespace) {
            let name = String(key.dropFirst(namespace.count))
            if !NextProjectRetirement.autoCreatedSecureKeys.contains(name) { return true }
        }
        let marker = relay.scope.key(ProjectLegacyMarker.key)
        if prefs.keys.contains(where: { $0.hasPrefix(namespace) && $0 != marker }) { return true }
        return roots.contains { hasContents(projectDirectory($0, relay.id)) }
    }

    func productionAdoptable(_ production: RelayProfile?, sharingLegacy: Bool) throws -> Bool {
        if let production, !production.namespace.isEmpty { return false }
        if try hasAccountState(AppEnv.prodHost) { return false }
        if !(try secrets.read(AuthService.sessionKey) ?? "").isEmpty { return false }
        if prefs.bool(forKey: AuthService.guestModeKey) == true { return false }
        if prefs.contains(NextProjectRetirement.carriedKey) { return false }
        if sharingLegacy { return true }
        for key in try secrets.readAll().keys {
            if ProjectKeyPolicy.isNamespaced(key) || ProjectKeyPolicy.isSharedSecureKey(key) { continue }
            if !NextProjectRetirement.autoCreatedSecureKeys.contains(key) { return false }
        }
        let legacyPrefs = ProjectKeyPolicy.projectPreferenceKeys.union(ProjectKeyPolicy.legacyPlaintextSecrets)
        if legacyPrefs.contains(where: { prefs.contains($0) }) { return false }
        for root in roots {
            let repos = root.appendingPathComponent("forkmesh", isDirectory: true)
                .appendingPathComponent("repos", isDirectory: true)
            if hasContents(repos) { return false }
            if fileManager.fileExists(atPath: projectDirectory(root, AppEnv.prodHost).path) { return false }
        }
        return true
    }

    private func adopt(_ alias: RelayProfile) throws -> Bool {
        var moved: [(from: URL, to: URL)] = []
        for root in roots {
            let from = projectDirectory(root, alias.id)
            guard fileManager.fileExists(atPath: from.path) else { continue }
            let to = projectDirectory(root, AppEnv.prodHost)
            do {
                try fileManager.moveItem(at: from, to: to)
                moved.append((from, to))
            } catch {
                Log.warning("projects", "Next project files could not move to ForkMesh: \(error)")
                undo(moved)
                return false
            }
        }
        let fromSession = AuthService.sessionKeyFor(alias.id)
        let toSession = AuthService.sessionKeyFor(AppEnv.prodHost)
        do {
            if let session = try secrets.read(fromSession), !session.isEmpty {
                try secrets.write(toSession, session)
                guard try secrets.read(toSession) == session else {
                    try? secrets.delete(toSession)
                    undo(moved)
                    return false
                }
                do {
                    try secrets.delete(fromSession)
                } catch {
                    Log.warning("projects", "The Next session copy stays behind: \(error)")
                }
            }
        } catch {
            try? secrets.delete(toSession)
            undo(moved)
            throw error
        }
        moveFlag(AuthService.guestModeKeyFor(alias.id), to: AuthService.guestModeKeyFor(AppEnv.prodHost))
        moveFlag(AuthService.revokedBannerKeyFor(alias.id), to: AuthService.revokedBannerKeyFor(AppEnv.prodHost))
        let fromCarried = "\(NextProjectRetirement.carriedKey)/\(alias.id)"
        if let carried = prefs.string(forKey: fromCarried) {
            prefs.set(carried, forKey: "\(NextProjectRetirement.carriedKey)/\(AppEnv.prodHost)")
            prefs.remove(fromCarried)
        }
        return true
    }

    private func adoptedProduction(_ production: RelayProfile?, from alias: RelayProfile) -> RelayProfile {
        let base = production ?? RelayProfile(
            id: AppEnv.prodHost, label: RelayDirectory.productionLabel, serverUrl: AppEnv.prodServerUrl,
            addedAt: alias.addedAt, builtIn: true)
        return base.copyWith(
            lastUsedAt: max(base.lastUsedAt, alias.lastUsedAt), namespace: alias.namespace, builtIn: true)
    }

    private func moveFlag(_ from: String, to: String) {
        guard prefs.bool(forKey: from) == true else { return }
        prefs.set(true, forKey: to)
        prefs.remove(from)
    }

    private func undo(_ moved: [(from: URL, to: URL)]) {
        for step in moved.reversed() {
            do {
                try fileManager.moveItem(at: step.to, to: step.from)
            } catch {
                Log.error("projects", "Next project files could not be put back: \(error)")
            }
        }
    }

    private func erase(_ alias: RelayProfile) throws {
        let scope = alias.scope
        guard !scope.isLegacy else { return }
        for key in prefs.keys where key.hasPrefix(scope.namespace) {
            prefs.remove(key)
        }
        prefs.remove(AuthService.revokedBannerKeyFor(alias.id))
        for root in roots {
            ProjectStorage.eraseDirectory(scope, under: root, fileManager: fileManager)
        }
        guard scope.namespace.count > ProjectScope.namespaceRoot.count, ProjectKeyPolicy.isNamespaced(scope.namespace)
        else { return }
        for key in try secrets.readAll().keys where key.hasPrefix(scope.namespace) {
            try secrets.delete(key)
        }
    }

    private func projectDirectory(_ root: URL, _ projectId: String) -> URL {
        root.appendingPathComponent("projects", isDirectory: true)
            .appendingPathComponent(ProjectScope.slugFor(projectId), isDirectory: true)
    }

    private func hasContents(_ directory: URL) -> Bool {
        let entries = (try? fileManager.contentsOfDirectory(atPath: directory.path)) ?? []
        return !entries.isEmpty
    }
}
