package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.bytes.dartTrim
import com.forkmesh.nativeapp.git.bytes.encodeLowerHex
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.repository.GitRemoteAccess
import com.forkmesh.nativeapp.git.security.GitCancellationToken
import com.forkmesh.nativeapp.git.security.GitResourceLimits
import com.forkmesh.nativeapp.git.transport.GitHttpRequest
import com.forkmesh.nativeapp.git.transport.GitHttpResponse
import com.forkmesh.nativeapp.git.transport.GitHttpTransport
import com.forkmesh.nativeapp.git.transport.GitIoHttpTransport
import com.forkmesh.nativeapp.security.DeviceIdentity
import java.net.URI
import java.security.MessageDigest

fun forkMeshRemoteHost(url: String): String = try {
    URI(url.dartTrim()).host ?: ""
} catch (error: Exception) {
    ""
}

const val forkMeshPushCanonicalPrefix = "forkmesh-git-push-v1"

fun forkMeshBodyDigest(body: ByteArray): String = encodeLowerHex(MessageDigest.getInstance("SHA-256").digest(body))

fun forkMeshPushCanonical(owner: String, repo: String, method: String, ts: String, body: ByteArray): String =
    listOf(
        forkMeshPushCanonicalPrefix,
        owner.dartTrim().lowercase(),
        repo.dartTrim(),
        method.dartTrim().uppercase(),
        ts,
        forkMeshBodyDigest(body),
    ).joinToString("\n")

class ForkMeshRemoteCredentials(
    origin: String,
    val owner: String,
    val repo: String,
    signer: DeviceIdentity? = null,
    pubkeyB64url: String = "",
    val endpointUrl: String = "",
) {
    val signer: DeviceIdentity? = if (endpointUrl.isEmpty()) signer else null

    val pubkeyB64url: String = if (endpointUrl.isEmpty()) pubkeyB64url else ""

    val origin: String = normalizeOrigin(origin)

    val canSign: Boolean get() = signer != null && pubkeyB64url.isNotEmpty()

    val remoteUrl: String get() = endpointUrl.ifEmpty { relayUrl }

    val relayUrl: String get() = "${this.origin}/$owner/$repo"

    val host: String get() = forkMeshRemoteHost(endpointUrl.ifEmpty { this.origin })

    fun at(endpoint: String): ForkMeshRemoteCredentials =
        ForkMeshRemoteCredentials(origin = this.origin, owner = owner, repo = repo, endpointUrl = endpoint)

    val anonymous: ForkMeshRemoteCredentials
        get() = ForkMeshRemoteCredentials(origin = this.origin, owner = owner, repo = repo, endpointUrl = endpointUrl)

    companion object {
        fun normalizeOrigin(value: String): String {
            val parsed = try {
                URI(value.dartTrim())
            } catch (error: Exception) {
                return ""
            }
            val host = parsed.host
            if (host.isNullOrEmpty()) return ""
            val source = parsed.scheme?.lowercase() ?: ""
            val scheme = if (source == "ws" || source == "http") "http" else "https"
            val defaultPort = when (source) {
                "http" -> 80
                "https" -> 443
                else -> -1
            }
            val port = if (parsed.port >= 0 && parsed.port != defaultPort) ":${parsed.port}" else ""
            return "$scheme://$host$port"
        }
    }
}

class ForkMeshSigningTransport(
    val credentials: ForkMeshRemoteCredentials,
    inner: GitHttpTransport? = null,
    private val clock: () -> Long = System::currentTimeMillis,
) : GitHttpTransport {
    private val inner: GitHttpTransport = inner ?: GitIoHttpTransport()

    override fun send(
        request: GitHttpRequest,
        cancellation: GitCancellationToken?,
        limits: GitResourceLimits?,
    ): GitHttpResponse = inner.send(signed(request), cancellation, limits)

    override fun close() = inner.close()

    private fun signed(request: GitHttpRequest): GitHttpRequest {
        val signer = credentials.signer
        if (!credentials.canSign || signer == null) return request
        val ts = clock().toString()
        val canonical = forkMeshPushCanonical(
            owner = credentials.owner,
            repo = credentials.repo,
            method = request.method,
            ts = ts,
            body = request.body,
        )
        val signature = signer.sign(canonical.toByteArray(Charsets.UTF_8))
        val headers = LinkedHashMap(request.headers)
        headers["x-forkmesh-pubkey"] = credentials.pubkeyB64url
        headers["x-forkmesh-ts"] = ts
        headers["x-forkmesh-sig"] = signature.trimEnd('=')
        headers["x-forkmesh-body-sha256"] = forkMeshBodyDigest(request.body)
        return GitHttpRequest(
            method = request.method,
            url = request.url,
            headers = headers,
            body = request.body,
            maxResponseBytes = request.maxResponseBytes,
            expectedContentType = request.expectedContentType,
            deadline = request.deadline,
        )
    }
}

fun forkMeshRemoteAccess(
    credentials: ForkMeshRemoteCredentials,
    inner: GitHttpTransport? = null,
    route: GitRemoteRoute? = null,
): GitRemoteAccess {
    val base = inner ?: GitIoHttpTransport(surfaceRedirects = true)
    return GitRemoteAccess(
        transport = ForkMeshRedirectTransport(
            relayBase = credentials.remoteUrl,
            signed = ForkMeshSigningTransport(credentials = credentials, inner = base),
            plain = base,
            route = route ?: GitRemoteRoute(),
        ),
    )
}

@Suppress("EnumEntryName")
enum class ForkMeshRemoteError {
    pushUnauthorized,
    pushTooLarge,
    pushUnavailable,
    nonFastForward,
    notFound,
    network,
    tls,
    cancelled,
    tooLarge,
    protocol,
    other,
}

class ForkMeshRemoteFailure(val error: ForkMeshRemoteError, val message: String) {
    val code: String
        get() = when (error) {
            ForkMeshRemoteError.pushUnauthorized -> "push_unauthorized"
            ForkMeshRemoteError.pushTooLarge -> "push_too_large"
            ForkMeshRemoteError.pushUnavailable -> "push_unavailable"
            ForkMeshRemoteError.nonFastForward -> "non_fast_forward"
            ForkMeshRemoteError.notFound -> "not_found"
            ForkMeshRemoteError.network -> "network_unavailable"
            ForkMeshRemoteError.tls -> "tls_rejected"
            ForkMeshRemoteError.cancelled -> "cancelled"
            ForkMeshRemoteError.tooLarge -> "resource_limit_exceeded"
            ForkMeshRemoteError.protocol -> "protocol_unsupported"
            ForkMeshRemoteError.other -> "remote_rejected"
        }

    override fun toString(): String = message
}

private val httpStatusPattern = Regex("remote returned HTTP (\\d{3})")

fun forkMeshHttpStatus(message: String): Int? = httpStatusPattern.find(message)?.groupValues?.get(1)?.toIntOrNull()

private const val unauthorizedMessage =
    "ForkMesh refused this device (push_unauthorized). Sign in again, or check that this node still has publish access."
private const val pushTooLargeMessage =
    "Push is too large for mobile right now (8 MB limit) — commit fewer/smaller files."

fun describeRemoteFailure(error: Any?, push: Boolean = false): ForkMeshRemoteFailure {
    if (error is ForkMeshRemoteFailure) return error
    if (error !is GitException) return ForkMeshRemoteFailure(ForkMeshRemoteError.other, error.toString())
    return when (error.code) {
        GitErrorCode.authenticationFailed -> ForkMeshRemoteFailure(ForkMeshRemoteError.pushUnauthorized, unauthorizedMessage)
        GitErrorCode.nonFastForward -> ForkMeshRemoteFailure(
            ForkMeshRemoteError.nonFastForward,
            "Someone else pushed first — fetch and retry.",
        )
        GitErrorCode.networkUnavailable -> ForkMeshRemoteFailure(
            ForkMeshRemoteError.network,
            "Could not reach ForkMesh. Check your connection and try again.",
        )
        GitErrorCode.tlsRejected -> ForkMeshRemoteFailure(
            ForkMeshRemoteError.tls,
            "The secure connection to ForkMesh was rejected.",
        )
        GitErrorCode.cancelled -> ForkMeshRemoteFailure(ForkMeshRemoteError.cancelled, "Operation cancelled.")
        GitErrorCode.resourceLimitExceeded -> ForkMeshRemoteFailure(
            ForkMeshRemoteError.tooLarge,
            if (push) pushTooLargeMessage else "This repository is too large to clone on this device.",
        )
        GitErrorCode.protocolUnsupported -> ForkMeshRemoteFailure(
            ForkMeshRemoteError.protocol,
            "ForkMesh answered with something this engine cannot read.",
        )
        GitErrorCode.remoteRejected -> fromStatus(error.message, push)
        else -> ForkMeshRemoteFailure(ForkMeshRemoteError.other, error.message)
    }
}

private fun fromStatus(message: String, push: Boolean): ForkMeshRemoteFailure = when (forkMeshHttpStatus(message)) {
    401, 403 -> ForkMeshRemoteFailure(ForkMeshRemoteError.pushUnauthorized, unauthorizedMessage)
    404 -> ForkMeshRemoteFailure(ForkMeshRemoteError.notFound, "That repository is not available on ForkMesh.")
    413 -> ForkMeshRemoteFailure(ForkMeshRemoteError.pushTooLarge, pushTooLargeMessage)
    503 -> ForkMeshRemoteFailure(
        ForkMeshRemoteError.pushUnavailable,
        if (push) {
            "No ForkMesh node can accept this push right now — try again in a moment."
        } else {
            "This repository is offline on ForkMesh right now."
        },
    )
    else -> ForkMeshRemoteFailure(ForkMeshRemoteError.other, message)
}
