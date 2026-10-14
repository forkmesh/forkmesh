// The logic half of the relay settings (the Relay section of Settings and the Add Relay sheet),
// the relay networks screen and the relay switch overlay.

import FMAccount
import FMApi
import FMBase
import FMNet
import Foundation
import Observation

/// One relay row: who you are there is the useful second line.
public struct RelayRowInfo: Equatable, Identifiable, Sendable {
    public let relay: RelayProfile
    public let active: Bool
    /// "Signed in as alice", "Browsing as a guest", "Not signed in".
    public let who: String
    public let signedIn: Bool
    /// The host, or "On this network", shown before `who` when the title does not already say it.
    public let place: String
    public let removable: Bool
    public var account: String = ""
    public var avatarPng: String = ""
    public var userStatus: UserStatus = .empty

    public var id: String { relay.id }
}

/// An inline result line under the relay list.
public struct RelayNotice: Equatable, Sendable {
    public let message: String
    public let error: Bool

    public init(message: String, error: Bool) {
        self.message = message
        self.error = error
    }
}

@MainActor
@Observable
public final class RelaySettingsModel {
    @ObservationIgnored public let settings: SettingsService
    @ObservationIgnored public let auth: AuthService
    @ObservationIgnored public let relay: RelayService
    @ObservationIgnored public let switcher: RelaySwitchController?
    @ObservationIgnored let probeFactory: () -> RelayProbe
    @ObservationIgnored let http: HTTPClient?
    public private(set) var statuses: [String: UserStatus] = [:]

    /// Relay id being tested, so only that row shows a spinner.
    public private(set) var testing = ""
    public private(set) var notice: RelayNotice?

    public init(
        settings: SettingsService, auth: AuthService, relay: RelayService, switcher: RelaySwitchController?,
        probe: @escaping () -> RelayProbe, http: HTTPClient? = nil
    ) {
        self.settings = settings
        self.auth = auth
        self.relay = relay
        self.switcher = switcher
        self.probeFactory = probe
        self.http = http
    }

    public var busy: Bool { switcher?.busy ?? false }
    public var canSwitch: Bool { switcher?.canSwitch ?? false }
    public var connection: RelayConnectionState { relay.state }
    public var hasMultiple: Bool { settings.relays.hasMultiple }

    public var rows: [RelayRowInfo] {
        let activeId = settings.relays.activeId
        return settings.relays.relays.map { profile in
            let session = auth.sessionForRelay(profile.id)
            let guest = auth.isSignedInOnRelay(profile.id) && session == nil
            let who =
                session.map { "Signed in as \($0.nodeName)" } ?? (guest ? "Browsing as a guest" : "Not signed in")
            let place: String
            if profile.label != profile.id {
                place = profile.subtitle
            } else if profile.isLoopback {
                place = "On this network"
            } else {
                place = ""
            }
            let active = profile.id == activeId
            return RelayRowInfo(
                relay: profile, active: active, who: who, signedIn: session != nil, place: place,
                removable: !profile.builtIn, account: session?.nodeName ?? "", avatarPng: session?.avatarPng ?? "",
                userStatus: session == nil ? .empty : active ? relay.ownStatus : statuses[profile.id] ?? .empty)
        }
    }

    public func refreshStatuses() async {
        guard let http else { return }
        let activeId = settings.relays.activeId
        var reads: [(String, URL, String)] = []
        for profile in settings.relays.relays where profile.id != activeId && profile.hasInstance {
            guard let token = auth.sessionForRelay(profile.id)?.sessionToken, !token.isEmpty,
                let url = URL(string: "\(profile.httpOrigin)/api/accounts/user-status")
            else { continue }
            reads.append((profile.id, url, token))
        }
        if reads.isEmpty { return }
        let found = await withTaskGroup(of: (String, UserStatus?).self) { group in
            for (id, url, token) in reads {
                group.addTask { (id, await RelaySettingsModel.readStatus(http, url: url, token: token)) }
            }
            var out: [String: UserStatus] = [:]
            for await (id, status) in group {
                if let status { out[id] = status }
            }
            return out
        }
        statuses.merge(found) { _, new in new }
    }

    nonisolated static func readStatus(_ http: HTTPClient, url: URL, token: String) async -> UserStatus? {
        let request = HTTPRequest(
            method: "POST", url: url,
            headers: [
                "Accept": "application/json", "Content-Type": "application/json", "Authorization": "Bearer \(token)",
            ],
            body: Data("{}".utf8), timeout: 8)
        guard let response = try? await Deadline.send(http, request, seconds: 8), response.status == 200 else {
            return nil
        }
        return UserStatus(json: response.jsonObject["userStatus"])
    }

    /// Optional features the active relay does not serve.
    public var missingFeatures: [RelayFeature] {
        let features = settings.relays.active.features
        return RelayFeature.allCases.filter { !$0.core && !features.has($0) }
    }

    public var activeLabel: String { settings.relays.active.label }

    public var missingExplanation: String {
        settings.relays.active.features.isDeclared
            ? "This relay does not serve these APIs, so the app hides them here. Everything else works normally."
            : "These did not answer on this relay, so the app stopped offering them. They will come back if the relay "
                + "starts serving them."
    }

    public var buildLine: String { "Build: \(settings.env.describe(settings.serverUrl))" }

    public static let explanation =
        "Each relay is its own network, with its own account and its own repositories. Switching moves the whole app "
        + "— repos, issues, chat and notifications — to that relay. Repositories you have cloned to this phone stay put."

    public func say(_ message: String, error: Bool = false) {
        notice = message.isEmpty ? nil : RelayNotice(message: message, error: error)
    }

    public func switchTo(_ profile: RelayProfile) async {
        guard let switcher, switcher.canSwitch else { return }
        say("")
        let result = await switcher.switchTo(profile.id)
        if !result.ok && result.error.isEmpty { return }
        if !result.ok {
            say(result.error, error: true)
        } else if result.offline {
            say(result.error, error: true)
        } else {
            say("Now on \(profile.label).")
        }
    }

    public func test(_ profile: RelayProfile) async {
        if !testing.isEmpty { return }
        testing = profile.id
        defer { testing = "" }
        let result = await probeFactory().verify(profile)
        if result.ok {
            settings.relays.markVerified(profile.id, rev: result.rev, features: result.features)
            say("\(profile.label): \(result.message)")
        } else {
            say("\(profile.label): \(result.message)", error: true)
        }
    }

    public func rename(_ profile: RelayProfile, to name: String) {
        settings.relays.rename(profile.id, name)
    }

    public func setLogo(_ profile: RelayProfile, logo: String) {
        settings.relays.setLogo(profile.id, logo)
    }

    /// The confirmation text before removing `profile`.
    public func removeConfirmation(_ profile: RelayProfile) -> String {
        // A project with its own namespace takes its data on this phone with it (its node key,
        // settings and files); a pre-projects row shares production's and leaves it in place.
        let data = profile.scope.isLegacy
            ? "Repositories you have cloned to this phone are not deleted."
            : "Its data on this phone, including repositories cloned for it, is erased."
        let name = profile.hasInstance ? profile.id : profile.label
        if let session = auth.sessionForRelay(profile.id) {
            return "This phone will forget \(name) and sign out of \(session.nodeName) there. \(data)"
        }
        if !profile.hasInstance { return "This phone will forget \(name). \(data)" }
        return "This phone will forget \(name). \(data) You can add it again at any time."
    }

    public func remove(_ profile: RelayProfile) async {
        guard let switcher else { return }
        let result = await switcher.removeRelay(profile.id)
        say(result.ok ? "Removed \(profile.label)." : result.error, error: !result.ok)
    }

    public func added(_ profile: RelayProfile) {
        say("Added \(profile.label). Tap it to switch.")
    }

    public func connectToggle() {
        switch relay.state {
        case .offline: Task { await relay.connect() }
        case .connecting, .connected: Task { await relay.disconnect() }
        }
    }

    /// `_RelayStateBadge` / `_ConnectionPip` label.
    public nonisolated static func stateLabel(_ state: RelayConnectionState) -> String {
        switch state {
        case .connected: return "Online"
        case .connecting: return "Connecting"
        case .offline: return "Offline"
        }
    }
}

/// The Add Relay sheet: ask for an address, verify it, save it when it answers like a relay.
@MainActor
@Observable
public final class AddRelayModel {
    @ObservationIgnored let relays: RelayDirectory
    @ObservationIgnored let probe: RelayProbe

    public var host = "" {
        didSet {
            if host != oldValue && result != nil {
                // A verdict about a host no longer in the box is a lie.
                generation += 1
                result = nil
            }
        }
    }
    public var label = ""
    public private(set) var result: RelayProbeResult?
    public private(set) var checking = false
    @ObservationIgnored private var generation = 0

    public init(relays: RelayDirectory, probe: RelayProbe) {
        self.relays = relays
        self.probe = probe
    }

    public var canAdd: Bool { (result?.ok == true || known != nil) && !checking }

    public var known: RelayProfile? { result?.knownAs.flatMap { relays.byId($0.id) } }

    public var addLabel: String { known.map { "Open \($0.label)" } ?? "Add relay" }
    public var checkLabel: String { canAdd ? "Check again" : "Check relay" }

    /// "Not available here: ..." under a successful check, or nil.
    public var missingLine: String? {
        guard let result, result.ok, !result.missingOptional.isEmpty else { return nil }
        return "Not available here: \(result.missingOptional.map(\.label).joined(separator: " · "))"
    }

    public func check() async {
        generation += 1
        let current = generation
        checking = true
        result = nil
        let outcome = await probe.check(host, existing: relays.relays, label: label)
        if current != generation { return }
        checking = false
        result = outcome
    }

    /// Saves the verified relay (never switches to it); nil when nothing verified.
    public func add() -> RelayProfile? {
        if let known { return known }
        guard let verified = result?.profile else { return nil }
        return relays.add(verified)
    }

    public static let intro =
        "A relay is a ForkMesh network. Enter its address and we will check that it really is one before adding it."
}

@MainActor
@Observable
public final class AddProjectModel {
    @ObservationIgnored let relays: RelayDirectory
    @ObservationIgnored let probe: RelayProbe

    public var name = ""
    public var host = "" {
        didSet {
            if host != oldValue && result != nil {
                generation += 1
                result = nil
            }
        }
    }
    public private(set) var result: RelayProbeResult?
    public private(set) var checking = false
    @ObservationIgnored private var generation = 0

    public init(relays: RelayDirectory, probe: RelayProbe) {
        self.relays = relays
        self.probe = probe
    }

    public var cleanName: String { name.dartTrimmed }
    public var hasHost: Bool { !host.dartTrimmed.isEmpty }
    public var known: RelayProfile? { result?.knownAs.flatMap { relays.byId($0.id) } }

    public var canCreate: Bool {
        if checking { return false }
        if !hasHost { return !cleanName.isEmpty }
        return result?.ok == true || known != nil
    }

    public var needsCheck: Bool { hasHost && !canCreate }

    public var createLabel: String { known.map { "Open \($0.label)" } ?? "Create project" }
    public var checkLabel: String { checking ? "Checking…" : "Check instance" }

    public var missingLine: String? {
        guard let result, result.ok, !result.missingOptional.isEmpty else { return nil }
        return "Not available here: \(result.missingOptional.map(\.label).joined(separator: " · "))"
    }

    public func check() async {
        guard hasHost else { return }
        generation += 1
        let current = generation
        checking = true
        result = nil
        let outcome = await probe.check(host, existing: relays.relays, label: cleanName)
        if current != generation { return }
        checking = false
        result = outcome
    }

    public func create(logo: String = "") -> RelayProfile? {
        guard canCreate else { return nil }
        if !hasHost { return relays.addNamed(cleanName, logo: logo) }
        let saved: RelayProfile
        if let known {
            saved = known
        } else if let verified = result?.profile {
            saved = relays.add(cleanName.isEmpty ? verified : verified.copyWith(label: cleanName))
        } else {
            return nil
        }
        if !logo.isEmpty { relays.setLogo(saved.id, logo) }
        return relays.byId(saved.id) ?? saved
    }

    public static let intro =
        "A project keeps its own sign-in, keys and data on this phone. A name is all it needs; attach an instance "
        + "if it lives on one. Without one it is not linked to any instance and shows no repositories until you choose one."
}

@MainActor
@Observable
public final class AttachInstanceModel {
    @ObservationIgnored let probe: RelayProbe
    @ObservationIgnored let attachTo: @MainActor (RelayProfile) async -> RelaySwitchResult

    public var host = "" {
        didSet {
            if host != oldValue && result != nil {
                generation += 1
                result = nil
            }
        }
    }
    public private(set) var result: RelayProbeResult?
    public private(set) var checking = false
    public private(set) var error = ""
    @ObservationIgnored private var generation = 0

    public init(probe: RelayProbe, attachTo: @escaping @MainActor (RelayProfile) async -> RelaySwitchResult) {
        self.probe = probe
        self.attachTo = attachTo
    }

    public var hasHost: Bool { !host.dartTrimmed.isEmpty }
    public var verified: RelayProfile? { result?.ok == true ? result?.profile : nil }
    public var checkLabel: String { checking ? "Checking…" : "Check instance" }

    public func check() async {
        guard hasHost, !checking else { return }
        generation += 1
        let current = generation
        checking = true
        error = ""
        result = nil
        let outcome = await probe.check(host)
        if current != generation { return }
        checking = false
        result = outcome
    }

    @discardableResult
    public func attach() async -> Bool {
        guard let instance = verified, !checking else { return false }
        checking = true
        error = ""
        let outcome = await attachTo(instance)
        checking = false
        if !outcome.ok { error = outcome.error }
        return outcome.ok
    }

    public static func intro(_ projectName: String) -> String {
        "\(projectName) works on this device without an instance: repositories you keep on the phone stay here. "
            + "Connect one to sign in, sync repositories and chat. Until you do, nothing is loaded from any instance."
    }
}

/// The relay networks screen: every relay's repositories, side by side.
@MainActor
@Observable
public final class RelayNetworksModel {
    public let catalog: CrossRelayCatalog
    @ObservationIgnored let settings: SettingsService
    @ObservationIgnored let switcher: RelaySwitchController?

    public init(catalog: CrossRelayCatalog, settings: SettingsService, switcher: RelaySwitchController?) {
        self.catalog = catalog
        self.settings = settings
        self.switcher = switcher
    }

    public var summary: String {
        if catalog.loading { return "Reading your relays…" }
        let repos = catalog.totalRepositories
        let relays = catalog.entries.count
        return "\(repos) \(repos == 1 ? "repository" : "repositories") across \(relays) \(relays == 1 ? "relay" : "relays")"
    }

    public func refresh() async { await catalog.refresh() }

    /// Switches to the repository's relay when needed; true when the app is now on it.
    public func open(on profile: RelayProfile) async -> Bool {
        if profile.id == settings.relays.activeId { return true }
        guard let switcher else { return false }
        return await switcher.switchTo(profile.id).ok
    }

    public func switchTo(_ profile: RelayProfile) async {
        guard let switcher else { return }
        _ = await switcher.switchTo(profile.id)
        await catalog.refresh()
    }

    public nonisolated static func signedOutNote(_ profile: RelayProfile) -> String {
        "Signed out on \(profile.label). Switch to it to sign in and see its repositories."
    }

    public nonisolated static func sectionTitle(_ entry: RelayCatalogEntry) -> String {
        entry.active ? "\(entry.relay.label) · active" : entry.relay.label
    }
}

extension RelaySwitchController {
    /// The relay switch overlay's title.
    public var overlayTitle: String {
        target.map { "Switching to \($0.label)" } ?? "Switching relay"
    }
}
