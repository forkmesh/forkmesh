package com.forkmesh.nativeapp.runners

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.HttpTransport
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.net.HttpClient
import java.util.concurrent.CopyOnWriteArrayList

data class RunnerReply(val status: Int, val body: JsonMap, val parsed: Boolean) {
    val ok: Boolean get() = status in 200..299 && parsed
}

open class RunnerAttentionApi(
    private val origin: () -> String,
    private val sessionToken: () -> String,
    private val transport: HttpTransport = HttpTransport.DEFAULT,
) {
    companion object {
        fun from(api: ApiService, transport: HttpTransport = HttpTransport.DEFAULT): RunnerAttentionApi =
            RunnerAttentionApi({ api.gitOrigin }, { api.sessionTokenProvider?.invoke() ?: "" }, transport)

        private fun sessionPath(runner: String, id: String): String =
            "/api/runners/${runner.trim().lowercase()}/login/${id.trim()}"
    }

    val hasSession: Boolean get() = sessionToken().isNotEmpty()

    val appOrigin: String get() = origin().trimEnd('/')

    fun dashboardUrl(runner: String, provider: String): String = RunnerAttention.dashboardUrl(appOrigin, runner, provider)

    fun connectUrl(item: RunnerAttentionItem): String = RunnerAttention.connectUrl(appOrigin, item)

    open fun attention(): RunnerReply = request("GET", RunnerAttention.ATTENTION_PATH, null)

    open fun runners(): RunnerReply = request("GET", RunnerAttention.RUNNERS_PATH, null)

    open fun startLogin(runner: String, provider: String): RunnerReply =
        request("POST", "/api/runners/${runner.trim().lowercase()}/login", mapOf("provider" to provider.trim()))

    open fun activateAccount(runner: String, provider: String, account: String): RunnerReply =
        request(
            "POST",
            RunnerAccounts.accountsPath(runner),
            mapOf("provider" to provider.trim(), "account" to account.trim(), "action" to RunnerAccounts.ACTIVATE),
        )

    open fun output(runner: String, id: String, afterSeq: Long): RunnerReply =
        request("GET", sessionPath(runner, id) + "/output?after=${afterSeq.coerceAtLeast(0)}", null)

    open fun input(runner: String, id: String, data: String): RunnerReply =
        request("POST", sessionPath(runner, id) + "/input", mapOf("data" to data))

    open fun cancel(runner: String, id: String): RunnerReply =
        request("POST", sessionPath(runner, id) + "/cancel", emptyMap())

    private fun request(method: String, path: String, body: Map<String, Any?>?): RunnerReply {
        val request = HttpClient.Request(method, appOrigin + path)
        request.header("Accept", "application/json")
        if (body != null) {
            request.header("Content-Type", "application/json")
            request.header("Origin", appOrigin)
            request.body = Json.stringify(body).toByteArray(Charsets.UTF_8)
        }
        request.bearer(sessionToken())
        request.timeouts(15_000, 20_000)
        val response = transport.send(request)
        val parsed = runCatching { Json.parse(String(response.body, Charsets.UTF_8)) }.getOrNull()
        return RunnerReply(response.status, parsed.asJsonMap(), parsed is Map<*, *>)
    }
}

class RunnerAttentionStore(
    private val api: RunnerAttentionApi,
    private val clock: () -> Long = { System.currentTimeMillis() },
) {
    companion object {
        const val FORCED_FLOOR_MS = 5_000L
        const val ENDPOINT_RETRY_MS = 30 * 60_000L
    }

    enum class Source { None, Attention, Runners }

    @Volatile
    var items: List<RunnerAttentionItem> = emptyList()
        private set

    @Volatile
    var source: Source = Source.None
        private set

    @Volatile
    var pollMs: Long = RunnerAttention.DEFAULT_POLL_MS
        private set

    private var lastFetchMs = 0L
    private var attentionMissingUntil = 0L
    private val listeners = CopyOnWriteArrayList<(List<RunnerAttentionItem>) -> Unit>()

    fun addListener(listener: (List<RunnerAttentionItem>) -> Unit) {
        listeners += listener
    }

    fun removeListener(listener: (List<RunnerAttentionItem>) -> Unit) {
        listeners -= listener
    }

    @Synchronized
    fun refresh(force: Boolean = false): Boolean {
        val now = clock()
        val floor = if (force) FORCED_FLOOR_MS else pollMs
        if (lastFetchMs > 0 && now - lastFetchMs < floor) return false
        lastFetchMs = now
        if (!api.hasSession) {
            source = Source.None
            publish(emptyList())
            return true
        }
        val next = try {
            load(now)
        } catch (_: Exception) {
            null
        } ?: return true
        publish(next)
        return true
    }

    fun clear() {
        synchronized(this) {
            lastFetchMs = 0
            source = Source.None
        }
        publish(emptyList())
    }

    private fun load(now: Long): List<RunnerAttentionItem>? {
        if (now >= attentionMissingUntil) {
            val reply = api.attention()
            when {
                reply.ok -> {
                    source = Source.Attention
                    pollMs = RunnerAttention.pollMs(reply.body)
                    return RunnerAttention.parseAttention(reply.body)
                }
                reply.status == 401 || reply.status == 403 -> {
                    source = Source.Attention
                    return emptyList()
                }
                reply.status == 404 || reply.status == 405 || (reply.status in 200..299 && !reply.parsed) ->
                    attentionMissingUntil = now + ENDPOINT_RETRY_MS
                else -> return null
            }
        }
        val reply = api.runners()
        return when {
            reply.ok -> {
                source = Source.Runners
                pollMs = RunnerAttention.DEFAULT_POLL_MS
                RunnerAttention.deriveFromRunners(reply.body)
            }
            reply.status == 401 || reply.status == 403 -> {
                source = Source.Runners
                emptyList()
            }
            else -> null
        }
    }

    private fun publish(next: List<RunnerAttentionItem>) {
        items = next
        for (listener in listeners) listener(next)
    }
}
