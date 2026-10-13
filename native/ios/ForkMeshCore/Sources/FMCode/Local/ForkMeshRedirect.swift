import FMGit
import Foundation

public final class GitRemoteRoute: @unchecked Sendable {
    private let lock = NSLock()
    private var followed: String?

    public init() {}

    public var mirrorUrl: String? {
        lock.lock()
        defer { lock.unlock() }
        return followed
    }

    public var mirrorHost: String? { mirrorUrl.map(forkMeshRemoteHost) }

    func follow(_ base: String) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        if followed != nil { return false }
        followed = base
        return true
    }
}

struct GitRedirectTarget: Equatable {
    let url: URL
    let base: String

    static func resolve(_ request: GitHttpRequest, location: String?, relayBase: String) -> GitRedirectTarget? {
        guard request.method == "GET", isUploadPackAdvertisement(request.url),
              let location, !location.isEmpty,
              let resolved = URL(string: location, relativeTo: request.url)?.absoluteURL,
              resolved.scheme?.lowercased() == "https",
              let host = resolved.host, !host.isEmpty,
              (resolved.user ?? "").isEmpty, (resolved.password ?? "").isEmpty,
              isUploadPackAdvertisement(resolved),
              var components = URLComponents(url: resolved, resolvingAgainstBaseURL: false)
        else { return nil }
        let suffix = "/info/refs"
        components.path = String(components.path.dropLast(suffix.count))
        components.query = nil
        components.fragment = nil
        guard !components.path.isEmpty, let base = components.string, base != relayBase,
              (try? GitRemoteUrl.parse(base)) != nil
        else { return nil }
        return GitRedirectTarget(url: resolved, base: base)
    }

    static func isUploadPackAdvertisement(_ url: URL) -> Bool {
        guard url.path.hasSuffix("/info/refs"),
              let items = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems
        else { return false }
        return items.contains { $0.name == "service" && $0.value == GitSmartHttpService.uploadPack.name }
    }
}

final class ForkMeshRedirectTransport: GitHttpTransport, @unchecked Sendable {
    let relayBase: String
    let route: GitRemoteRoute
    private let signed: GitHttpTransport
    private let plain: GitHttpTransport

    init(relayBase: String, signed: GitHttpTransport, plain: GitHttpTransport, route: GitRemoteRoute) {
        var base = relayBase
        while base.hasSuffix("/") { base.removeLast() }
        self.relayBase = base
        self.signed = signed
        self.plain = plain
        self.route = route
    }

    func send(
        _ request: GitHttpRequest,
        cancellation: GitCancellationToken?,
        limits: GitResourceLimits?
    ) async throws -> GitHttpResponse {
        if let mirror = route.mirrorUrl, let moved = try rebase(request, onto: mirror) {
            return try direct(moved, try await plain.send(moved, cancellation: cancellation, limits: limits))
        }
        let response = try await signed.send(request, cancellation: cancellation, limits: limits)
        guard response.statusCode >= 300 && response.statusCode < 400 else { return response }
        let location = response.headers["location"]
        guard let target = GitRedirectTarget.resolve(request, location: location, relayBase: relayBase),
              route.follow(target.base)
        else {
            throw classifyHttpFailure(request, statusCode: response.statusCode, location: location)
        }
        let followed = try GitHttpRequest(
            method: "GET",
            url: target.url,
            headers: request.headers,
            maxResponseBytes: request.maxResponseBytes,
            expectedContentType: request.expectedContentType,
            deadline: request.deadline,
            onBodyBytes: request.onBodyBytes)
        return try direct(followed, try await plain.send(followed, cancellation: cancellation, limits: limits))
    }

    func close() async {
        await signed.close()
    }

    private func direct(_ request: GitHttpRequest, _ response: GitHttpResponse) throws -> GitHttpResponse {
        if response.statusCode >= 300 && response.statusCode < 400 {
            throw classifyHttpFailure(request, statusCode: response.statusCode, location: response.headers["location"])
        }
        return response
    }

    private func rebase(_ request: GitHttpRequest, onto mirror: String) throws -> GitHttpRequest? {
        let text = request.url.absoluteString
        guard text.hasPrefix(relayBase + "/"), let url = URL(string: mirror + text.dropFirst(relayBase.count)) else {
            return nil
        }
        return try GitHttpRequest(
            method: request.method,
            url: url,
            headers: request.headers,
            body: request.body,
            maxResponseBytes: request.maxResponseBytes,
            expectedContentType: request.expectedContentType,
            deadline: request.deadline,
            onBodyBytes: request.onBodyBytes)
    }
}
