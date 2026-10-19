package com.forkmesh.nativeapp.runners

import com.forkmesh.nativeapp.format.relativeTime
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str

enum class RunnerStatus(val raw: String) {
    Active("active"), Paused("paused"), Revoked("revoked"), Unknown("");

    companion object {
        fun of(raw: String?): RunnerStatus {
            val value = raw?.trim()?.lowercase().orEmpty()
            return entries.firstOrNull { it != Unknown && it.raw == value } ?: Unknown
        }
    }
}

enum class RunnerAction(val raw: String, val label: String) {
    SignIn("sign-in", "Sign in"),
    Accounts("accounts", "Accounts"),
    Pause("pause", "Pause"),
    Resume("resume", "Resume"),
    Revoke("revoke", "Revoke"),
}

enum class RunnerAuth { Ok, Expiring, Expired, SignedOut, NotInstalled }

data class RunnerProviderState(
    val provider: String,
    val binaryFound: Boolean,
    val auth: RunnerAuth,
    val message: String = "",
    val expiresAtMs: Long = 0,
    val activeAccount: RunnerAccount? = null,
    val accountCount: Int = 0,
) {
    val needsSignIn: Boolean get() = auth == RunnerAuth.Expired || auth == RunnerAuth.SignedOut || auth == RunnerAuth.Expiring
}

data class RunnerRow(
    val name: String,
    val scope: String = "",
    val status: RunnerStatus = RunnerStatus.Unknown,
    val lastSeenAtMs: Long = 0,
    val activeLeases: Int = 0,
    val providers: List<RunnerProviderState> = emptyList(),
) {
    val revoked: Boolean get() = status == RunnerStatus.Revoked

    fun online(nowMs: Long): Boolean = RunnerList.online(lastSeenAtMs, nowMs)

    val actions: List<RunnerAction> get() = RunnerList.actions(status)

    val signInProviders: List<String> get() = providers.filter { it.binaryFound }.map { it.provider }
}

sealed class RunnerListResult {
    data object SignedOut : RunnerListResult()
    data class Loaded(val rows: List<RunnerRow>) : RunnerListResult()
    data class Failed(val message: String) : RunnerListResult()
}

data class RunnerChange(val ok: Boolean, val status: RunnerStatus?, val message: String)

object RunnerList {
    const val ONLINE_WINDOW_MS = 10 * 60_000L
    const val PAUSE_PATH = "/api/runners/pause"
    const val RESUME_PATH = "/api/runners/resume"
    const val REVOKE_PATH = "/api/runners/revoke"
    private const val DOT = " · "

    fun parse(payload: JsonMap): List<RunnerRow> {
        val out = ArrayList<RunnerRow>()
        for (row in payload.maps("runners")) {
            val name = row.str("name").trim().lowercase()
            if (!RunnerAttention.validRunner(name)) continue
            val availability = row.obj("availability")
            val providers = RunnerAttention.PROVIDERS.mapNotNull { provider ->
                val raw = availability[provider] as? Map<*, *> ?: return@mapNotNull null
                providerState(provider, raw.asJsonMap())
            }
            out += RunnerRow(
                name = name,
                scope = row.str("scope").trim(),
                status = RunnerStatus.of(row.str("status")),
                lastSeenAtMs = jsonLong(row["lastSeenAt"]).coerceAtLeast(0),
                activeLeases = jsonLong(row["activeLeases"]).coerceIn(0, Int.MAX_VALUE.toLong()).toInt(),
                providers = providers,
            )
        }
        return sorted(out)
    }

    fun load(api: RunnerAttentionApi): RunnerListResult {
        if (!api.hasSession) return RunnerListResult.SignedOut
        val reply = try {
            api.runners()
        } catch (_: Exception) {
            return RunnerListResult.Failed(RunnerAccounts.loadError(0, emptyMap()))
        }
        return when {
            reply.status == 401 || reply.status == 403 -> RunnerListResult.SignedOut
            reply.ok && reply.body["ok"] != false -> RunnerListResult.Loaded(parse(reply.body))
            else -> RunnerListResult.Failed(RunnerAccounts.loadError(reply.status, reply.body))
        }
    }

    fun change(api: RunnerAttentionApi, action: RunnerAction, name: String): RunnerChange {
        if (actionPath(action) == null || !RunnerAttention.validRunner(name.trim().lowercase())) {
            return RunnerChange(false, null, actionError(action, 0, emptyMap()))
        }
        val reply = try {
            api.changeStatus(action, name)
        } catch (_: Exception) {
            null
        }
        if (reply == null || !reply.ok || reply.body["ok"] == false) {
            return RunnerChange(false, null, actionError(action, reply?.status ?: 0, reply?.body ?: emptyMap()))
        }
        val status = RunnerStatus.of(reply.body.str("status")).takeIf { it != RunnerStatus.Unknown } ?: statusAfter(action)
        return RunnerChange(true, status, actionNotice(action, name.trim().lowercase()))
    }

    fun sorted(rows: List<RunnerRow>): List<RunnerRow> = rows.sortedBy { if (it.revoked) 1 else 0 }

    fun providerState(provider: String, entry: JsonMap): RunnerProviderState {
        val auth = entry.obj("auth")
        val accounts = entry.maps("accounts")
        val active = accounts.firstOrNull { it.bool("active") }?.let { account ->
            RunnerAccount(
                id = account.str("id").trim(),
                label = account.str("label").trim(),
                email = account.str("email").trim(),
                plan = account.str("plan").trim(),
                active = true,
            )
        }
        return RunnerProviderState(
            provider = provider,
            binaryFound = entry.bool("binaryFound"),
            auth = authOf(entry),
            message = auth.str("message").filter { !Character.isISOControl(it) }.trim().take(240),
            expiresAtMs = jsonLong(auth["expiresAt"]).coerceAtLeast(0),
            activeAccount = active,
            accountCount = accounts.size,
        )
    }

    fun authOf(entry: JsonMap): RunnerAuth {
        if (!entry.bool("binaryFound")) return RunnerAuth.NotInstalled
        val loginState = entry.str("loginState").trim().lowercase()
        val authState = entry.obj("auth").str("state").trim().lowercase()
        return when {
            loginState == "expired" || authState == "expired" -> RunnerAuth.Expired
            authState == "expiring" -> RunnerAuth.Expiring
            authState == "missing" || loginState == "missing" -> RunnerAuth.SignedOut
            else -> RunnerAuth.Ok
        }
    }

    fun online(lastSeenAtMs: Long, nowMs: Long): Boolean = lastSeenAtMs > 0 && nowMs - lastSeenAtMs <= ONLINE_WINDOW_MS

    fun actions(status: RunnerStatus): List<RunnerAction> = when (status) {
        RunnerStatus.Active -> listOf(RunnerAction.SignIn, RunnerAction.Accounts, RunnerAction.Pause, RunnerAction.Revoke)
        RunnerStatus.Paused -> listOf(RunnerAction.SignIn, RunnerAction.Accounts, RunnerAction.Resume, RunnerAction.Revoke)
        RunnerStatus.Revoked, RunnerStatus.Unknown -> emptyList()
    }

    fun actionPath(action: RunnerAction): String? = when (action) {
        RunnerAction.Pause -> PAUSE_PATH
        RunnerAction.Resume -> RESUME_PATH
        RunnerAction.Revoke -> REVOKE_PATH
        RunnerAction.SignIn, RunnerAction.Accounts -> null
    }

    fun actionBody(name: String): Map<String, Any?> = mapOf("name" to name.trim().lowercase())

    fun statusAfter(action: RunnerAction): RunnerStatus? = when (action) {
        RunnerAction.Pause -> RunnerStatus.Paused
        RunnerAction.Resume -> RunnerStatus.Active
        RunnerAction.Revoke -> RunnerStatus.Revoked
        RunnerAction.SignIn, RunnerAction.Accounts -> null
    }

    fun statusLabel(status: RunnerStatus): String = when (status) {
        RunnerStatus.Paused -> "Paused"
        RunnerStatus.Revoked -> "Revoked"
        RunnerStatus.Active, RunnerStatus.Unknown -> ""
    }

    fun jobsText(activeLeases: Int): String = when (activeLeases) {
        0 -> "No jobs"
        1 -> "1 job"
        else -> "$activeLeases jobs"
    }

    fun authText(auth: RunnerAuth): String = when (auth) {
        RunnerAuth.Ok -> "Signed in"
        RunnerAuth.Expiring -> "Sign-in expires soon"
        RunnerAuth.Expired -> "Sign-in expired"
        RunnerAuth.SignedOut -> "Not signed in"
        RunnerAuth.NotInstalled -> "Not installed"
    }

    fun providerText(state: RunnerProviderState): String {
        val parts = arrayListOf(authText(state.auth))
        if (state.auth != RunnerAuth.NotInstalled) state.activeAccount?.let { parts += it.displayName }
        return parts.joinToString(DOT)
    }

    fun seenText(lastSeenAtMs: Long, nowMs: Long): String = when {
        online(lastSeenAtMs, nowMs) -> "Online"
        lastSeenAtMs <= 0 -> "Never seen"
        else -> "Seen ${relativeTime(lastSeenAtMs, nowMs)}"
    }

    fun summary(row: RunnerRow): String =
        listOf(row.scope, jobsText(row.activeLeases)).filter { it.isNotEmpty() }.joinToString(DOT)

    fun revokeTitle(name: String): String = "Revoke $name?"

    fun revokeMessage(name: String): String =
        "$name stops taking jobs and its key stops working. This cannot be undone; to use the machine again, register it as a new runner."

    fun actionNotice(action: RunnerAction, name: String): String = when (action) {
        RunnerAction.Pause -> "$name is paused. It finishes the jobs it holds and takes no new ones."
        RunnerAction.Resume -> "$name is taking jobs again."
        RunnerAction.Revoke -> "$name is revoked."
        RunnerAction.SignIn, RunnerAction.Accounts -> ""
    }

    fun actionError(action: RunnerAction, status: Int, body: JsonMap): String {
        val detail = body.str("message").trim().ifEmpty {
            when {
                status == 401 -> "the account session was refused; sign in again"
                status == 404 -> "that runner is not registered to this account"
                status == 409 -> "that runner is revoked"
                status == 429 -> "too many changes; try again in a minute"
                status > 0 -> "HTTP $status"
                else -> "ForkMesh could not be reached"
            }
        }
        return "Could not ${action.label.lowercase()} the runner: $detail"
    }
}
