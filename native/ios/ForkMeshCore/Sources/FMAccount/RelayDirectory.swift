import FMBase
import Foundation
import Observation

/// Every relay this phone knows about, and which one is active.
///
/// Holding the set of relays, each with its own identity, its own session (see
/// `AuthService.relayScope`) and its own cached data, is what makes a relay a workspace the user
/// can move between without losing the one they left.
///
/// The directory only records intent. Taking a switch through the running app (sockets down,
/// caches cleared, the target relay's session loaded, sockets back up, in that order) is the relay
/// switch controller's job, because half of those steps can fail and the user has to be told which.
///
/// Storage is synchronous (`KeyValueStore`), so these are plain calls.
@MainActor
@Observable
public final class RelayDirectory {
    nonisolated public static let listKey = "relays/list"
    nonisolated public static let activeKey = "relays/active"
    /// Set once the legacy single-relay `server/url` preference has been folded into the list, so
    /// a relay the user later removes is not resurrected from it on the next launch.
    nonisolated public static let migratedKey = "relays/migratedFromServerUrl"

    @ObservationIgnored private let prefs: KeyValueStore
    @ObservationIgnored private let env: AppEnv
    @ObservationIgnored private let clock: Clock
    /// Folds retired room paths and the production aliases onto the canonical URL, so those never
    /// become a second relay row. Supplied by `SettingsService`, which owns the rule.
    @ObservationIgnored private let canonicalize: (String) -> String
    /// `addListener` / `removeListener` for code outside SwiftUI (`SettingsService` forwards it).
    @ObservationIgnored public let listeners = Listeners()

    public private(set) var relays: [RelayProfile] = []
    private var activeIdValue = ""

    /// Seeds the build's relay, so `active` can never fail on an empty list. `load` replaces it
    /// with the persisted set.
    public init(
        prefs: KeyValueStore,
        env: AppEnv = .prod,
        clock: Clock = SystemClock(),
        canonicalizeUrl: ((String) -> String)? = nil
    ) {
        self.prefs = prefs
        self.env = env
        self.clock = clock
        self.canonicalize = canonicalizeUrl ?? { $0 }
        ensureBuiltIn()
        activeIdValue = relays[0].id
    }

    /// Load the list, seed the build's own relay, and adopt a legacy override.
    public static func load(
        prefs: KeyValueStore,
        env: AppEnv = .prod,
        clock: Clock = SystemClock(),
        canonicalizeUrl: ((String) -> String)? = nil
    ) -> RelayDirectory {
        let directory = RelayDirectory(prefs: prefs, env: env, clock: clock, canonicalizeUrl: canonicalizeUrl)
        directory.restore()
        return directory
    }

    /// The relay the app is currently pointed at. Never fails: an active id naming a relay that is
    /// no longer in the list falls back to the first.
    public var active: RelayProfile {
        relays.first { $0.id == activeIdValue } ?? relays[0]
    }

    public var activeId: String { active.id }

    /// The relay URL every call site reads through `SettingsService.serverUrl`.
    public var activeServerUrl: String { active.serverUrl }

    public var hasMultiple: Bool { relays.count > 1 }

    /// What the active relay supports.
    public var activeFeatures: RelayFeatureSet { active.features }

    public func byId(_ id: String) -> RelayProfile? { relays.first { $0.id == id } }

    public func contains(_ id: String) -> Bool { byId(id) != nil }

    @discardableResult
    public func addListener(_ listener: @escaping @Sendable () -> Void) -> ListenerToken { listeners.add(listener) }

    public func removeListener(_ token: ListenerToken) { listeners.remove(token) }

    private func notifyListeners() { listeners.notify() }

    private func restore() {
        let raw = prefs.string(forKey: RelayDirectory.listKey) ?? ""
        var parsed: [RelayProfile] = []
        if !raw.isEmpty, let decoded = try? JSON.parse(raw), let items = decoded.arrayValue {
            // A corrupt list must not brick the app into a relay-less state; the built-in relay is
            // re-seeded below and the user re-adds the rest.
            for item in items {
                guard let object = item.objectValue else { continue }
                let relay = RelayProfile.fromJSON(object)
                if !relay.id.isEmpty { parsed.append(relay) }
            }
        }
        relays = parsed.map(RelayDirectory.retargetAlias)
        adoptLegacyServerUrl()
        ensureBuiltIn()
        activeIdValue = prefs.string(forKey: RelayDirectory.activeKey) ?? ""
        if !contains(activeIdValue) { activeIdValue = relays[0].id }
        persist()
    }

    /// Fold a relay URL typed into the old single-field Settings box into the list, once.
    private func adoptLegacyServerUrl() {
        if prefs.bool(forKey: RelayDirectory.migratedKey) == true { return }
        prefs.set(true, forKey: RelayDirectory.migratedKey)
        let raw = prefs.string(forKey: SettingsService.serverUrlKey)?.dartTrimmed ?? ""
        if raw.isEmpty { return }
        // Canonicalize first: an alias of the deployed relay must not add a duplicate row.
        let stored = canonicalize(raw)
        guard let relay = RelayProfile.tryParse(stored, now: clock.nowMillis), !contains(relay.id) else { return }
        if RelayProfile.normalizeOrigin(builtInServerUrl) == relay.id {
            return // the build's own relay; ensureBuiltIn seeds it.
        }
        // Kept verbatim: it may name a room path this build no longer produces. It predates
        // projects, so it shared the one legacy store and keeps it.
        relays.append(relay.copyWith(serverUrl: RelayDirectory.foldAliasUrl(stored), namespace: ""))
        // A deliberate override was also the relay they were on.
        if (prefs.string(forKey: RelayDirectory.activeKey) ?? "").isEmpty {
            prefs.set(relay.id, forKey: RelayDirectory.activeKey)
        }
    }

    private func ensureBuiltIn() {
        let defaultUrl = builtInServerUrl
        guard let id = RelayProfile.normalizeOrigin(defaultUrl) else {
            if relays.isEmpty { relays = [production()] }
            ensureFixedProjects()
            return
        }
        if byId(id) != nil {
            // Adopt it as the built-in so it can never be removed out from under the build, and
            // refresh the URL in case the room path moved. Learned features are carried: they are
            // about the relay, not about this launch.
            relays = relays.map { (relay: RelayProfile) -> RelayProfile in
                if relay.id == id {
                    return relay.copyWith(label: RelayDirectory.builtInLabel(relay), serverUrl: defaultUrl, builtIn: true)
                }
                if relay.builtIn && !RelayDirectory.isFixedProject(relay.id) {
                    return relay.copyWith(builtIn: false) // demoted: the environment moved on
                }
                return relay
            }
            ensureFixedProjects()
            return
        }
        // Drop a previous build's built-in relay, but only when the user never signed into it or
        // renamed it; otherwise it stays as a normal entry.
        var next: [RelayProfile] = [
            id == AppEnv.prodHost
                ? production()
                : RelayProfile(
                    id: id, label: id, serverUrl: defaultUrl, addedAt: clock.nowMillis, builtIn: true,
                    namespace: ProjectScope.namespaceFor(id)),
        ]
        for relay in relays {
            let fixed = RelayDirectory.isFixedProject(relay.id)
            if relay.builtIn && !fixed && relay.lastUsedAt == 0 && relay.label == relay.id { continue }
            next.append(relay.copyWith(builtIn: fixed))
        }
        relays = next
        ensureFixedProjects()
    }

    private func production() -> RelayProfile {
        RelayProfile(
            id: AppEnv.prodHost, label: RelayDirectory.productionLabel, serverUrl: AppEnv.prodServerUrl,
            addedAt: clock.nowMillis, builtIn: true)
    }

    nonisolated public static let productionLabel = "ForkMesh"
    nonisolated public static let formerNextLabel = "ForkMesh (from Next)"
    nonisolated static let retiredNextLabel = "ForkMesh Next"

    nonisolated public static func isFixedProject(_ id: String) -> Bool {
        id == AppEnv.prodHost
    }

    nonisolated static func builtInLabel(_ relay: RelayProfile) -> String {
        if relay.label != relay.id { return relay.label }
        if relay.id == AppEnv.prodHost { return productionLabel }
        return relay.label
    }

    private var builtInServerUrl: String {
        let url = env.defaultServerUrl
        guard let origin = RelayProfile.hostOrigin(url), AppEnv.isProdAlias(origin) else { return url }
        return AppEnv.prodServerUrl
    }

    nonisolated static func retargetAlias(_ relay: RelayProfile) -> RelayProfile {
        guard AppEnv.isProdAlias(relay.id) else { return relay }
        let renamed = relay.label == relay.id || relay.label == retiredNextLabel
        return relay.copyWith(
            label: renamed ? formerNextLabel : relay.label, serverUrl: AppEnv.prodServerUrl, worldUrl: "",
            homepageUrl: "", builtIn: false)
    }

    nonisolated static func foldAliasUrl(_ url: String) -> String {
        guard let origin = RelayProfile.hostOrigin(url), AppEnv.isProdAlias(origin) else { return url }
        return AppEnv.prodServerUrl
    }

    private func ensureFixedProjects() {
        for fixed in [production()] {
            guard byId(fixed.id) != nil else {
                relays.append(fixed)
                continue
            }
            relays = relays.map { (relay: RelayProfile) -> RelayProfile in
                guard relay.id == fixed.id else { return relay }
                return relay.copyWith(
                    label: RelayDirectory.builtInLabel(relay),
                    worldUrl: relay.worldUrl.isEmpty ? fixed.worldUrl : nil,
                    homepageUrl: relay.homepageUrl.isEmpty ? fixed.homepageUrl : nil,
                    builtIn: true)
            }
        }
        let envId = RelayProfile.normalizeOrigin(builtInServerUrl) ?? ""
        func rank(_ relay: RelayProfile) -> Int {
            if !relay.builtIn { return 3 }
            if relay.id == envId { return 0 }
            if relay.id == AppEnv.prodHost { return 1 }
            return 2
        }
        // Swift's sort is not stable; the index keeps each group in list order.
        relays = relays.enumerated()
            .sorted { a, b in
                let ra = rank(a.element)
                let rb = rank(b.element)
                return ra != rb ? ra < rb : a.offset < b.offset
            }
            .map(\.element)
    }

    /// Save a relay that passed the `/api/version` handshake. Adding never switches to it.
    /// Re-adding an existing relay refreshes its handshake stamp instead of duplicating it.
    @discardableResult
    public func add(_ relay: RelayProfile) -> RelayProfile {
        if let existing = byId(relay.id) {
            let updated = existing.copyWith(verifiedAt: relay.verifiedAt, verifiedRev: relay.verifiedRev)
            replace(updated)
            return updated
        }
        relays.append(relay)
        ProjectLegacyMarker.markNew(prefs: prefs, scope: relay.scope)
        persist()
        notifyListeners()
        return relay
    }

    @discardableResult
    public func addNamed(_ name: String, logo: String = "") -> RelayProfile? {
        guard let project = RelayProfile.named(name, now: clock.nowMillis) else { return nil }
        let saved = project.copyWith(logo: logo)
        relays.append(saved)
        ProjectLegacyMarker.markNew(prefs: prefs, scope: saved.scope)
        persist()
        notifyListeners()
        return saved
    }

    @discardableResult
    public func attach(_ id: String, to instance: RelayProfile) -> RelayProfile? {
        guard let relay = byId(id), !relay.hasInstance, instance.hasInstance, !instance.serverUrl.isEmpty else { return nil }
        let attached = relay.attached(to: instance)
        replace(attached)
        return attached
    }

    public func setLogo(_ id: String, _ logo: String) {
        guard let relay = byId(id) else { return }
        replace(relay.copyWith(logo: logo))
    }

    /// Point the app at `url`, adding a relay for it when it is not known yet. The URL is stored
    /// verbatim. This is the compatibility path behind `SettingsService.setServerUrl`; it skips
    /// the `/api/version` handshake by design. Returns the relay now active.
    @discardableResult
    public func useUrl(_ url: String) -> RelayProfile? {
        let clean = RelayDirectory.foldAliasUrl(canonicalize(url.dartTrimmed))
        if clean.isEmpty { return nil }
        guard let id = RelayProfile.normalizeOrigin(clean) else { return nil }
        let existing = byId(id)
        let relay = existing?.copyWith(serverUrl: clean)
            ?? RelayProfile(
                id: id, label: id, serverUrl: clean, addedAt: clock.nowMillis, namespace: ProjectScope.namespaceFor(id))
        if existing == nil {
            relays.append(relay)
        } else {
            relays = relays.map { $0.id == id ? relay : $0 }
        }
        activeIdValue = id
        prefs.set(id, forKey: RelayDirectory.activeKey)
        persist()
        notifyListeners()
        return relay
    }

    public func rename(_ id: String, _ label: String) {
        guard let relay = byId(id) else { return }
        let clean = label.dartTrimmed
        replace(relay.copyWith(label: clean.isEmpty ? relay.id : clean))
    }

    /// Edit a project's name and World host. The app host is the project's identity (every stored
    /// slot is keyed by it), so it is not editable: pointing a project somewhere else is adding a
    /// new one. Built-in projects keep the World site they ship with.
    @discardableResult
    public func update(_ id: String, label: String? = nil, worldUrl: String? = nil) -> RelayProfile? {
        guard let relay = byId(id) else { return nil }
        let cleanLabel = label?.dartTrimmed
        let updated = relay.copyWith(
            label: cleanLabel.map { $0.isEmpty ? relay.id : $0 },
            worldUrl: (worldUrl == nil || relay.builtIn) ? nil : RelayProfile.normalizeWorldUrl(worldUrl ?? ""))
        replace(updated)
        return updated
    }

    /// Record that `feature` is absent on `id`, learned from a 404. Persisted; a relay that
    /// declared its features ignores this.
    public func markFeatureMissing(_ id: String, _ feature: RelayFeature) {
        guard let relay = byId(id) else { return }
        let next = relay.features.markMissing(feature)
        if next == relay.features { return }
        replace(relay.copyWith(features: next))
    }

    public func markVerified(_ id: String, rev: String = "", features: RelayFeatureSet? = nil) {
        guard let relay = byId(id) else { return }
        replace(relay.copyWith(
            verifiedAt: clock.nowMillis,
            verifiedRev: rev.isEmpty ? relay.verifiedRev : rev,
            features: features))
    }

    /// Record that a relay became active (stamping `lastUsedAt`). Callers go through the relay
    /// switch controller; this only writes the preference.
    public func setActive(_ id: String) {
        if !contains(id) || id == activeIdValue { return }
        activeIdValue = id
        prefs.set(id, forKey: RelayDirectory.activeKey)
        let now = clock.nowMillis
        relays = relays.map { $0.id == id ? $0.copyWith(lastUsedAt: now) : $0 }
        persist()
        notifyListeners()
    }

    /// Forget a relay. The built-in relay cannot be removed, nor can the active one (switch away
    /// first). Returns whether it was removed.
    @discardableResult
    public func remove(_ id: String) -> Bool {
        guard let relay = byId(id), !relay.builtIn, id != activeIdValue else { return false }
        relays.removeAll { $0.id == id }
        persist()
        notifyListeners()
        return true
    }

    /// The relay to fall back to when `id` is being removed: the most recently used other relay,
    /// else the built-in one.
    public func fallbackFor(_ id: String) -> RelayProfile {
        // Swift's sort is not stable; sort indices so ties keep list order.
        let others = relays.enumerated().filter { $0.element.id != id }
            .sorted { a, b in
                a.element.lastUsedAt != b.element.lastUsedAt ? a.element.lastUsedAt > b.element.lastUsedAt : a.offset < b.offset
            }
            .map(\.element)
        if others.isEmpty { return active }
        return others.first { $0.lastUsedAt > 0 } ?? others.first { $0.builtIn } ?? others[0]
    }

    private func replace(_ relay: RelayProfile) {
        relays = relays.map { $0.id == relay.id ? relay : $0 }
        persist()
        notifyListeners()
    }

    private func persist() {
        let list = JSONValue.array(relays.map { .object($0.toJSON()) })
        prefs.set(JSON.encode(list), forKey: RelayDirectory.listKey)
        prefs.set(activeIdValue, forKey: RelayDirectory.activeKey)
    }
}
