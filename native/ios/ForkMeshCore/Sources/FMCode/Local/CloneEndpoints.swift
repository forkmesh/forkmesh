import FMApi
import FMBase
import FMGit
import Foundation

public struct CloneEndpoint: Equatable, Sendable {
    public enum Kind: Equatable, Sendable {
        case mirror(node: String, healthy: Bool)
        case relay
    }

    public let kind: Kind
    public let url: String

    public init(kind: Kind, url: String) {
        self.kind = kind
        self.url = url
    }

    public var host: String { forkMeshRemoteHost(url) }

    public var isRelay: Bool { kind == .relay }

    public var label: String {
        switch kind {
        case .mirror(let node, _):
            return host.isEmpty ? node : host
        case .relay:
            return "the relay (\(host))"
        }
    }

    public func credentials(_ relay: ForkMeshRemoteCredentials) -> ForkMeshRemoteCredentials {
        isRelay ? relay : relay.at(endpoint: url)
    }
}

public struct CloneAttempt: Equatable, Sendable {
    public let endpoint: CloneEndpoint
    public let message: String

    public init(endpoint: CloneEndpoint, message: String) {
        self.endpoint = endpoint
        self.message = message
    }
}

public struct CloneFailure: Error, Equatable, Sendable, CustomStringConvertible, LocalizedError {
    public let repository: String
    public let attempts: [CloneAttempt]
    public let lookupProblem: String?
    public let action: String

    public init(repository: String, attempts: [CloneAttempt], lookupProblem: String? = nil, action: String = "clone") {
        self.repository = repository
        self.attempts = attempts
        self.lookupProblem = lookupProblem
        self.action = action
    }

    public var message: String {
        let mirrors = attempts.filter { !$0.endpoint.isRelay }
        var lines = [String]()
        if mirrors.isEmpty {
            lines.append(
                lookupProblem.map { "Could not list the mirrors of \(repository) (\($0))." }
                    ?? "No mirror is serving \(repository) right now.")
        } else {
            lines.append("Could not \(action) \(repository) from any of its \(attempts.count) sources.")
        }
        for attempt in attempts {
            lines.append("• \(attempt.endpoint.label): \(attempt.message)")
        }
        return lines.joined(separator: "\n")
    }

    public var description: String { message }
    public var errorDescription: String? { message }
}

public enum CloneEndpoints {
    public static func plan(
        owner: String, name: String, mirrors: [RepoMirror], relay: ForkMeshRemoteCredentials
    ) -> [CloneEndpoint] {
        var seen = Set<String>()
        var candidates = [(mirror: RepoMirror, url: String)]()
        for mirror in mirrors where !mirror.abuseBlocked && (mirror.cloneAvailable || mirror.online) {
            guard let url = mirrorUrl(mirror, owner: owner, name: name) else { continue }
            if seen.insert(url.lowercased()).inserted { candidates.append((mirror, url)) }
        }
        candidates.sort { a, b in
            if a.mirror.isHealthy != b.mirror.isHealthy { return a.mirror.isHealthy }
            if a.mirror.behind != b.mirror.behind { return !a.mirror.behind }
            let la = a.mirror.latencyMs > 0 ? a.mirror.latencyMs : Int.max
            let lb = b.mirror.latencyMs > 0 ? b.mirror.latencyMs : Int.max
            if la != lb { return la < lb }
            if a.mirror.lastSyncMs != b.mirror.lastSyncMs { return a.mirror.lastSyncMs > b.mirror.lastSyncMs }
            return a.url < b.url
        }
        var out = candidates.map {
            CloneEndpoint(kind: .mirror(node: $0.mirror.node.isEmpty ? $0.mirror.owner : $0.mirror.node, healthy: $0.mirror.isHealthy), url: $0.url)
        }
        if !relay.origin.isEmpty, seen.insert(relay.relayUrl.lowercased()).inserted {
            out.append(CloneEndpoint(kind: .relay, url: relay.relayUrl))
        }
        return out
    }

    static func mirrorUrl(_ mirror: RepoMirror, owner: String, name: String) -> String? {
        var url = mirror.mirrorUrl
        if url.isEmpty, !mirror.endpoint.isEmpty {
            var base = mirror.endpoint
            while base.hasSuffix("/") { base.removeLast() }
            url = "\(base)/\(owner)/\(name).git"
        }
        guard !url.isEmpty, (try? GitRemoteUrl.parse(url)) != nil else { return nil }
        return url
    }

    public static func isFinal(_ error: Error) -> Bool {
        if error is CancellationError || error is LocalRepoStoreException { return true }
        if let error = error as? GitWorkspaceException {
            return error.code == .cancelled || error.code == .resourceLimitExceeded
        }
        if let error = error as? GitException {
            return error.code == .cancelled || error.code == .resourceLimitExceeded
        }
        return false
    }

    @MainActor
    public static func run<T: Sendable>(
        repository: String,
        endpoints: [CloneEndpoint],
        lookupProblem: String? = nil,
        action: String = "clone",
        earlier: [CloneAttempt] = [],
        onAttempt: @MainActor (CloneEndpoint, Int) -> Void = { _, _ in },
        _ attempt: @MainActor (CloneEndpoint) async throws -> T
    ) async throws -> T {
        var attempts = earlier
        for (index, endpoint) in endpoints.enumerated() {
            onAttempt(endpoint, index)
            do {
                return try await attempt(endpoint)
            } catch {
                if isFinal(error) { throw error }
                attempts.append(CloneAttempt(endpoint: endpoint, message: LocalRepoScreenModel.message(error)))
            }
        }
        throw CloneFailure(repository: repository, attempts: attempts, lookupProblem: lookupProblem, action: action)
    }
}
