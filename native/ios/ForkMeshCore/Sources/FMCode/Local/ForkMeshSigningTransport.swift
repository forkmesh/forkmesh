import FMBase
import FMCrypto
import FMGit
import FMNet
import Foundation

/// Signs every engine HTTP request with the ForkMesh device headers.
///
/// Reads of a public repository need no signature, but the headers are still
/// attached whenever `credentials.canSign` so a private or org-scoped read
/// authorizes with the same key.
final class ForkMeshSigningTransport: GitHttpTransport, @unchecked Sendable {
    let credentials: ForkMeshRemoteCredentials
    private let inner: GitHttpTransport
    private let clock: @Sendable () -> Date

    init(
        credentials: ForkMeshRemoteCredentials,
        inner: GitHttpTransport? = nil,
        clock: (@Sendable () -> Date)? = nil
    ) {
        self.credentials = credentials
        self.inner = inner ?? GitNetHttpTransport()
        self.clock = clock ?? { Date() }
    }

    func send(
        _ request: GitHttpRequest,
        cancellation: GitCancellationToken?,
        limits: GitResourceLimits?
    ) async throws -> GitHttpResponse {
        let signed = try signedRequest(request)
        return try await inner.send(signed, cancellation: cancellation, limits: limits)
    }

    func close() async {
        await inner.close()
    }

    /// A copy of `request` carrying `X-ForkMesh-Pubkey/Ts/Sig`.
    func signedRequest(_ request: GitHttpRequest) throws -> GitHttpRequest {
        if !credentials.canSign { return request }
        let ts = String(clock().epochMillis)
        let canonical = forkMeshPushCanonical(
            owner: credentials.owner,
            repo: credentials.repo,
            method: request.method,
            ts: ts,
            body: request.body)
        let signature = try Ed25519.sign(seed: credentials.seed, message: Array(canonical.utf8))
        var headers = request.headers
        headers["x-forkmesh-pubkey"] = credentials.pubkeyB64url
        headers["x-forkmesh-ts"] = ts
        headers["x-forkmesh-sig"] = B64.encodeUrlNoPadding(signature)
        // The digest the signature covers. Stating it lets the Worker verify a
        // push without holding the pack and stream it to the hosting node,
        // which checks the bytes against this same digest; without it a pack
        // above the Worker's 8 MB buffer is refused.
        headers["x-forkmesh-body-sha256"] = forkMeshBodyDigest(request.body)
        return try GitHttpRequest(
            method: request.method,
            url: request.url,
            headers: headers,
            body: request.body,
            maxResponseBytes: request.maxResponseBytes,
            expectedContentType: request.expectedContentType,
            deadline: request.deadline,
            onBodyBytes: request.onBodyBytes)
    }
}

/// Engine access for one ForkMesh remote. Built inside the worker task, so
/// the key material is used only there.
func forkMeshRemoteAccess(
    _ credentials: ForkMeshRemoteCredentials,
    inner: GitHttpTransport? = nil,
    route: GitRemoteRoute? = nil
) -> GitRemoteAccess {
    let base = inner ?? GitNetHttpTransport(surfaceRedirects: true)
    return GitRemoteAccess(transport: ForkMeshRedirectTransport(
        relayBase: credentials.remoteUrl,
        signed: ForkMeshSigningTransport(credentials: credentials, inner: base),
        plain: base,
        route: route ?? GitRemoteRoute()))
}
