// Kotlin counterparts: api/AccountSettings.kt and app/.../feature/account/SettingsService.kt.

import FMBase
import Foundation
import Observation

/// The appearance setting, stored by name (`appearance/themeMode`). The app target maps it onto
/// SwiftUI's `ColorScheme?`.
public enum ThemeMode: String, CaseIterable, Sendable {
    case system
    case light
    case dark
}

/// The accent colour picked in Settings, stored by name
/// (`appearance/accent`). The app target's `FmAccent` holds the colours; map with
/// `FmAccent.fromName(settings.accent.rawValue)`.
public enum AccentChoice: String, CaseIterable, Sendable {
    case blue
    case cyan
    case teal
    case green
    case lime
    case amber
    case orange
    case red
    case pink
    case violet
    case purple
    case graphite

    public static let fallback: AccentChoice = .blue

    /// The label shown in the picker.
    public var label: String {
        switch self {
        case .blue: return "Blue"
        case .cyan: return "Cyan"
        case .teal: return "Teal"
        case .green: return "Green"
        case .lime: return "Lime"
        case .amber: return "Amber"
        case .orange: return "Orange"
        case .red: return "Red"
        case .pink: return "Pink"
        case .violet: return "Violet"
        case .purple: return "Purple"
        case .graphite: return "Graphite"
        }
    }

    /// Resolves a stored name, falling back to `fallback` for anything unknown.
    public static func fromName(_ name: String?) -> AccentChoice {
        name.flatMap(AccentChoice.init(rawValue:)) ?? fallback
    }
}

/// Persisted user/app settings, mirroring the Qt client's QSettings keys: display name, relay
/// URL/room/passphrase, import tokens, and the app preferences below.
///
/// Every getter reads the store (so a value written by another service shows at once) and
/// registers with Observation through `revision`, which every change bumps. Code outside SwiftUI
/// uses `addListener`.
@MainActor
@Observable
public final class SettingsService {
    // MARK: Keys (identical to the retired Flutter app's)

    nonisolated public static let serverUrlKey = "server/url"
    nonisolated public static let localWorkerUrlMigrationKey = "server/localWorkerUrlMigrated"
    nonisolated static let serverUrlEnvKey = "server/urlEnv"
    nonisolated public static let passphraseKey = "server/passphrase"
    /// Legacy manual-seal storage, kept only so `clearSecrets` also removes a value an older build
    /// wrote. Chat E2EE creates its own X25519 identity automatically.
    nonisolated public static let roomSealKey = "server/roomSeal"
    nonisolated public static let githubTokenKey = "import/githubToken"
    nonisolated public static let gitlabTokenKey = "import/gitlabToken"
    nonisolated static let secretKeys = [passphraseKey, roomSealKey, githubTokenKey, gitlabTokenKey]

    nonisolated public static let displayNameKey = "profile/name"
    nonisolated public static let roomKey = "server/room"
    nonisolated public static let bugButtonAnchorKey = "bugReport/anchor"
    nonisolated public static let bugButtonLeftKey = "bugReport/onLeft"
    nonisolated public static let themeModeKey = "appearance/themeMode"
    nonisolated public static let accentKey = "appearance/accent"
    nonisolated public static let backgroundImageKey = "appearance/backgroundImage"
    nonisolated public static let backgroundDimKey = "appearance/backgroundDim"
    nonisolated public static let defaultBackgroundDim: Double = 0.4
    nonisolated public static let maxBackgroundDim: Double = 0.9
    nonisolated public static let serveAsNodeKey = "node/serve"
    nonisolated public static let iosUdidKey = "device/iosUdid"
    nonisolated public static let iosUdidProductKey = "device/iosUdidProduct"
    nonisolated public static let iosUdidVersionKey = "device/iosUdidVersion"
    nonisolated public static let iosUdidNonceKey = "device/iosUdidNonce"
    nonisolated public static let showSponsoredKey = "ads/showSponsored"
    nonisolated public static let sponsoredHiddenUntilKey = "ads/hiddenUntilMs"
    nonisolated public static let showTelemetryStripKey = "diagnostics/showStrip"
    nonisolated public static let showAgentDotsKey = "agentWatch/showDots"
    nonisolated public static let signInOnKey = "packages/sign-in/on"
    nonisolated public static let aquariumOnKey = "packages/aquarium/on"
    nonisolated public static let removedPackagesKey = "packages/removed"
    /// Seven days, in milliseconds.
    nonisolated public static let sponsoredHideDurationMs: Int64 = 7 * 24 * 60 * 60 * 1000

    nonisolated public static let defaultRoom = "world-general"
    nonisolated public static let defaultPassphrase = ""

    nonisolated public static let legacyPublicRoomPassphrase = "forkmesh-public-room"

    /// The retired local-Worker URL of the Android emulator build (`AppEnv.androidEmulatorServerUrl`).
    nonisolated public static let androidEmulatorLocalWorkerServerUrl =
        "ws://\(AppEnv.androidEmulatorHost):\(AppEnv.devPort)\(AppEnv.relayPath)"

    // MARK: State

    @ObservationIgnored public let prefs: KeyValueStore
    @ObservationIgnored let secure: SecureStore
    @ObservationIgnored public let env: AppEnv
    @ObservationIgnored let clock: Clock
    @ObservationIgnored public let listeners = Listeners()
    @ObservationIgnored private var relaysToken: ListenerToken?

    /// Every relay this phone knows about, and which one is active. The app reads the *active*
    /// relay through `serverUrl`; this is for the Settings screen and the relay switch controller.
    public let relays: RelayDirectory

    /// Bumped by every change; the getters read it so SwiftUI re-renders on any settings change.
    public private(set) var revision = 0

    private var passphraseValue = SettingsService.defaultPassphrase
    private var githubTokenValue = ""
    private var gitlabTokenValue = ""

    /// A service over `prefs` without the startup migrations (tests, previews). The app uses
    /// `create`.
    public init(
        prefs: KeyValueStore,
        secureStore: SecureStore,
        relays: RelayDirectory? = nil,
        env: AppEnv = .prod,
        clock: Clock = SystemClock()
    ) {
        self.prefs = prefs
        self.secure = secureStore
        self.env = env
        self.clock = clock
        self.relays = relays ?? RelayDirectory(prefs: prefs, env: env, clock: clock)
        // Every call site reads the relay through `serverUrl`, so a change of active relay has to
        // look like a settings change to them.
        relaysToken = self.relays.addListener { [weak self] in
            MainActor.assumeIsolated { self?.notifyListeners() }
        }
    }

    /// `SettingsService.create`: migrate plaintext secrets into the secure store, drop the retired
    /// room phrase, run the two single-relay migrations, load the relay directory, read secrets.
    public static func create(
        prefs: KeyValueStore,
        secureStore: SecureStore,
        env: AppEnv = .prod,
        clock: Clock = SystemClock()
    ) -> SettingsService {
        secureStore.migrateFromPrefs(prefs, keys: secretKeys)
        // Manual room phrases are obsolete and must not linger as unused secret material.
        secureStore.delete(roomSealKey)
        migrateLocalWorkerServerUrl(prefs)
        retireStoredSounds(prefs)
        // Both single-relay migrations run before the directory is built: they work on the legacy
        // `server/url` string, and the directory adopts whatever survives them.
        adoptEnvironmentServerUrl(prefs, env: env)
        let relays = RelayDirectory.load(prefs: prefs, env: env, clock: clock, canonicalizeUrl: canonicalServerUrl)
        let service = SettingsService(prefs: prefs, secureStore: secureStore, relays: relays, env: env, clock: clock)
        service.loadSecrets()
        service.settleLegacyProject()
        return service
    }

    // MARK: Listeners

    @discardableResult
    public func addListener(_ listener: @escaping @Sendable () -> Void) -> ListenerToken { listeners.add(listener) }

    public func removeListener(_ token: ListenerToken) { listeners.remove(token) }

    /// Stop forwarding relay changes.
    public func dispose() {
        if let relaysToken { relays.removeListener(relaysToken) }
        relaysToken = nil
    }

    private func notifyListeners() {
        revision &+= 1
        listeners.notify()
    }

    /// Reading `revision` registers the caller with Observation; every getter goes through it.
    private func track() { _ = revision }

    // MARK: Migrations

    /// Drop a saved relay URL that is one of the retired local-Worker defaults, once.
    nonisolated public static func migrateLocalWorkerServerUrl(_ prefs: KeyValueStore) {
        if prefs.bool(forKey: localWorkerUrlMigrationKey) == true { return }
        prefs.set(true, forKey: localWorkerUrlMigrationKey)
        guard let stored = prefs.string(forKey: serverUrlKey) else { return }
        if isRetiredLocalWorkerServerUrl(stored) { prefs.remove(serverUrlKey) }
    }

    /// Whether `value` is a relay URL a previous build could have defaulted to. Canonicalization
    /// also matches the legacy `rooms/general` path.
    nonisolated public static func isRetiredLocalWorkerServerUrl(_ value: String) -> Bool {
        [AppEnv.devServerUrl, androidEmulatorLocalWorkerServerUrl].contains(canonicalServerUrl(value))
    }

    /// Drop a persisted relay URL that belongs to a different environment than this build's. The
    /// environment name is recorded alongside the URL so a deliberate override survives.
    nonisolated static func adoptEnvironmentServerUrl(_ prefs: KeyValueStore, env: AppEnv) {
        let envName = env.environment.rawValue
        let storedEnv = prefs.string(forKey: serverUrlEnvKey)
        if storedEnv == envName { return }
        // Only an actual environment *change* invalidates a stored relay.
        let stored = prefs.string(forKey: serverUrlKey)?.dartTrimmed ?? ""
        let mismatched = storedEnv != nil && !stored.isEmpty && AppEnv.isLoopbackUrl(stored) != env.isDev
        if mismatched { prefs.remove(serverUrlKey) }
        prefs.set(envName, forKey: serverUrlEnvKey)
    }

    private func loadSecrets() {
        passphraseValue = SettingsService.normalizePassphrase(secure.read(SettingsService.passphraseKey) ?? "")
        githubTokenValue = secure.read(SettingsService.githubTokenKey) ?? ""
        gitlabTokenValue = secure.read(SettingsService.gitlabTokenKey) ?? ""
    }

    // MARK: Relay

    /// Relay URL this build targets before any user override.
    public var defaultServerUrl: String { env.defaultServerUrl }

    /// The active relay's URL. Every REST call, socket and sign-in resolves its origin from here.
    public var serverUrl: String {
        track()
        return relays.activeServerUrl
    }

    /// `serverUrl` parsed with `ServerURL.parse`.
    public var server: ServerURL { ServerURL.parse(serverUrl) }

    /// Whether the active relay serves `feature`. A surface whose relay does not support it
    /// should not be drawn.
    public func supports(_ feature: RelayFeature) -> Bool {
        track()
        return relays.activeFeatures.has(feature)
    }

    /// Point the app at `v`, adding a relay for it if it is not known yet. Kept for code and tests
    /// that name a relay URL directly; it neither verifies the host nor takes the running app
    /// across, so it is not what a screen should call.
    public func setServerUrl(_ v: String) {
        let clean = v.dartTrimmed
        if clean.isEmpty {
            resetServerUrl()
            return
        }
        relays.useUrl(clean)
    }

    /// True when the active relay is not the one this build ships with.
    public var hasServerUrlOverride: Bool {
        track()
        return !relays.active.builtIn
    }

    /// Return to the relay this build ships with. Other relays stay in the list, with their
    /// sessions: this is a switch, not a forget.
    public func resetServerUrl() {
        guard let builtIn = relays.relays.first(where: { $0.builtIn }) else { return }
        relays.setActive(builtIn.id)
    }

    // MARK: Project

    /// The active project: one ForkMesh instance and everything this phone holds for it.
    public var project: RelayProfile {
        track()
        return relays.active
    }

    /// The active project's id (its origin).
    public var projectId: String { project.id }

    /// Where the active project's data lives (see `ProjectScope`). WS2 (per-project push) and WS5
    /// (per-project local repositories) address their storage through this.
    public var scope: ProjectScope { project.scope }

    /// The public World site of the active project. The relay URL is deliberately not used:
    /// tokens and room paths must never reach the URL launcher.
    public var worldUri: URL? { project.worldURL ?? URL(string: env.worldSiteUrl) }

    /// `SettingsService.worldUriForServerUrl`: the World site of the active project.
    public func worldUriForServerUrl(_: String) -> URL? { worldUri }

    /// The active project's public homepage (terms, privacy, plans).
    public var homepageUri: URL? { project.homepageURL }

    /// Re-read the secrets after a project switch moved the secure store to another namespace,
    /// and tell every listener that the settings changed.
    public func reloadProjectSecrets() {
        loadSecrets()
        notifyListeners()
    }

    /// Fold the retired room path and the `forkmesh.com` / `api.forkmesh.com` aliases onto the
    /// canonical production URL. Anything that is not a `ws(s)` URL on a shared room path is
    /// returned unchanged.
    nonisolated public static func canonicalServerUrl(_ value: String) -> String {
        let trimmed = value.dartTrimmed
        guard var components = URLComponents(string: trimmed),
              let scheme = components.scheme?.lowercased(), scheme == "ws" || scheme == "wss" else {
            return value
        }
        let legacyPath = "/api/repo/mainnode/forkmesh/rooms/general/ws"
        let currentPath = "/api/repo/mainnode/forkmesh/rooms/world-general/ws"
        let path = components.percentEncodedPath
        guard path == legacyPath || path == currentPath else { return value }
        let host = (components.host ?? "").lowercased()
        let canonicalHost = (host == AppEnv.prodHost || AppEnv.isProdAlias(host)) && components.port == nil
        // Scheme and host are normalized to lower case.
        components.scheme = canonicalHost ? "wss" : scheme
        components.host = canonicalHost ? "app.forkmesh.com" : host
        components.percentEncodedPath = currentPath
        return components.string ?? value
    }

    // MARK: Profile, room, secrets

    public var displayName: String {
        track()
        return prefs.string(forKey: SettingsService.displayNameKey) ?? ""
    }

    public var room: String {
        track()
        return prefs.string(forKey: SettingsService.roomKey) ?? SettingsService.defaultRoom
    }

    public var passphrase: String {
        track()
        return passphraseValue
    }

    public var githubToken: String {
        track()
        return githubTokenValue
    }

    public var gitlabToken: String {
        track()
        return gitlabTokenValue
    }

    public func setDisplayName(_ v: String) { set(SettingsService.displayNameKey, v) }

    public func setRoom(_ v: String) { set(SettingsService.roomKey, v) }

    public func setPassphrase(_ v: String) {
        let clean = SettingsService.normalizePassphrase(v)
        passphraseValue = clean
        if clean.isEmpty {
            secure.delete(SettingsService.passphraseKey)
        } else {
            secure.write(SettingsService.passphraseKey, clean)
        }
        notifyListeners()
    }

    public func setGithubToken(_ v: String) {
        githubTokenValue = v
        if v.isEmpty {
            secure.delete(SettingsService.githubTokenKey)
        } else {
            secure.write(SettingsService.githubTokenKey, v)
        }
        notifyListeners()
    }

    public func setGitlabToken(_ v: String) {
        gitlabTokenValue = v
        if v.isEmpty {
            secure.delete(SettingsService.gitlabTokenKey)
        } else {
            secure.write(SettingsService.gitlabTokenKey, v)
        }
        notifyListeners()
    }

    public func clearSecrets() {
        for key in SettingsService.secretKeys {
            secure.delete(key)
            prefs.remove(key)
        }
        passphraseValue = SettingsService.defaultPassphrase
        githubTokenValue = ""
        gitlabTokenValue = ""
        notifyListeners()
    }

    // MARK: Bug report control
    //
    // The report button floats over the app and is draggable. Stored as a fraction of the vertical
    // travel and a side, not as pixels, so a rotation or split screen cannot put it off-screen.

    /// Where the report button sits vertically, 0 at the top of its travel and 1 at the bottom.
    /// Defaults low on the screen, near the thumb.
    public var bugButtonAnchor: Double {
        track()
        guard let stored = prefs.double(forKey: SettingsService.bugButtonAnchorKey), !stored.isNaN else { return 0.72 }
        return min(max(stored, 0), 1)
    }

    /// True when the button has been dragged to the leading edge.
    public var bugButtonOnLeft: Bool {
        track()
        return prefs.bool(forKey: SettingsService.bugButtonLeftKey) ?? false
    }

    public func setBugButtonPlacement(anchor: Double, onLeft: Bool) {
        prefs.set(anchor.isNaN ? 0.72 : min(max(anchor, 0), 1), forKey: SettingsService.bugButtonAnchorKey)
        prefs.set(onLeft, forKey: SettingsService.bugButtonLeftKey)
        notifyListeners()
    }

    // MARK: Appearance (stored by name, never by index)

    /// Light, dark, or following the system. Defaults to the system.
    public var themeMode: ThemeMode {
        track()
        return prefs.string(forKey: SettingsService.themeModeKey).flatMap(ThemeMode.init(rawValue:)) ?? .system
    }

    public func setThemeMode(_ mode: ThemeMode) { set(SettingsService.themeModeKey, mode.rawValue) }

    /// The accent colour picked in Settings. Defaults to blue.
    public var accent: AccentChoice {
        track()
        return AccentChoice.fromName(prefs.string(forKey: SettingsService.accentKey))
    }

    public func setAccent(_ accent: AccentChoice) { set(SettingsService.accentKey, accent.rawValue) }

    public var backgroundImage: String {
        track()
        let name: String = prefs.string(forKey: SettingsService.backgroundImageKey) ?? ""
        return name.contains("/") ? "" : name
    }

    public func setBackgroundImage(_ name: String) { set(SettingsService.backgroundImageKey, name) }

    public var backgroundDim: Double {
        track()
        return SettingsService.clampBackgroundDim(prefs.double(forKey: SettingsService.backgroundDimKey))
    }

    public func setBackgroundDim(_ dim: Double) {
        prefs.set(SettingsService.clampBackgroundDim(dim), forKey: SettingsService.backgroundDimKey)
        notifyListeners()
    }

    nonisolated static func clampBackgroundDim(_ dim: Double?) -> Double {
        guard let dim, !dim.isNaN else { return defaultBackgroundDim }
        return min(max(dim, 0), maxBackgroundDim)
    }

    // MARK: Serving as a node
    //
    // Signing in binds this phone's key as a `mobile_node`, but acting as the account's node (the
    // signed /api/sync drain and the /api/nodes/events socket) is opt-in.

    public var serveAsNode: Bool {
        track()
        return prefs.bool(forKey: SettingsService.serveAsNodeKey) ?? false
    }

    public func setServeAsNode(_ v: Bool) {
        prefs.set(v, forKey: SettingsService.serveAsNodeKey)
        notifyListeners()
    }

    // MARK: iOS UDID (read once through the web profile service; see IosUdidLink)

    /// This iPhone's UDID, or empty until a read has come back.
    public var iosUdid: String {
        track()
        return prefs.string(forKey: SettingsService.iosUdidKey) ?? ""
    }

    /// Apple's model identifier sent with the UDID, e.g. `iPhone15,2`.
    public var iosUdidProduct: String {
        track()
        return prefs.string(forKey: SettingsService.iosUdidProductKey) ?? ""
    }

    /// The iOS build sent with the UDID, e.g. `22A3354`.
    public var iosUdidVersion: String {
        track()
        return prefs.string(forKey: SettingsService.iosUdidVersionKey) ?? ""
    }

    /// Start a read: remember a fresh nonce and return it. Persisted because iOS may end the app
    /// while the user is in Safari and Settings.
    public func beginIosUdidRead() -> String {
        let nonce = IosUdidLink.newNonce()
        prefs.set(nonce, forKey: SettingsService.iosUdidNonceKey)
        return nonce
    }

    /// Save `link` if it answers the read this app started; any other nonce is ignored.
    @discardableResult
    public func acceptIosUdidLink(_ link: IosUdidLink) -> Bool {
        let pending = prefs.string(forKey: SettingsService.iosUdidNonceKey) ?? ""
        if pending.isEmpty || pending != link.nonce { return false }
        prefs.remove(SettingsService.iosUdidNonceKey)
        prefs.set(link.udid, forKey: SettingsService.iosUdidKey)
        prefs.set(link.product, forKey: SettingsService.iosUdidProductKey)
        prefs.set(link.version, forKey: SettingsService.iosUdidVersionKey)
        notifyListeners()
        return true
    }

    // MARK: Sponsored messages (purely local controls)

    /// Whether sponsored messages may be shown at all. On by default.
    public var showSponsoredMessages: Bool {
        track()
        return prefs.bool(forKey: SettingsService.showSponsoredKey) ?? true
    }

    public func setShowSponsoredMessages(_ v: Bool) {
        prefs.set(v, forKey: SettingsService.showSponsoredKey)
        notifyListeners()
    }

    public var showTelemetryStrip: Bool {
        track()
        return prefs.bool(forKey: SettingsService.showTelemetryStripKey) ?? true
    }

    public func setShowTelemetryStrip(_ v: Bool) {
        prefs.set(v, forKey: SettingsService.showTelemetryStripKey)
        notifyListeners()
    }

    public var showAgentDots: Bool {
        track()
        return prefs.bool(forKey: SettingsService.showAgentDotsKey) ?? false
    }

    public func setShowAgentDots(_ v: Bool) {
        prefs.set(v, forKey: SettingsService.showAgentDotsKey)
        notifyListeners()
    }

    @discardableResult
    public func settleLegacyProject() -> Bool {
        ProjectLegacyMarker.settle(prefs: prefs, secure: secure, scope: scope)
    }

    public var legacyProject: Bool {
        track()
        return ProjectLegacyMarker.stored(prefs, scope: scope) ?? settleLegacyProject()
    }

    public var signInOn: Bool {
        track()
        return prefs.bool(forKey: SettingsService.signInOnKey) ?? true
    }

    public func setSignInOn(_ v: Bool) {
        prefs.set(v, forKey: SettingsService.signInOnKey)
        notifyListeners()
    }

    public var aquariumOn: Bool {
        track()
        return prefs.bool(forKey: SettingsService.aquariumOnKey) ?? false
    }

    public func setAquariumOn(_ v: Bool) {
        prefs.set(v, forKey: SettingsService.aquariumOnKey)
        notifyListeners()
    }

    public var removedPackages: Set<String> {
        track()
        return Set(prefs.stringList(forKey: SettingsService.removedPackagesKey) ?? [])
    }

    public func setPackageRemoved(_ id: String, removed: Bool) {
        var next = Set(prefs.stringList(forKey: SettingsService.removedPackagesKey) ?? [])
        if removed { next.insert(id) } else { next.remove(id) }
        prefs.set(next.sorted(), forKey: SettingsService.removedPackagesKey)
        notifyListeners()
    }

    /// True while a "Hide" tap is still in force.
    public var sponsoredMessagesHidden: Bool {
        track()
        let until = Int64(prefs.int(forKey: SettingsService.sponsoredHiddenUntilKey) ?? 0)
        return until > clock.nowMillis
    }

    /// Remember a "Hide" tap for seven days.
    public func hideSponsoredMessages() {
        prefs.set(Int(clock.nowMillis + SettingsService.sponsoredHideDurationMs), forKey: SettingsService.sponsoredHiddenUntilKey)
        notifyListeners()
    }

    nonisolated public static let hapticsKey = "device/haptics"
    nonisolated public static let soundsOffKey = "device/soundsOff"
    nonisolated public static let retiredSoundsKeys = ["device/sounds", "sounds.enabled"]
    nonisolated public static let dictationLocaleKey = "voice/dictationLocale"
    nonisolated public static let dictateOnOpenKey = "voice/dictateOnOpen"
    nonisolated public static let localAuthorNameKey = "repos/authorName"
    nonisolated public static let localAuthorEmailKey = "repos/authorEmail"
    nonisolated public static let localDefaultBranchKey = "repos/defaultBranch"
    nonisolated public static let fallbackBranch = "main"

    nonisolated public static func hapticsEnabled(_ prefs: KeyValueStore) -> Bool {
        prefs.bool(forKey: hapticsKey) ?? true
    }

    nonisolated public static func soundsEnabled(_ prefs: KeyValueStore) -> Bool {
        prefs.bool(forKey: soundsOffKey) != true
    }

    nonisolated public static func retireStoredSounds(_ prefs: KeyValueStore) {
        for key in retiredSoundsKeys where prefs.contains(key) {
            prefs.remove(key)
        }
    }

    nonisolated public static func dictationLocale(_ prefs: KeyValueStore) -> String {
        prefs.string(forKey: dictationLocaleKey) ?? ""
    }

    nonisolated public static func dictatesOnOpen(_ prefs: KeyValueStore) -> Bool {
        prefs.bool(forKey: dictateOnOpenKey) ?? true
    }

    nonisolated public static func localAuthorOverride(_ prefs: KeyValueStore) -> (name: String, email: String)? {
        let name = (prefs.string(forKey: localAuthorNameKey) ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        let email = (prefs.string(forKey: localAuthorEmailKey) ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        if name.isEmpty && email.isEmpty { return nil }
        return (name, email)
    }

    nonisolated public static func localDefaultBranch(_ prefs: KeyValueStore) -> String {
        let stored = (prefs.string(forKey: localDefaultBranchKey) ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        return isValidBranchName(stored) ? stored : fallbackBranch
    }

    nonisolated public static func isValidBranchName(_ name: String) -> Bool {
        guard !name.isEmpty, name.count <= 100, !name.hasPrefix("-"), !name.hasPrefix("/"), !name.hasSuffix("/"),
              !name.hasSuffix(".lock"), !name.hasSuffix("."), !name.contains(".."), !name.contains("//"), !name.contains("@{")
        else { return false }
        let banned: Set<Character> = [" ", "~", "^", ":", "?", "*", "[", "\\"]
        return !name.contains(where: { banned.contains($0) || $0.isNewline || ($0.asciiValue.map { $0 < 32 || $0 == 127 } ?? false) })
    }

    nonisolated public static func isPlausibleEmail(_ email: String) -> Bool {
        let parts = email.split(separator: "@", omittingEmptySubsequences: false)
        guard parts.count == 2, !parts[0].isEmpty, parts[1].contains("."), !parts[1].hasPrefix("."), !parts[1].hasSuffix(".") else {
            return false
        }
        return email.count <= 254 && !email.contains(where: { $0.isWhitespace || $0 == "<" || $0 == ">" })
    }

    public var hapticsEnabled: Bool {
        track()
        return SettingsService.hapticsEnabled(prefs)
    }

    public func setHapticsEnabled(_ v: Bool) {
        prefs.set(v, forKey: SettingsService.hapticsKey)
        notifyListeners()
    }

    public var soundsEnabled: Bool {
        track()
        return SettingsService.soundsEnabled(prefs)
    }

    public func setSoundsEnabled(_ v: Bool) {
        if v {
            prefs.remove(SettingsService.soundsOffKey)
        } else {
            prefs.set(true, forKey: SettingsService.soundsOffKey)
        }
        notifyListeners()
    }

    public var dictationLocale: String {
        track()
        return SettingsService.dictationLocale(prefs)
    }

    public func setDictationLocale(_ identifier: String) {
        let cleaned = String(identifier.filter { $0.isLetter || $0.isNumber || $0 == "-" || $0 == "_" }.prefix(35))
        if cleaned.isEmpty {
            prefs.remove(SettingsService.dictationLocaleKey)
            notifyListeners()
        } else {
            set(SettingsService.dictationLocaleKey, cleaned)
        }
    }

    public var dictatesOnOpen: Bool {
        track()
        return SettingsService.dictatesOnOpen(prefs)
    }

    public func setDictatesOnOpen(_ v: Bool) {
        prefs.set(v, forKey: SettingsService.dictateOnOpenKey)
        notifyListeners()
    }

    public var localAuthorName: String {
        track()
        return prefs.string(forKey: SettingsService.localAuthorNameKey) ?? ""
    }

    public var localAuthorEmail: String {
        track()
        return prefs.string(forKey: SettingsService.localAuthorEmailKey) ?? ""
    }

    public func setLocalAuthorName(_ v: String) {
        set(SettingsService.localAuthorNameKey, String(v.filter { !$0.isNewline && $0 != "<" && $0 != ">" }.prefix(64)))
    }

    public func setLocalAuthorEmail(_ v: String) {
        set(SettingsService.localAuthorEmailKey, String(v.filter { !$0.isWhitespace && $0 != "<" && $0 != ">" }.prefix(254)))
    }

    public var localDefaultBranch: String {
        track()
        return SettingsService.localDefaultBranch(prefs)
    }

    @discardableResult
    public func setLocalDefaultBranch(_ v: String) -> Bool {
        let trimmed = v.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty {
            prefs.remove(SettingsService.localDefaultBranchKey)
            notifyListeners()
            return true
        }
        guard SettingsService.isValidBranchName(trimmed) else { return false }
        set(SettingsService.localDefaultBranchKey, trimmed)
        return true
    }

    // MARK: Helpers

    private func set(_ key: String, _ value: String) {
        prefs.set(value, forKey: key)
        notifyListeners()
    }

    nonisolated static func normalizePassphrase(_ v: String) -> String {
        v == legacyPublicRoomPassphrase ? "" : v
    }
}
