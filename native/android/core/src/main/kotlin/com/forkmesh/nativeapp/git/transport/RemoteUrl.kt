package com.forkmesh.nativeapp.git.transport

import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import java.net.URI
import java.net.URISyntaxException

class GitRemoteUrl private constructor(val uri: URI) {
    val redactedOrigin: String
        get() = if (uri.port == -1) "${uri.scheme}://${uri.host}" else "${uri.scheme}://${uri.host}:${uri.port}"

    override fun equals(other: Any?): Boolean = other is GitRemoteUrl && other.uri == uri

    override fun hashCode(): Int = uri.hashCode()

    override fun toString(): String = uri.toString()

    companion object {
        fun parse(value: String): GitRemoteUrl {
            val parsed = try {
                URI(value)
            } catch (error: URISyntaxException) {
                null
            }
            if (parsed == null ||
                !"https".equals(parsed.scheme, ignoreCase = true) ||
                parsed.host.isNullOrEmpty() ||
                !parsed.rawUserInfo.isNullOrEmpty() ||
                !parsed.rawFragment.isNullOrEmpty() ||
                !parsed.rawQuery.isNullOrEmpty()
            ) {
                throw GitException(
                    GitErrorCode.protocolUnsupported,
                    "remote URL must be a credential-free HTTPS URL",
                )
            }
            val text = StringBuilder("https://").append(parsed.host.lowercase())
            if (parsed.port != -1 && parsed.port != 443) text.append(':').append(parsed.port)
            text.append(parsed.normalize().rawPath ?: "")
            return GitRemoteUrl(URI(text.toString()))
        }
    }
}
