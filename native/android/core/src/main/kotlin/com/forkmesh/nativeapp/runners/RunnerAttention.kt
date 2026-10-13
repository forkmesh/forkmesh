package com.forkmesh.nativeapp.runners

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.firstStr
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str

enum class RunnerAttentionState { Expired, Expiring }

data class RunnerAttentionItem(
    val runner: String,
    val provider: String,
    val state: RunnerAttentionState,
    val message: String = "",
    val expiresAtMs: Long = 0,
    val openLoginId: String = "",
    val canConnect: Boolean = true,
    val connectUrl: String = "",
) {
    val expired: Boolean get() = state == RunnerAttentionState.Expired
    val key: String get() = "$runner:$provider"
}

data class RunnerSignInTarget(val runner: String, val provider: String, val openLoginId: String = "")

object RunnerAttention {
    const val ATTENTION_PATH = "/api/runners/attention"
    const val RUNNERS_PATH = "/api/runners"
    const val DASHBOARD_PATH = "/dashboard/runners"
    const val DEFAULT_POLL_MS = 300_000L
    const val MIN_POLL_MS = 5_000L
    const val MAX_POLL_MS = 3_600_000L
    val PROVIDERS = listOf("claude-code", "codex")

    private val RUNNER_NAME = Regex("^[a-z](?:[a-z0-9-]{0,61}[a-z0-9])?$")
    private val LOGIN_ID = Regex("^[0-9a-f]{32}$")
    private val OPEN_LOGIN = setOf("pending", "running")
    private val CLEAR_STATES = setOf("", "ok", "available")
    private const val MESSAGE_MAX = 240

    fun validRunner(name: String): Boolean = RUNNER_NAME.matches(name)

    fun validProvider(provider: String): Boolean = provider in PROVIDERS

    fun validLoginId(id: String): Boolean = LOGIN_ID.matches(id)

    fun providerShort(provider: String): String = if (provider == "codex") "Codex" else "Claude"

    fun providerLong(provider: String): String = if (provider == "codex") "Codex" else "Claude Code"

    fun connectLabel(provider: String): String = "Connect ${providerShort(provider)}"

    fun parseAttention(payload: JsonMap): List<RunnerAttentionItem> {
        val key = listOf("attention", "items", "runners").firstOrNull { payload[it] is List<*> } ?: return emptyList()
        val found = ArrayList<RunnerAttentionItem>()
        for (row in payload.maps(key)) {
            if (row["availability"] is Map<*, *>) {
                found += deriveRunner(row)
                continue
            }
            val runner = row.firstStr("runner", "name").trim().lowercase()
            when (val nested = row["providers"]) {
                is List<*> -> for (entry in nested) {
                    if (entry is Map<*, *>) flatItem(entry.asJsonMap(), runner)?.let(found::add)
                }
                is Map<*, *> -> for ((provider, entry) in nested) {
                    val fields = LinkedHashMap(entry.asJsonMap())
                    if (jsonText(fields["provider"]).isEmpty()) fields["provider"] = jsonText(provider)
                    if (entry is String) fields["state"] = entry
                    flatItem(fields, runner)?.let(found::add)
                }
                else -> flatItem(row, runner)?.let(found::add)
            }
        }
        return normalize(found)
    }

    fun deriveFromRunners(payload: JsonMap): List<RunnerAttentionItem> =
        normalize(payload.maps("runners").flatMap(::deriveRunner))

    fun deriveRunner(row: JsonMap): List<RunnerAttentionItem> {
        if (row.str("status") != "active") return emptyList()
        val runner = row.firstStr("name", "runner").trim().lowercase()
        val availability = row.obj("availability")
        val out = ArrayList<RunnerAttentionItem>()
        for (provider in PROVIDERS) {
            val raw = availability[provider] as? Map<*, *> ?: continue
            val entry = raw.asJsonMap()
            if (entry["binaryFound"] != true) continue
            val loginState = entry.str("loginState")
            val auth = entry.obj("auth")
            val authState = if (loginState == "expired") "expired" else auth.str("state")
            val state = when {
                authState == "expired" -> RunnerAttentionState.Expired
                loginState == "available" && authState == "expiring" -> RunnerAttentionState.Expiring
                else -> null
            } ?: continue
            out += RunnerAttentionItem(
                runner = runner,
                provider = provider,
                state = state,
                message = auth.str("message"),
                expiresAtMs = jsonLong(auth["expiresAt"]),
                openLoginId = openLogin(row, provider),
            )
        }
        return out
    }

    private fun openLogin(row: JsonMap, provider: String): String =
        row.maps("logins").firstOrNull {
            it.str("provider") == provider && it.str("state") in OPEN_LOGIN && validLoginId(it.str("id"))
        }?.str("id") ?: ""

    private fun flatItem(row: JsonMap, fallbackRunner: String): RunnerAttentionItem? {
        val runner = row.firstStr("runner", "name").trim().lowercase().ifEmpty { fallbackRunner }
        val provider = row.str("provider").trim().lowercase()
        val auth = row.obj("auth")
        val raw = row.str("state").ifEmpty { row.str("loginState") }.ifEmpty { auth.str("state") }.trim().lowercase()
        val state = stateOf(raw) ?: return null
        val loginId = row.firstStr("loginId", "openLoginId").ifEmpty { row.obj("login").str("id") }.trim()
        return RunnerAttentionItem(
            runner = runner,
            provider = provider,
            state = state,
            message = row.str("message").ifEmpty { auth.str("message") },
            expiresAtMs = jsonLong(row["expiresAt"]).takeIf { it > 0 } ?: jsonLong(auth["expiresAt"]),
            openLoginId = loginId,
            canConnect = row["canConnect"] != false,
            connectUrl = row.str("connectUrl").trim(),
        )
    }

    fun pollMs(payload: JsonMap): Long {
        val value = jsonLong(payload["pollMs"])
        return if (value <= 0) DEFAULT_POLL_MS else value.coerceIn(MIN_POLL_MS, MAX_POLL_MS)
    }

    fun validConnectPath(path: String): Boolean {
        if (!path.startsWith(DASHBOARD_PATH)) return false
        val rest = path.substring(DASHBOARD_PATH.length)
        if (rest.isNotEmpty() && rest[0] != '?' && rest[0] != '#') return false
        return path.none { it.isWhitespace() || Character.isISOControl(it) || it == '\\' }
    }

    fun stateOf(raw: String): RunnerAttentionState? {
        val value = raw.trim().lowercase()
        if (value in CLEAR_STATES) return null
        return if (value == "expiring") RunnerAttentionState.Expiring else RunnerAttentionState.Expired
    }

    fun normalize(items: List<RunnerAttentionItem>): List<RunnerAttentionItem> {
        val byKey = LinkedHashMap<String, RunnerAttentionItem>()
        for (item in items) {
            if (!validRunner(item.runner) || !validProvider(item.provider)) continue
            val clean = item.copy(
                message = cleanMessage(item.message),
                expiresAtMs = item.expiresAtMs.coerceAtLeast(0),
                openLoginId = item.openLoginId.takeIf(::validLoginId) ?: "",
                connectUrl = item.connectUrl.takeIf(::validConnectPath) ?: "",
            )
            val existing = byKey[clean.key]
            byKey[clean.key] = when {
                existing == null -> clean
                clean.expired && !existing.expired -> clean.copy(openLoginId = clean.openLoginId.ifEmpty { existing.openLoginId })
                else -> existing.copy(openLoginId = existing.openLoginId.ifEmpty { clean.openLoginId })
            }
        }
        return byKey.values.sortedWith(
            compareBy<RunnerAttentionItem>({ if (it.expired) 0 else 1 }, { it.runner }, { PROVIDERS.indexOf(it.provider) }),
        )
    }

    private fun cleanMessage(value: String): String =
        value.filter { !Character.isISOControl(it) }.trim().take(MESSAGE_MAX)

    fun title(item: RunnerAttentionItem): String =
        if (item.expired) "${item.runner} needs ${providerShort(item.provider)} sign-in"
        else "${item.runner}: ${providerShort(item.provider)} sign-in expires soon"

    fun detail(item: RunnerAttentionItem, expiryText: String = ""): String =
        if (item.expired) "Jobs for ${providerLong(item.provider)} are not running on this runner."
        else if (expiryText.isNotEmpty()) "Sign in again before it expires ($expiryText)."
        else "Sign in again before it expires."

    fun buttonLabel(item: RunnerAttentionItem): String = if (item.openLoginId.isNotEmpty()) "Open sign-in" else "Sign in"

    fun summaryTitle(items: List<RunnerAttentionItem>): String = when (items.size) {
        0 -> ""
        1 -> title(items[0])
        else -> "${items.size} runner sign-ins needed"
    }

    fun anyExpired(items: List<RunnerAttentionItem>): Boolean = items.any { it.expired }

    fun isRunnerPing(kind: String, href: String): Boolean =
        kind == "operational_alert" && href.contains(DASHBOARD_PATH)

    fun pingTarget(href: String): RunnerSignInTarget? {
        val trimmed = href.trim()
        val start = trimmed.indexOf(DASHBOARD_PATH)
        if (start < 0) return null
        val prefix = trimmed.substring(0, start)
        if (prefix.isNotEmpty() && !Regex("^https://[A-Za-z0-9.-]+(?::\\d+)?$").matches(prefix)) return null
        val rest = trimmed.substring(start + DASHBOARD_PATH.length)
        if (rest.isNotEmpty() && rest[0] != '?' && rest[0] != '#' && rest != "/" && !rest.startsWith("/?")) return null
        val query = rest.substringBefore('#').substringAfter('?', "")
        val params = HashMap<String, String>()
        for (pair in query.split('&')) {
            val name = pair.substringBefore('=')
            val value = runCatching { java.net.URLDecoder.decode(pair.substringAfter('=', ""), "UTF-8") }.getOrNull() ?: continue
            params.putIfAbsent(name, value)
        }
        val runner = params["runner"].orEmpty().trim().lowercase()
        val provider = params["provider"].orEmpty().trim().lowercase()
        if (!validRunner(runner) || !validProvider(provider)) return null
        return RunnerSignInTarget(runner, provider)
    }

    fun dashboardUrl(origin: String, runner: String, provider: String): String {
        val base = origin.trimEnd('/') + DASHBOARD_PATH
        if (!validRunner(runner) || !validProvider(provider)) return base
        return "$base?runner=$runner&provider=$provider&connect=1"
    }

    fun connectUrl(origin: String, item: RunnerAttentionItem): String =
        if (validConnectPath(item.connectUrl)) origin.trimEnd('/') + item.connectUrl
        else dashboardUrl(origin, item.runner, item.provider)
}
