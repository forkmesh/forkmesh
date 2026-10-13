// The `/api/version` handshake that decides whether a host the user typed is really a ForkMesh
// relay before it may enter the relay list.

import FMAccount
import FMBase
import FMNet
import Foundation

/// Why a relay was or was not admitted.
public enum RelayProbeStatus: String, Sendable {
    /// `/api/version` answered like a ForkMesh relay. The only status that allows saving it.
    case ok
    /// Nothing URL-shaped could be read out of what the user typed.
    case invalidUrl
    /// The host is already in the relay list.
    case duplicate
    /// DNS did not resolve, or the connection was refused or reset.
    case unreachable
    /// The connection opened but nothing answered in time.
    case timeout
    /// TLS failed: an expired, self-signed, or wrong-host certificate.
    case insecure
    /// Something answered, but it is not a ForkMesh relay.
    case notForkMesh
    /// A ForkMesh-shaped origin that is refusing or erroring right now.
    case serverError
    /// A real ForkMesh relay that does not serve the APIs the app cannot work without.
    case missingCore
}

/// The outcome of the handshake, with a sentence the Add Relay sheet can show as-is.
public struct RelayProbeResult: Equatable, Sendable {
    public let status: RelayProbeStatus
    /// The relay, ready to save. Only set when `status` is `.ok`.
    public let profile: RelayProfile?
    /// The relay's build rev and app version.
    public let rev: String
    public let version: String
    public let statusCode: Int
    /// What the relay said it supports, when it said anything.
    public let features: RelayFeatureSet
    public let knownAs: RelayProfile?

    public init(
        status: RelayProbeStatus, profile: RelayProfile? = nil, rev: String = "", version: String = "",
        statusCode: Int = 0, features: RelayFeatureSet = .undeclared(), knownAs: RelayProfile? = nil
    ) {
        self.knownAs = knownAs
        self.status = status
        self.profile = profile
        self.rev = rev
        self.version = version
        self.statusCode = statusCode
        self.features = features
    }

    /// Optional features this relay does not have, for the "what you will not get here" line,
    /// in `RelayFeature` order.
    public var missingOptional: [RelayFeature] {
        RelayFeature.allCases.filter { !$0.core && !features.has($0) }
    }

    public var ok: Bool { status == .ok }

    /// One sentence naming what failed and what the user can do about it.
    public var message: String {
        switch status {
        case .ok:
            return version.isEmpty ? "Connected. This is a ForkMesh relay." : "Connected to ForkMesh \(version)."
        case .invalidUrl:
            return "That does not look like a host. Enter something like “relay.example.com” or “192.168.1.20:8787”."
        case .duplicate:
            if let knownAs {
                return "That address is \(knownAs.label) (\(knownAs.id)), which is already in your list."
            }
            return "This relay is already in your list."
        case .unreachable:
            return "Could not reach that host. Check the address and your connection."
        case .timeout:
            return "That host did not answer in time. It may be offline, or behind a network this phone cannot see."
        case .insecure:
            return "The secure connection failed. That host’s certificate is not valid for this address."
        case .notForkMesh:
            return "That host answered, but it is not a ForkMesh relay."
        case .serverError:
            return statusCode == 0
                ? "That relay is not answering right now. Try again in a moment."
                : "That relay answered \(statusCode). Try again in a moment."
        case .missingCore:
            let missing = RelayFeature.allCases.filter { features.missingCore.contains($0) }.map(\.label)
            return "That relay is a ForkMesh relay, but it does not serve \(RelayProbeResult.list(missing)), "
                + "which the app cannot work without."
        }
    }

    static func list(_ items: [String]) -> String {
        if items.isEmpty { return "anything" }
        if items.count == 1 { return items[0] }
        return "\(items.dropLast().joined(separator: ", ")) or \(items[items.count - 1])"
    }
}

/// Verifies that a host the user typed is really a ForkMesh relay before it is allowed into the
/// relay list: `GET <origin>/api/version` needs no account and is served by every relay
/// (`{ok, rev, worker, version, ...}` with an `x-forkmesh-worker` header). Refusing a wrong host
/// before it is written is what keeps an arbitrary domain out of every later REST call, socket and
/// sign-in form.
public struct RelayProbe: Sendable {
    let http: HTTPClient
    let timeout: TimeInterval
    let clock: Clock

    public init(http: HTTPClient, timeout: TimeInterval = 8, clock: Clock = SystemClock()) {
        self.http = http
        self.timeout = timeout
        self.clock = clock
    }

    /// Probe `input`, rejecting anything already present in `existing`.
    public func check(_ input: String, existing: [RelayProfile] = [], label: String = "") async -> RelayProbeResult {
        guard let candidate = RelayProfile.tryParse(input, label: label, now: clock.nowMillis) else {
            return RelayProbeResult(status: .invalidUrl)
        }
        if let known = existing.first(where: { $0.id == candidate.id }) {
            let typed = RelayProfile.hostOrigin(input) ?? candidate.id
            return RelayProbeResult(status: .duplicate, knownAs: typed == candidate.id ? nil : known)
        }
        return await verify(candidate, existing: existing)
    }

    static let permanentRedirects: Set<Int> = [301, 308]
    static let temporaryRedirects: Set<Int> = [302, 303, 307]
    static let maxRedirects = 3

    /// Run the handshake against an already-parsed `candidate` (also the "Test connection"
    /// action on a saved relay).
    public func verify(_ candidate: RelayProfile, existing: [RelayProfile] = []) async -> RelayProbeResult {
        await verify(candidate, existing: existing, hops: 0)
    }

    private func send(_ url: URL, follow: Bool) async -> Result<HTTPResponse, RelayProbeFailure> {
        let request = HTTPRequest(
            method: "GET", url: url, headers: ["Accept": "application/json"], timeout: timeout, followRedirects: follow)
        do {
            return .success(try await Deadline.send(http, request, seconds: timeout))
        } catch let error as HTTPError {
            switch error {
            case .timedOut: return .failure(RelayProbeFailure(status: .timeout))
            case .transport(let message) where RelayProbe.looksLikeTLS(message):
                return .failure(RelayProbeFailure(status: .insecure))
            default: return .failure(RelayProbeFailure(status: .unreachable))
            }
        } catch {
            return .failure(RelayProbeFailure(status: .unreachable))
        }
    }

    private func redirectTarget(_ response: HTTPResponse, from url: URL) -> RelayProfile? {
        guard let location = response.header("location"),
            let target = URL(string: location.dartTrimmed, relativeTo: url)?.absoluteURL,
            let scheme = target.scheme?.lowercased(),
            scheme == "https" || (scheme == "http" && url.scheme?.lowercased() == "http")
        else { return nil }
        return RelayProfile.tryParse(target.absoluteString, now: clock.nowMillis)
    }

    private func verify(_ candidate: RelayProfile, existing: [RelayProfile], hops: Int) async -> RelayProbeResult {
        guard let url = URL(string: "\(candidate.httpOrigin)/api/version") else {
            return RelayProbeResult(status: .invalidUrl)
        }
        var response: HTTPResponse
        switch await send(url, follow: false) {
        case .success(let answer): response = answer
        case .failure(let failure): return RelayProbeResult(status: failure.status)
        }
        if RelayProbe.permanentRedirects.contains(response.status), hops < RelayProbe.maxRedirects,
            let target = redirectTarget(response, from: url), target.id != candidate.id
        {
            if let known = existing.first(where: { $0.id == target.id }) {
                return RelayProbeResult(status: .duplicate, statusCode: response.status, knownAs: known)
            }
            let label = candidate.label == candidate.id ? "" : candidate.label
            let moved = RelayProfile.tryParse(
                target.id, label: label, worldUrl: candidate.worldUrl, now: clock.nowMillis) ?? target
            return await verify(moved, existing: existing, hops: hops + 1)
        }
        if RelayProbe.permanentRedirects.union(RelayProbe.temporaryRedirects).contains(response.status) {
            switch await send(url, follow: true) {
            case .success(let answer): response = answer
            case .failure(let failure): return RelayProbeResult(status: failure.status)
            }
        }

        // A non-JSON answer is the common "some other web server lives here" case, reported as
        // such below rather than as a parse failure.
        let body = response.jsonObject
        let headerIdentified = response.header("x-forkmesh-worker") != nil

        if response.status != 200 {
            // A 4xx from a host that still identifies as ForkMesh (a relay behind maintenance) is a
            // transient server problem; anything else at a non-200 is not the relay we wanted.
            return RelayProbeResult(
                status: headerIdentified ? .serverError : .notForkMesh, statusCode: response.status)
        }

        // Two independent markers, so a generic `{"ok":true}` from an unrelated API cannot pass:
        // the Worker's own header, or the rev/worker pair only /api/version produces.
        let identified =
            headerIdentified
            || (body["ok"]?.boolValue == true && body.contains("rev") && body.contains("worker"))
        if !identified {
            return RelayProbeResult(status: .notForkMesh, statusCode: response.status)
        }

        // What this relay says it serves. A relay that predates the field says nothing, and is
        // taken as having everything until a 404 proves otherwise.
        let features = RelayFeatureSet.fromVersionBody(body)
        if !features.hasCore {
            return RelayProbeResult(status: .missingCore, statusCode: response.status, features: features)
        }

        let rev = RelayProbe.dartText(body["rev"])
        return RelayProbeResult(
            status: .ok,
            profile: candidate.copyWith(verifiedAt: clock.nowMillis, verifiedRev: rev, features: features),
            rev: rev,
            version: RelayProbe.dartText(body["version"]),
            statusCode: response.status,
            features: features)
    }

    /// A missing or null value is "", anything else its `text`.
    static func dartText(_ value: JSONValue?) -> String {
        guard let value, !value.isNull else { return "" }
        return value.text
    }

    /// URLSession reports certificate failures as transport errors; their text is what tells a
    /// bad certificate apart from an unreachable host.
    static func looksLikeTLS(_ message: String) -> Bool {
        let lower = message.lowercased()
        return lower.contains("certificate") || lower.contains("ssl") || lower.contains("tls")
            || lower.contains("secure connection") || lower.contains("handshake")
    }
}

struct RelayProbeFailure: Error {
    let status: RelayProbeStatus
}
