package com.forkmesh.nativeapp.insights

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.model.OrgApiException
import java.net.URLEncoder

fun interface InsightsRequesting {
    fun call(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?>
}

class ApiInsightsTransport(private val api: ApiService) : InsightsRequesting {
    override fun call(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> =
        api.accountApiCall(method, path, body)
}

class InsightsException(val code: String, message: String = "") : RuntimeException(message.ifEmpty { describe(code) }) {
    override val message: String get() = super.message ?: describe(code)

    val needsAdmin: Boolean get() = code in ADMIN_CODES

    companion object {
        private val ADMIN_CODES = setOf("admin_required", "unauthorized", "not_admin", "http_401", "http_403")

        fun describe(code: String): String = when (code) {
            in ADMIN_CODES -> "This needs an administrator account."
            "http_404", "not_found" -> "This server does not offer that yet."
            "self_quarantine_forbidden" -> "You cannot quarantine your own account."
            "self_delete_forbidden" -> "You cannot delete your own account."
            "target_is_admin" -> "Administrators cannot be quarantined or deleted here."
            "no_such_user", "no_such_quarantined_user" -> "That account no longer exists."
            "restricted" -> "This table is only readable through its own page."
            "unknown_table" -> "That table no longer exists."
            else -> code.replace('_', ' ')
        }

        fun from(error: Throwable): InsightsException = when (error) {
            is InsightsException -> error
            is OrgApiException -> InsightsException(error.code)
            else -> InsightsException("request_failed", error.message ?: error.toString())
        }
    }
}

object InsightsQuery {
    fun encode(value: String): String = URLEncoder.encode(value, "UTF-8").replace("+", "%20").replace("*", "%2A").replace("%7E", "~")

    fun path(base: String, items: List<Pair<String, String>>): String {
        val pairs = items.filter { it.second.isNotEmpty() }.map { "${encode(it.first)}=${encode(it.second)}" }
        return if (pairs.isEmpty()) base else base + "?" + pairs.joinToString("&")
    }
}

sealed class InsightsLoad<out T> {
    data object Idle : InsightsLoad<Nothing>()
    data object Loading : InsightsLoad<Nothing>()
    data class Failed(val error: InsightsException) : InsightsLoad<Nothing>()
    data class Loaded<T>(val value: T) : InsightsLoad<T>()

    val valueOrNull: T? get() = (this as? Loaded<T>)?.value

    val errorOrNull: InsightsException? get() = (this as? Failed)?.error

    val isLoading: Boolean get() = this == Loading

    companion object {
        fun <T> attempt(current: InsightsLoad<T>, fetch: () -> T): InsightsLoad<T> = try {
            Loaded(fetch())
        } catch (error: Exception) {
            val value = current.valueOrNull
            if (value != null) Loaded(value) else Failed(InsightsException.from(error))
        }
    }
}
