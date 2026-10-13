// The ForkMesh git remote's credentials, push-signature canonical string and
// failure taxonomy; the signing transport itself is in
// ForkMeshSigningTransport.swift.
//
// The mobile node authenticates a git request by signing
// `forkmesh-git-push-v1\n{owner}\n{repo}\n{method}\n{ts}\n{sha256hex(body)}`
// with its Ed25519 device key (mobile-node-contract §7). The signing wraps the
// engine's transport because that is where the request's final method, URL and
// body are known.

import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMGit
import Foundation

/// Canonical prefix of the per-request push authorization signature.
public let forkMeshPushCanonicalPrefix = "forkmesh-git-push-v1"

/// Lowercase hex SHA-256 of `body`.
public func forkMeshBodyDigest(_ body: [UInt8]) -> String {
    Hex.encode(FMCrypto.Sha256.hash(body))
}

/// The exact bytes a device key signs to authorize one git request.
///
/// `owner` is lowercased and `method` uppercased to match
/// `_git_push_signature_message` in `app/src/entry.py`; `ts` is unix
/// milliseconds as decimal digits, bounded by the login skew window.
public func forkMeshPushCanonical(owner: String, repo: String, method: String, ts: String, body: [UInt8]) -> String {
    [
        forkMeshPushCanonicalPrefix,
        owner.dartTrimmed.lowercased(),
        repo.dartTrimmed,
        method.dartTrimmed.uppercased(),
        ts,
        forkMeshBodyDigest(body),
    ].joined(separator: "\n")
}

/// Material for one repository's mesh remote.
///
/// `seed` is the raw 32-byte Ed25519 seed backing the device identity, or
/// empty for an anonymous read of a public repository. It crosses into the
/// worker task with the credentials and is used only to sign requests there.
public struct ForkMeshRemoteCredentials: Equatable, Sendable {
    /// `https://host[:port]` of the app origin (the masked proxy).
    public let origin: String
    public let owner: String
    public let repo: String
    public let seed: [UInt8]
    public let pubkeyB64url: String
    public let endpointUrl: String

    public init(
        origin: String, owner: String, repo: String, seed: [UInt8]? = nil, pubkeyB64url: String = "",
        endpointUrl: String = ""
    ) {
        self.origin = ForkMeshRemoteCredentials.normalizeOrigin(origin)
        self.owner = owner
        self.repo = repo
        self.seed = endpointUrl.isEmpty ? (seed ?? []) : []
        self.pubkeyB64url = endpointUrl.isEmpty ? pubkeyB64url : ""
        self.endpointUrl = endpointUrl
    }

    /// True when this node can sign requests (a bound session with a seed).
    public var canSign: Bool { seed.count == 32 && !pubkeyB64url.isEmpty }

    /// `{origin}/{owner}/{repo}` — the smart-HTTP endpoint on the app origin.
    public var remoteUrl: String { endpointUrl.isEmpty ? relayUrl : endpointUrl }

    public var relayUrl: String { "\(origin)/\(owner)/\(repo)" }

    public var host: String { forkMeshRemoteHost(endpointUrl.isEmpty ? origin : endpointUrl) }

    public func at(endpoint url: String) -> ForkMeshRemoteCredentials {
        ForkMeshRemoteCredentials(origin: origin, owner: owner, repo: repo, endpointUrl: url)
    }

    /// Drops the signing material, keeping the same endpoint.
    public var anonymous: ForkMeshRemoteCredentials {
        ForkMeshRemoteCredentials(origin: origin, owner: owner, repo: repo, endpointUrl: endpointUrl)
    }

    /// Accepts the app's stored server URL (which may be a `ws`/`wss` relay
    /// URL) and returns the matching HTTPS origin.
    static func normalizeOrigin(_ value: String) -> String {
        let parsed = ServerURL.parse(value.dartTrimmed)
        if parsed.host.isEmpty { return "" }
        let scheme = parsed.scheme == "ws" || parsed.scheme == "http" ? "http" : "https"
        let port = parsed.port.map { ":\($0)" } ?? ""
        return "\(scheme)://\(parsed.host)\(port)"
    }
}

/// Stable ForkMesh error codes for a failed clone/fetch/push.
public enum ForkMeshRemoteError: String, CaseIterable, Sendable {
    case pushUnauthorized
    case pushTooLarge
    case pushUnavailable
    case pushUnsupported
    case nonFastForward
    case notFound
    case network
    case tls
    case cancelled
    case tooLarge
    case `protocol`
    case other
}

/// A remote failure classified into a stable code plus a sentence for the UI.
public struct ForkMeshRemoteFailure: Error, Equatable, Sendable, CustomStringConvertible {
    public let error: ForkMeshRemoteError
    public let message: String

    public init(_ error: ForkMeshRemoteError, _ message: String) {
        self.error = error
        self.message = message
    }

    /// The wire-style code used in logs and tests (`push_too_large`, ...).
    public var code: String {
        switch error {
        case .pushUnauthorized: return "push_unauthorized"
        case .pushTooLarge: return "push_too_large"
        case .pushUnavailable: return "push_unavailable"
        case .pushUnsupported: return "push_unsupported"
        case .nonFastForward: return "non_fast_forward"
        case .notFound: return "not_found"
        case .network: return "network_unavailable"
        case .tls: return "tls_rejected"
        case .cancelled: return "cancelled"
        case .tooLarge: return "resource_limit_exceeded"
        case .protocol: return "protocol_unsupported"
        case .other: return "remote_rejected"
        }
    }

    public var description: String { message }
}

/// The Worker's HTTP status, recovered from the engine's `remoteRejected`
/// message. `classifyHttpFailure` in the engine renders exactly
/// `remote returned HTTP <status>`, which is the only place a status survives
/// the typed error taxonomy.
public func forkMeshHttpStatus(_ message: String) -> Int? {
    let marker = Array("remote returned HTTP ".utf8)
    let bytes = Array(message.utf8)
    guard bytes.count >= marker.count + 3 else { return nil }
    var index = 0
    while index + marker.count + 3 <= bytes.count {
        if Array(bytes[index..<(index + marker.count)]) == marker {
            let digits = bytes[(index + marker.count)..<(index + marker.count + 3)]
            if digits.allSatisfy({ (0x30...0x39).contains($0) }) {
                return Int(String(decoding: digits, as: UTF8.self))
            }
        }
        index += 1
    }
    return nil
}

/// The `host[:port]` a message may name, or "" when `origin` is unusable.
///
/// The app can be pointed at any relay, so a refusal that is a property of
/// the *relay* rather than of the repository has to say which one refused.
public func forkMeshRemoteHost(_ origin: String) -> String {
    let parsed = ServerURL.parse(origin.dartTrimmed)
    if parsed.host.isEmpty { return "" }
    if let port = parsed.port { return "\(parsed.host):\(port)" }
    return parsed.host
}

private func named(_ host: String) -> String { host.isEmpty ? "" : " (\(host))" }

private let unauthorizedMessage =
    "ForkMesh refused this device (push_unauthorized). Sign in again, "
    + "or check that this node still has publish access."

private let pushTooLargeMessage =
    "Push is too large for mobile right now (8 MB limit) — commit fewer/smaller files."

/// Map an engine failure onto a ForkMesh code and a user-facing sentence.
///
/// `host` is the relay the request went to, from `forkMeshRemoteHost`; it is
/// named in the refusals a different relay would not have produced.
public func describeRemoteFailure(_ error: Error, push: Bool = false, host: String = "") -> ForkMeshRemoteFailure {
    if let failure = error as? ForkMeshRemoteFailure { return failure }
    guard let error = error as? GitException else {
        return ForkMeshRemoteFailure(.other, dartErrorText(error))
    }
    switch error.code {
    case .authenticationFailed:
        return ForkMeshRemoteFailure(.pushUnauthorized, unauthorizedMessage)
    case .nonFastForward:
        return ForkMeshRemoteFailure(.nonFastForward, "Someone else pushed first — fetch and retry.")
    case .networkUnavailable:
        return ForkMeshRemoteFailure(.network, "Could not reach ForkMesh. Check your connection and try again.")
    case .tlsRejected:
        return ForkMeshRemoteFailure(.tls, "The secure connection to ForkMesh was rejected.")
    case .cancelled:
        return ForkMeshRemoteFailure(.cancelled, "Operation cancelled.")
    case .resourceLimitExceeded:
        return ForkMeshRemoteFailure(
            .tooLarge,
            push ? pushTooLargeMessage : "This repository is too large to clone on this device.")
    case .protocolUnsupported:
        return ForkMeshRemoteFailure(.protocol, "ForkMesh answered with something this engine cannot read.")
    case .remoteRejected:
        return failureFromStatus(error.message, push: push, host: host)
    default:
        return ForkMeshRemoteFailure(.other, error.message)
    }
}

private func failureFromStatus(_ message: String, push: Bool, host: String) -> ForkMeshRemoteFailure {
    switch forkMeshHttpStatus(message) {
    case 401, 403:
        return ForkMeshRemoteFailure(.pushUnauthorized, unauthorizedMessage)
    case 404:
        return ForkMeshRemoteFailure(.notFound, "That repository is not available on ForkMesh.")
    case 413:
        return ForkMeshRemoteFailure(.pushTooLarge, pushTooLargeMessage)
    case 501:
        // A relay that predates the direct-HTTPS receive-pack route answers
        // 501 `direct_https_receive_pack_required` before it reads the pack.
        // Nothing about the repository, the commit, or this device can make
        // that push land, so say it is the relay and stop the user retrying.
        return ForkMeshRemoteFailure(
            .pushUnsupported,
            push
                ? "This relay\(named(host)) does not accept pushes from mobile. "
                    + "Switch to a relay that does in Settings → Relay, then push again."
                : "This relay\(named(host)) does not support this operation.")
    case 503:
        return ForkMeshRemoteFailure(
            .pushUnavailable,
            push
                ? "No ForkMesh node on this relay\(named(host)) can accept this "
                    + "push right now — a node that hosts repositories has to be "
                    + "online for one to land. Wait for one, then try again."
                : "Nothing on this relay\(named(host)) is serving this repository right now.")
    default:
        return ForkMeshRemoteFailure(.other, message)
    }
}
