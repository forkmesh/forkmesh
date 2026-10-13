import FMBase
import Foundation

/// One area of relay functionality the app can do without.
///
/// The mobile app calls around 44 distinct endpoint families. Requiring a relay to implement all
/// of them would mean "run your own relay" is really "reimplement ForkMesh", which defeats the
/// point of the app being able to point anywhere. So the surface is split: a small core a relay is
/// refused without, and these, each of which can simply not be there.
///
/// A feature that is missing should leave the UI *absent*, not broken. A toggle that silently does
/// nothing is a bug; a section that is not there needs no explanation.
///
/// The raw value is the wire name a relay declares in `/api/version`'s `features` array, so adding a case here is the only change needed.
public enum RelayFeature: String, CaseIterable, Sendable {
    // --- Core. A relay without these is not usable as a relay at all, so RelayProbe refuses to
    // add it rather than letting the user discover an empty app one screen at a time.
    /// `POST /api/accounts/login`, `/logout`, `GET /api/accounts/<name>`.
    case accounts
    /// `GET /api/repositories`: the catalog every repository list reads.
    case repositories
    /// `GET /api/repo/<owner>/<name>/{tree,blob,raw}`: reading code.
    case repoRead

    // --- Optional.
    /// The shared chat room socket and `GET /api/chat/room-key`. Optional because the room path
    /// names one repository (`mainnode/forkmesh`), which a relay serving a different organization
    /// has no reason to host.
    case chat
    /// `GET/POST/DELETE /api/notifications`: the account alert inbox.
    case notifications
    /// `POST /api/accounts/register-device`, `/push-test`.
    ///
    /// This one can never be satisfied by a third-party relay, whatever it implements: the relay
    /// pushes as `com.forkmesh.forkmesh` signed with our own Apple team key. Listed so the UI can
    /// say so, not so a client can build it.
    case push
    /// `POST /api/accounts/event-ticket` and the `/api/nodes/events` socket.
    case accountEvents
    /// `GET /api/sync`: the signed owner-node drain behind "Serve as a node".
    case nodeSync
    /// `/api/repo/<o>/<n>/issues` and the inbox writes behind them.
    case issues
    /// `/api/repo/<o>/<n>/pulls`.
    case pulls
    /// `/api/repo/<o>/<n>/agents/*`: remote agent runs and transcripts.
    case agents
    /// `/api/repo/<o>/<n>/actions/*`.
    case actions
    /// `/api/orgs/*`.
    case orgs
    /// `GET /api/accounts/devices`: the node registry for this account.
    case devices
    /// `POST /api/repository-imports`: mirroring from GitHub/GitLab.
    case imports
    /// `POST /api/bug-reports`.
    case bugReports
    /// `GET /api/ads/serve`, `POST /api/ads/<id>/click`.
    case ads
    /// `GET /api/network/stats`, `/api/network/leaderboards`.
    case networkStats
    /// The account-scoped plugin APIs: `/api/calories`, `/api/steps`, `/api/water`, `/api/sleep`, `/api/sugar-break`, `/api/stretch`, `/api/notes`, `/api/tasks`.
    case plugins
    /// `/api/repo/<o>/<n>/bounty`.
    case bounties
    /// `POST /api/security/owner-keys`: owner device enrollment.
    case ownerKeys
    /// `POST /api/accounts/user-status`: away flag and custom status.
    case userStatus
    /// `GET /api/ios-udid` and its web page.
    case iosUdid

    /// True when a relay is refused at add time without it.
    public var core: Bool {
        switch self {
        case .accounts, .repositories, .repoRead: return true
        default: return false
        }
    }

    /// What the feature is called where the app has to name it.
    public var label: String {
        switch self {
        case .accounts: return "Accounts"
        case .repositories: return "Repositories"
        case .repoRead: return "Code browsing"
        case .chat: return "Chat"
        case .notifications: return "Notifications"
        case .push: return "Push notifications"
        case .accountEvents: return "Live account events"
        case .nodeSync: return "Serving as a node"
        case .issues: return "Issues"
        case .pulls: return "Pull requests"
        case .agents: return "Agents"
        case .actions: return "Actions"
        case .orgs: return "Organizations"
        case .devices: return "Account devices"
        case .imports: return "Repository import"
        case .bugReports: return "Bug reports"
        case .ads: return "Sponsored messages"
        case .networkStats: return "Network stats"
        case .plugins: return "Plugins"
        case .bounties: return "Bounties"
        case .ownerKeys: return "Owner signing"
        case .userStatus: return "Status"
        case .iosUdid: return "UDID lookup"
        }
    }

    /// The wire name a relay declares in `/api/version`'s `features` array.
    public var wireName: String { rawValue }

    public static func fromWireName(_ value: String) -> RelayFeature? {
        RelayFeature(rawValue: value.dartTrimmed)
    }

    public static var coreFeatures: Set<RelayFeature> { Set(allCases.filter(\.core)) }

    /// The feature a 404/405 on `path` proves absent, or nil when the answer is ambiguous.
    ///
    /// Deliberately covers only relay-wide, account-scoped routes. A 404 under
    /// `/api/repo/<owner>/<name>/...` is far more likely to mean "no such repository" than "this
    /// relay has no issues", so repo-scoped features are learned only from a relay's own
    /// declaration.
    public static func learnableFromPath(_ path: String) -> RelayFeature? {
        let clean = path.lowercased()
        if clean.hasPrefix("/api/repo/") { return nil }
        for (prefix, feature) in learnablePrefixes {
            if clean == prefix || clean.hasPrefix(prefix + "/") || clean.hasPrefix(prefix + "?") {
                return feature
            }
        }
        return nil
    }

    // No prefix is a prefix of another, so order does not change answers.
    private static let learnablePrefixes: [(String, RelayFeature)] = [
        ("/api/notifications", .notifications),
        ("/api/accounts/register-device", .push),
        ("/api/accounts/push-test", .push),
        ("/api/accounts/event-ticket", .accountEvents),
        ("/api/nodes/events", .accountEvents),
        ("/api/sync", .nodeSync),
        ("/api/orgs", .orgs),
        ("/api/accounts/devices", .devices),
        ("/api/accounts/user-status", .userStatus),
        ("/api/repository-imports", .imports),
        ("/api/bug-reports", .bugReports),
        ("/api/ads", .ads),
        ("/api/network", .networkStats),
        ("/api/calories", .plugins),
        ("/api/steps", .plugins),
        ("/api/water", .plugins),
        ("/api/sleep", .plugins),
        ("/api/sugar-break", .plugins),
        ("/api/stretch", .plugins),
        ("/api/notes", .plugins),
        ("/api/tasks", .plugins),
        ("/api/security/owner-keys", .ownerKeys),
        ("/api/chat/room-key", .chat),
        ("/api/ios-udid", .iosUdid),
    ]
}

/// What one relay supports.
///
/// Three states, not two, because "this relay says it has no notifications" and "we have never
/// asked" call for different behaviour: the first hides the surface outright, the second shows it
/// and finds out. Equality is by content.
public struct RelayFeatureSet: Equatable, Hashable, Sendable {
    private let declared: Set<RelayFeature>?
    private let knownMissing: Set<RelayFeature>

    private init(declared: Set<RelayFeature>?, knownMissing: Set<RelayFeature>) {
        self.declared = declared
        self.knownMissing = knownMissing
    }

    /// A relay that told us what it has. Anything absent is genuinely absent.
    public static func declared(_ features: Set<RelayFeature>) -> RelayFeatureSet {
        RelayFeatureSet(declared: features, knownMissing: [])
    }

    /// A relay that has not declared anything: every relay built before the `features` field
    /// existed, including our own production one. Everything is assumed present and the app
    /// learns otherwise from a 404.
    public static func undeclared(_ knownMissing: Set<RelayFeature> = []) -> RelayFeatureSet {
        RelayFeatureSet(declared: nil, knownMissing: knownMissing)
    }

    public var isDeclared: Bool { declared != nil }

    /// Whether the app should offer `feature` at all.
    public func has(_ feature: RelayFeature) -> Bool {
        if let declared { return declared.contains(feature) }
        return !knownMissing.contains(feature)
    }

    public var hasCore: Bool { RelayFeature.coreFeatures.allSatisfy(has) }

    /// Core features this relay is missing, for the refusal message.
    public var missingCore: Set<RelayFeature> { RelayFeature.coreFeatures.filter { !has($0) } }

    /// Record that `feature` answered 404/405. Only meaningful on an undeclared relay; a relay that
    /// declared its features is taken at its word.
    public func markMissing(_ feature: RelayFeature) -> RelayFeatureSet {
        if isDeclared || knownMissing.contains(feature) { return self }
        return .undeclared(knownMissing.union([feature]))
    }

    /// Wire names, in declaration order (the order carries no meaning). A leading "!" marks a feature learned missing on an undeclared relay.
    public func toJSON() -> [String] {
        if let declared {
            return RelayFeature.allCases.filter { declared.contains($0) }.map(\.wireName)
        }
        return RelayFeature.allCases.filter { knownMissing.contains($0) }.map { "!" + $0.wireName }
    }

    public var jsonValue: JSONValue { JSONValue(toJSON()) }

    public static func fromJSON(_ value: JSONValue?) -> RelayFeatureSet {
        guard let items = value?.arrayValue else { return .undeclared() }
        var missing = Set<RelayFeature>()
        var declared = Set<RelayFeature>()
        for item in items {
            let raw = item.text
            if raw.hasPrefix("!") {
                if let feature = RelayFeature.fromWireName(String(raw.dropFirst())) { missing.insert(feature) }
            } else if let feature = RelayFeature.fromWireName(raw) {
                declared.insert(feature)
            }
        }
        if declared.isEmpty && !missing.isEmpty { return .undeclared(missing) }
        if declared.isEmpty { return .undeclared() }
        return .declared(declared)
    }

    /// Read the `features` array out of an `/api/version` body. An empty or entirely unrecognised
    /// array is "declared nothing" rather than "did not declare".
    public static func fromVersionBody(_ body: JSONObject) -> RelayFeatureSet {
        guard let items = body["features"]?.arrayValue else { return .undeclared() }
        var declared = Set<RelayFeature>()
        for item in items {
            if let feature = RelayFeature.fromWireName(item.text) { declared.insert(feature) }
        }
        return .declared(declared)
    }
}
