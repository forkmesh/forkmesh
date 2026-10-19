package com.forkmesh.nativeapp.runners

import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.bool
import com.forkmesh.nativeapp.json.double
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger
import kotlin.math.roundToInt

data class RunnerUsageWindow(
    val id: String,
    val label: String,
    val usedPercent: Double,
    val resetsAtMs: Long = 0,
    val windowMinutes: Long = 0,
)

data class RunnerAccount(
    val id: String,
    val label: String = "",
    val email: String = "",
    val plan: String = "",
    val active: Boolean = false,
    val usageRead: Boolean = false,
    val usageError: String = "",
    val usageFetchedAtMs: Long = 0,
    val usage: List<RunnerUsageWindow> = emptyList(),
) {
    val displayName: String get() = label.ifEmpty { email }.ifEmpty { "Account" }
}

data class RunnerAccountRequest(
    val id: String,
    val provider: String,
    val action: String,
    val account: String,
    val state: String,
    val message: String = "",
    val createdAtMs: Long = 0,
) {
    val pending: Boolean get() = state == "pending"
}

data class RunnerProviderAccounts(
    val provider: String,
    val reported: Boolean = false,
    val binaryFound: Boolean = false,
    val accounts: List<RunnerAccount> = emptyList(),
) {
    val activeAccount: RunnerAccount? get() = accounts.firstOrNull { it.active }
}

data class RunnerAccountsRunner(
    val name: String,
    val scope: String = "",
    val status: String = "",
    val lastSeenAtMs: Long = 0,
    val providers: List<RunnerProviderAccounts> = emptyList(),
    val accountRequests: List<RunnerAccountRequest> = emptyList(),
) {
    fun provider(provider: String): RunnerProviderAccounts =
        providers.firstOrNull { it.provider == provider } ?: RunnerProviderAccounts(provider)

    fun accountRequestPending(provider: String, account: String): Boolean =
        accountRequests.any { it.provider == provider && it.account == account && it.pending }

    fun accountRequest(provider: String, account: String): RunnerAccountRequest? =
        accountRequests.filter { it.provider == provider && it.account == account }.maxByOrNull { it.createdAtMs }
}

data class RunnerAccountsSection(
    val runner: String,
    val provider: String,
    val binaryFound: Boolean,
    val accounts: List<RunnerAccount>,
    val pendingAccounts: Set<String>,
) {
    val key: String get() = "$runner:$provider"
    val canSignIn: Boolean get() = binaryFound
}

object RunnerAccounts {
    const val ACTIVATE = "activate"
    private const val DOT = " · "

    fun accountsPath(runner: String): String = "/api/runners/${runner.trim().lowercase()}/accounts"

    fun parse(payload: JsonMap): List<RunnerAccountsRunner> {
        val out = ArrayList<RunnerAccountsRunner>()
        for (row in payload.maps("runners")) {
            val name = row.str("name").trim()
            if (!RunnerAttention.validRunner(name)) continue
            val availability = row.obj("availability")
            val providers = RunnerAttention.PROVIDERS.map { provider ->
                val raw = availability[provider]
                if (raw !is Map<*, *>) {
                    RunnerProviderAccounts(provider)
                } else {
                    val entry = raw.asJsonMap()
                    RunnerProviderAccounts(
                        provider = provider,
                        reported = true,
                        binaryFound = entry.bool("binaryFound"),
                        accounts = entry.maps("accounts").map(::parseAccount).filter { RunnerAttention.validLoginId(it.id) },
                    )
                }
            }
            val requests = row.maps("accountRequests").map { item ->
                RunnerAccountRequest(
                    id = item.str("id").trim(),
                    provider = item.str("provider").trim(),
                    action = item.str("action").trim(),
                    account = item.str("account").trim(),
                    state = item.str("state").trim(),
                    message = item.str("message").trim(),
                    createdAtMs = jsonLong(item["createdAt"]),
                )
            }.filter { RunnerAttention.validLoginId(it.id) }
            out += RunnerAccountsRunner(
                name = name,
                scope = row.str("scope").trim(),
                status = row.str("status").trim(),
                lastSeenAtMs = jsonLong(row["lastSeenAt"]),
                providers = providers,
                accountRequests = requests,
            )
        }
        return out
    }

    private fun parseAccount(item: JsonMap): RunnerAccount {
        val usage = item["usage"]
        val usageMap = (usage as? Map<*, *>)?.asJsonMap()
        val windows = usageMap?.maps("windows")?.mapNotNull { window ->
            val id = window.str("id").trim()
            if (id.isEmpty()) {
                null
            } else {
                RunnerUsageWindow(
                    id = id,
                    label = window.str("label").trim().ifEmpty { id },
                    usedPercent = window.double("usedPercent").coerceIn(0.0, 100.0),
                    resetsAtMs = jsonLong(window["resetsAt"]),
                    windowMinutes = jsonLong(window["windowMinutes"]),
                )
            }
        } ?: emptyList()
        return RunnerAccount(
            id = item.str("id").trim(),
            label = item.str("label").trim(),
            email = item.str("email").trim(),
            plan = item.str("plan").trim(),
            active = item.bool("active"),
            usageRead = usageMap != null,
            usageError = usageMap?.str("error")?.trim().orEmpty(),
            usageFetchedAtMs = jsonLong(usageMap?.get("fetchedAt")),
            usage = windows,
        )
    }

    fun sections(runners: List<RunnerAccountsRunner>, provider: String? = null): List<RunnerAccountsSection> {
        val out = ArrayList<RunnerAccountsSection>()
        for (runner in runners) {
            if (runner.status != "active" && runner.status != "paused") continue
            for (name in RunnerAttention.PROVIDERS) {
                if (provider != null && name != provider) continue
                val state = runner.provider(name)
                if (!state.binaryFound && state.accounts.isEmpty()) continue
                out += RunnerAccountsSection(
                    runner = runner.name,
                    provider = name,
                    binaryFound = state.binaryFound,
                    accounts = state.accounts,
                    pendingAccounts = state.accounts.filter { runner.accountRequestPending(name, it.id) }.map { it.id }.toSet(),
                )
            }
        }
        return out
    }

    fun hasAccounts(runners: List<RunnerAccountsRunner>): Boolean = sections(runners).any { it.accounts.isNotEmpty() }

    fun anyPending(runners: List<RunnerAccountsRunner>): Boolean = runners.any { runner -> runner.accountRequests.any { it.pending } }

    fun accountCount(runners: List<RunnerAccountsRunner>): Int = sections(runners).sumOf { it.accounts.size }

    fun accountTitle(account: RunnerAccount, pending: Boolean): String {
        val parts = arrayListOf(account.displayName)
        if (account.plan.isNotEmpty()) parts += account.plan
        if (pending) parts += "switching…"
        return parts.joinToString(DOT)
    }

    fun accountHint(account: RunnerAccount, provider: String, runner: String): String =
        if (account.active) "Every ${RunnerAttention.providerShort(provider)} job on $runner runs as this account"
        else "Use this account for every ${RunnerAttention.providerShort(provider)} job on $runner"

    fun addLabel(section: RunnerAccountsSection): String =
        if (section.accounts.isEmpty()) "Sign in ${RunnerAttention.providerShort(section.provider)} on ${section.runner}…"
        else "Add ${RunnerAttention.providerShort(section.provider)} account on ${section.runner}…"

    fun usageErrorText(error: String): String = when (error) {
        "token_expired" -> "usage refreshes after its next run"
        "unauthorized" -> "the provider refused this login; sign it in again"
        "no_oauth_login" -> "usage limits need a subscription sign-in"
        "missing_login" -> "saved login missing; sign it in again"
        "unreachable" -> "usage service unreachable"
        else -> "usage unavailable ($error)"
    }

    fun durationText(ms: Long): String {
        val minutes = maxOf(1L, (ms + 30_000) / 60_000)
        if (minutes < 60) return "$minutes min"
        val hours = minutes / 60
        if (hours < 48) return "$hours h ${minutes % 60} min"
        return "${(hours + 12) / 24} days"
    }

    fun windowText(window: RunnerUsageWindow, nowMs: Long): String {
        val text = "${window.label} ${window.usedPercent.roundToInt()}%"
        return if (window.resetsAtMs > nowMs) "$text (resets in ${durationText(window.resetsAtMs - nowMs)})" else text
    }

    fun usageText(account: RunnerAccount, nowMs: Long): String {
        if (!account.usageRead) return "usage not read yet"
        val parts = account.usage.mapTo(ArrayList()) { windowText(it, nowMs) }
        if (account.usageError.isNotEmpty()) parts += usageErrorText(account.usageError)
        else if (parts.isEmpty()) parts += "no usage limits reported"
        return parts.joinToString(DOT)
    }

    fun switchingNotice(account: RunnerAccount, runner: String): String =
        "Switching $runner to ${account.displayName}; the runner applies it on its next check-in."

    fun switchedNotice(account: RunnerAccount, provider: String, runner: String): String =
        "$runner now runs ${RunnerAttention.providerLong(provider)} as ${account.displayName}."

    fun switchFailed(request: RunnerAccountRequest): String =
        "Could not change the runner account: ${request.message.ifEmpty { "the runner could not switch accounts" }}"

    fun changeError(status: Int, body: JsonMap): String {
        val detail = body.str("message").trim().ifEmpty {
            when {
                status == 401 -> "the account session was refused; sign in again"
                status == 403 -> "this account may not change that runner's accounts"
                status == 404 && body.str("error") == "unknown_account" -> "the runner no longer has that account"
                status == 404 -> "that runner is not registered"
                status == 429 -> "too many changes; try again in a minute"
                status > 0 -> "HTTP $status"
                else -> "ForkMesh could not be reached"
            }
        }
        return "Could not change the runner account: $detail"
    }

    fun loadError(status: Int, body: JsonMap): String {
        val detail = body.str("message").trim().ifEmpty { if (status > 0) "HTTP $status" else "ForkMesh could not be reached" }
        return "Could not load runners: $detail"
    }
}

data class RunnerAccountsState(
    val runners: List<RunnerAccountsRunner> = emptyList(),
    val loaded: Boolean = false,
    val loading: Boolean = false,
    val hidden: Boolean = false,
    val error: String = "",
    val notice: String = "",
    val noticeError: Boolean = false,
) {
    val sections: List<RunnerAccountsSection> get() = RunnerAccounts.sections(runners)
    val hasAccounts: Boolean get() = !hidden && RunnerAccounts.hasAccounts(runners)
    val anyPending: Boolean get() = RunnerAccounts.anyPending(runners)
}

class RunnerAccountsStore(
    private val api: RunnerAttentionApi,
    private val clock: () -> Long = { System.currentTimeMillis() },
    private val sleep: (Long) -> Unit = { Thread.sleep(it) },
) {
    companion object {
        const val PENDING_POLL_MS = 2_000L
        const val PENDING_MAX_READS = 30
        const val REFRESH_FLOOR_MS = 60_000L
    }

    @Volatile
    var state: RunnerAccountsState = RunnerAccountsState()
        private set

    private data class Watch(val runner: String, val provider: String, val account: RunnerAccount)

    @Volatile
    private var watch: Watch? = null

    private var lastLoadMs = 0L
    private val remaining = AtomicInteger(0)
    private val following = AtomicBoolean(false)
    private val listeners = CopyOnWriteArrayList<(RunnerAccountsState) -> Unit>()

    fun addListener(listener: (RunnerAccountsState) -> Unit) {
        listeners += listener
    }

    fun removeListener(listener: (RunnerAccountsState) -> Unit) {
        listeners -= listener
    }

    fun refresh(force: Boolean = false): Boolean {
        synchronized(this) {
            val now = clock()
            if (!force && lastLoadMs > 0 && now - lastLoadMs < REFRESH_FLOOR_MS) return false
            lastLoadMs = now
        }
        load()
        return true
    }

    fun load() {
        if (!api.hasSession) {
            update { RunnerAccountsState(loaded = true, hidden = true) }
            return
        }
        update { it.copy(loading = true) }
        val reply = try {
            api.runners()
        } catch (_: Exception) {
            null
        }
        update { current ->
            when {
                reply == null -> current.copy(loading = false, error = RunnerAccounts.loadError(0, emptyMap()))
                reply.status == 401 || reply.status == 403 -> RunnerAccountsState(loaded = true, hidden = true)
                reply.ok && reply.body["ok"] != false -> settle(
                    current.copy(
                        runners = RunnerAccounts.parse(reply.body),
                        loaded = true,
                        loading = false,
                        hidden = false,
                        error = "",
                    ),
                )
                else -> current.copy(loading = false, error = RunnerAccounts.loadError(reply.status, reply.body))
            }
        }
    }

    fun activate(runner: String, provider: String, account: RunnerAccount): Boolean {
        if (account.active || !RunnerAttention.validRunner(runner) || !RunnerAttention.validProvider(provider)) return false
        val reply = try {
            api.activateAccount(runner, provider, account.id)
        } catch (_: Exception) {
            null
        }
        val accepted = reply != null && reply.status in 200..299
        watch = if (accepted) Watch(runner, provider, account) else null
        update {
            if (accepted) it.copy(notice = RunnerAccounts.switchingNotice(account, runner), noticeError = false)
            else it.copy(notice = RunnerAccounts.changeError(reply?.status ?: 0, reply?.body ?: emptyMap()), noticeError = true)
        }
        remaining.set(PENDING_MAX_READS)
        load()
        followPending()
        return accepted
    }

    fun followPending() {
        if (!following.compareAndSet(false, true)) return
        try {
            while (state.anyPending && !state.hidden && remaining.getAndDecrement() > 0) {
                sleep(PENDING_POLL_MS)
                load()
            }
        } finally {
            following.set(false)
        }
    }

    private fun settle(next: RunnerAccountsState): RunnerAccountsState {
        val watched = watch ?: return next
        val request = next.runners.firstOrNull { it.name == watched.runner }
            ?.accountRequest(watched.provider, watched.account.id) ?: return next
        val notice = when (request.state) {
            "done" -> RunnerAccounts.switchedNotice(watched.account, watched.provider, watched.runner)
            "failed" -> RunnerAccounts.switchFailed(request)
            else -> return next
        }
        watch = null
        return next.copy(notice = notice, noticeError = request.state == "failed")
    }

    fun clear() {
        watch = null
        synchronized(this) { lastLoadMs = 0 }
        remaining.set(0)
        update { RunnerAccountsState() }
    }

    private fun update(change: (RunnerAccountsState) -> RunnerAccountsState) {
        val next = synchronized(this) {
            change(state).also { state = it }
        }
        for (listener in listeners) listener(next)
    }
}
