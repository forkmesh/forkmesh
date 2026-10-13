package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.protocol.GitSmartHttpService
import com.forkmesh.nativeapp.git.security.GitCancellationToken
import com.forkmesh.nativeapp.git.security.GitResourceLimits
import com.forkmesh.nativeapp.git.transport.GitHttpRequest
import com.forkmesh.nativeapp.git.transport.GitHttpResponse
import com.forkmesh.nativeapp.git.transport.GitHttpTransport
import com.forkmesh.nativeapp.git.transport.GitRemoteUrl
import com.forkmesh.nativeapp.git.transport.classifyHttpFailure
import java.net.URI
import java.util.concurrent.atomic.AtomicReference

class GitRemoteRoute {
    private val followed = AtomicReference<String?>(null)

    val mirrorUrl: String? get() = followed.get()

    val mirrorHost: String? get() = mirrorUrl?.let(::forkMeshRemoteHost)

    internal fun follow(base: String): Boolean = followed.compareAndSet(null, base)
}

internal data class GitRedirectTarget(val url: URI, val base: String) {
    companion object {
        private const val suffix = "/info/refs"

        fun resolve(request: GitHttpRequest, location: String?, relayBase: String): GitRedirectTarget? {
            if (request.method != "GET" || !isUploadPackAdvertisement(request.url)) return null
            if (location.isNullOrEmpty()) return null
            val resolved = try {
                request.url.resolve(URI(location))
            } catch (error: Exception) {
                return null
            }
            if (!"https".equals(resolved.scheme, ignoreCase = true)) return null
            if (resolved.host.isNullOrEmpty() || !resolved.rawUserInfo.isNullOrEmpty()) return null
            if (!isUploadPackAdvertisement(resolved)) return null
            val path = resolved.rawPath.dropLast(suffix.length)
            if (path.isEmpty()) return null
            val base = "https://${resolved.rawAuthority}$path"
            if (base == relayBase) return null
            try {
                GitRemoteUrl.parse(base)
            } catch (error: Exception) {
                return null
            }
            return GitRedirectTarget(resolved, base)
        }

        fun isUploadPackAdvertisement(url: URI): Boolean {
            val path = url.rawPath ?: return false
            if (!path.endsWith(suffix)) return false
            val query = url.rawQuery ?: return false
            return query.split("&").any { it == "service=${GitSmartHttpService.uploadPack.name}" }
        }
    }
}

internal class ForkMeshRedirectTransport(
    relayBase: String,
    private val signed: GitHttpTransport,
    private val plain: GitHttpTransport,
    val route: GitRemoteRoute,
) : GitHttpTransport {
    val relayBase: String = relayBase.trimEnd('/')

    override fun send(
        request: GitHttpRequest,
        cancellation: GitCancellationToken?,
        limits: GitResourceLimits?,
    ): GitHttpResponse {
        val mirror = route.mirrorUrl
        if (mirror != null) {
            val moved = rebase(request, mirror)
            if (moved != null) return direct(moved, plain.send(moved, cancellation, limits))
        }
        val response = signed.send(request, cancellation, limits)
        if (response.statusCode !in 300 until 400) return response
        val location = response.headers["location"]
        val target = GitRedirectTarget.resolve(request, location, relayBase)
        if (target == null || !route.follow(target.base)) {
            throw classifyHttpFailure(request, response.statusCode, location = location)
        }
        val followed = GitHttpRequest(
            method = "GET",
            url = target.url,
            headers = request.headers,
            maxResponseBytes = request.maxResponseBytes,
            expectedContentType = request.expectedContentType,
            deadline = request.deadline,
        )
        return direct(followed, plain.send(followed, cancellation, limits))
    }

    override fun close() = signed.close()

    private fun direct(request: GitHttpRequest, response: GitHttpResponse): GitHttpResponse {
        if (response.statusCode in 300 until 400) {
            throw classifyHttpFailure(request, response.statusCode, location = response.headers["location"])
        }
        return response
    }

    private fun rebase(request: GitHttpRequest, mirror: String): GitHttpRequest? {
        val text = request.url.toString()
        if (!text.startsWith("$relayBase/")) return null
        val url = try {
            URI(mirror + text.substring(relayBase.length))
        } catch (error: Exception) {
            return null
        }
        return GitHttpRequest(
            method = request.method,
            url = url,
            headers = request.headers,
            body = request.body,
            maxResponseBytes = request.maxResponseBytes,
            expectedContentType = request.expectedContentType,
            deadline = request.deadline,
        )
    }
}
