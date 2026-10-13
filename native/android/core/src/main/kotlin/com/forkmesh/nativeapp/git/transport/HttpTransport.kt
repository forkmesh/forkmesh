package com.forkmesh.nativeapp.git.transport

import com.forkmesh.nativeapp.git.bytes.utf8Encode
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.security.GitCancellationToken
import com.forkmesh.nativeapp.git.security.GitResourceLimits
import java.net.URI
import java.net.URISyntaxException
import java.util.Base64

fun interface GitHttpHeaderProvider {
    fun headers(method: String, url: URI, body: ByteArray): Map<String, String>
}

object GitHttpHeaders {
    private val headerValue = Regex("[\\x20-\\x7e]+")

    fun static(headers: Map<String, String>): GitHttpHeaderProvider {
        val copy: Map<String, String> = LinkedHashMap(headers)
        return GitHttpHeaderProvider { _, _, _ -> copy }
    }

    fun bearer(token: String): GitHttpHeaderProvider {
        if (token.isEmpty() || !headerValue.matches(token)) {
            throw GitException(GitErrorCode.authenticationFailed, "bearer token is not a valid header value")
        }
        return static(mapOf("authorization" to "Bearer $token"))
    }

    fun basic(username: String, password: String): GitHttpHeaderProvider {
        if (username.contains(":") || username.contains("\n") || password.contains("\n")) {
            throw GitException(GitErrorCode.authenticationFailed, "basic credentials contain invalid characters")
        }
        val encoded = Base64.getEncoder().encodeToString(utf8Encode("$username:$password"))
        return static(mapOf("authorization" to "Basic $encoded"))
    }
}

class GitCertificateChallenge(
    val host: String,
    val port: Int,
    certificateDer: ByteArray,
    val subject: String,
    val issuer: String,
    val sha1Fingerprint: String,
) {
    val certificateDer: ByteArray = certificateDer.copyOf()

    val defaultValidationPassed: Boolean = false
}

@Suppress("EnumEntryName")
enum class GitCertificateDecision { accept, reject }

fun interface GitCertificateChallengeHandler {
    fun decide(challenge: GitCertificateChallenge): GitCertificateDecision
}

class GitHttpRequest(
    val method: String,
    val url: URI,
    headers: Map<String, String> = emptyMap(),
    body: ByteArray = ByteArray(0),
    val maxResponseBytes: Int,
    val expectedContentType: String,
    val deadline: Long? = null,
) {
    val headers: Map<String, String>

    val body: ByteArray = body.copyOf()

    init {
        val lowered = LinkedHashMap<String, String>()
        for ((key, value) in headers) lowered[key.lowercase()] = value
        this.headers = lowered
        if (method != "GET" && method != "POST") {
            throw IllegalArgumentException("Invalid argument (method): $method")
        }
        if (!"https".equals(url.scheme, ignoreCase = true) || !url.rawUserInfo.isNullOrEmpty()) {
            throw GitException(GitErrorCode.protocolUnsupported, "transport accepts only credential-free HTTPS URLs")
        }
        if (maxResponseBytes <= 0) {
            throw IllegalArgumentException("Invalid argument (maxResponseBytes): $maxResponseBytes")
        }
    }
}

class GitHttpResponse(val statusCode: Int, headers: Map<String, String>, body: ByteArray) {
    val headers: Map<String, String>

    val body: ByteArray = body.copyOf()

    init {
        val lowered = LinkedHashMap<String, String>()
        for ((key, value) in headers) lowered[key.lowercase()] = value
        this.headers = lowered
    }

    val contentType: String? get() = headers["content-type"]
}

interface GitHttpTransport {
    fun send(
        request: GitHttpRequest,
        cancellation: GitCancellationToken? = null,
        limits: GitResourceLimits? = null,
    ): GitHttpResponse

    fun close()
}

fun classifyHttpFailure(request: GitHttpRequest, statusCode: Int, location: String? = null): GitException {
    if (statusCode in 300 until 400) {
        val destination = if (location == null) {
            null
        } else {
            try {
                URI(location)
            } catch (error: URISyntaxException) {
                null
            }
        }
        val destinationHost = if (destination == null || destination.host.isNullOrEmpty()) {
            "relative or unknown host"
        } else if (destination.host.equals(request.url.host, ignoreCase = true)) {
            "same host"
        } else {
            "different host"
        }
        return GitException(
            GitErrorCode.remoteRejected,
            "remote redirected ($statusCode) to $destinationHost; redirects are never followed",
        )
    }
    if (statusCode == 401 || statusCode == 403 || statusCode == 407) {
        return GitException(GitErrorCode.authenticationFailed, "remote refused credentials ($statusCode)")
    }
    return GitException(GitErrorCode.remoteRejected, "remote returned HTTP $statusCode")
}
