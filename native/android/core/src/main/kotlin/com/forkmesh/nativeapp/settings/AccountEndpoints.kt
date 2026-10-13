package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.format.relativeTimeFrom
import com.forkmesh.nativeapp.model.OrgApiException

class AccountEndpointException(val code: String, val status: Int = 0) : Exception("AccountEndpointError($code, $status)")

interface AccountEndpointCalling {
    fun accountCall(method: String, path: String, body: Map<String, Any?>? = null): Map<String, Any?>
    val serverOrigin: String
}

class ApiAccountEndpoints(private val api: ApiService) : AccountEndpointCalling {
    override val serverOrigin: String get() = api.gitOrigin

    override fun accountCall(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
        if (method !in setOf("GET", "POST", "DELETE", "PUT", "PATCH") || !path.startsWith("/api/")) {
            throw AccountEndpointException("unsupported_request")
        }
        return try {
            api.accountApiCall(method, path, body)
        } catch (failure: OrgApiException) {
            val status = if (failure.code.startsWith("http_")) failure.code.removePrefix("http_").toIntOrNull() ?: 0 else 0
            throw AccountEndpointException(failure.code, status)
        } catch (failure: AccountEndpointException) {
            throw failure
        } catch (_: Exception) {
            throw AccountEndpointException("offline")
        }
    }
}

object AccountErrorText {
    fun text(error: Throwable, fallback: String): String {
        val failure = error as? AccountEndpointException ?: return fallback
        return when (failure.code) {
            "invalid_session", "unauthorized" -> "Sign in again to continue."
            "forbidden" -> "Your role does not allow that."
            "not_found" -> "That was not found. It may have been removed."
            "origin_not_allowed" -> "The server refused this app's origin. Check the server address in Settings."
            "ssh_key_already_registered" -> "That key is already registered. Make a fresh key pair for this account."
            "ssh_key_limit_reached" -> "This account has the most SSH keys allowed. Remove one first."
            "unsupported_key_type" -> "That key type is not supported. Use ssh-ed25519, ECDSA or RSA of 3072 bits or more."
            "invalid_public_key", "invalid_key_encoding", "invalid_key_blob", "one_public_key_required" ->
                "That is not a single OpenSSH public key line."
            "too_many_bot_tokens" -> "This organization has the most active tokens allowed. Revoke one first."
            "invalid_bot_permissions" -> "Those permissions are not allowed."
            "grant_used" -> "That link was already used. Ask the desktop for a new one."
            "no_such_node" -> "The relay does not know that node yet."
            "no_such_user", "not_a_user" -> "Only a user account can own nodes."
            "bad_signature", "unauthorized_grant" -> "The desktop's signature did not verify."
            "invalid_report" -> "Fill in a title, a description and a component."
            "http_0", "offline" -> fallback
            else -> {
                if (failure.code.startsWith("http_")) return fallback
                val words = failure.code.replace('_', ' ')
                words.take(1).uppercase() + words.drop(1) + "."
            }
        }
    }
}

object AccountTime {
    fun label(millis: Long, nowMillis: Long): String = if (millis > 0) relativeTimeFrom(millis, nowMillis) else "never"
}

internal fun Map<String, Any?>.text(key: String, fallback: String = ""): String = when (val value = this[key]) {
    is String -> value
    null -> fallback
    is Number, is Boolean -> value.toString()
    else -> fallback
}

internal fun Map<String, Any?>.long(key: String): Long = when (val value = this[key]) {
    is Number -> value.toLong()
    is String -> value.toLongOrNull() ?: 0L
    else -> 0L
}

internal fun Map<String, Any?>.int(key: String, fallback: Int = 0): Int = when (val value = this[key]) {
    is Number -> value.toInt()
    is String -> value.toIntOrNull() ?: fallback
    else -> fallback
}

internal fun Map<String, Any?>.flag(key: String): Boolean = this[key] == true

@Suppress("UNCHECKED_CAST")
internal fun Map<String, Any?>.maps(key: String): List<Map<String, Any?>> =
    (this[key] as? List<*>).orEmpty().mapNotNull { it as? Map<String, Any?> }

internal fun Map<String, Any?>.strings(key: String): List<String> = (this[key] as? List<*>).orEmpty().mapNotNull { it as? String }

internal fun Map<String, Any?>.list(key: String): List<Any?> = (this[key] as? List<*>).orEmpty()

@Suppress("UNCHECKED_CAST")
internal fun Map<String, Any?>.obj(key: String): Map<String, Any?> = this[key] as? Map<String, Any?> ?: emptyMap()
