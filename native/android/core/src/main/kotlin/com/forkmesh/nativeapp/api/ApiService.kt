package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.control.DesktopCommandContract
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.ActionLog
import com.forkmesh.nativeapp.model.collapseRepositoryMirrors
import com.forkmesh.nativeapp.model.AgentSession
import com.forkmesh.nativeapp.model.AgentSessionPage
import com.forkmesh.nativeapp.model.AgentTranscript
import com.forkmesh.nativeapp.model.BountyWallet
import com.forkmesh.nativeapp.model.DesktopCommandResult
import com.forkmesh.nativeapp.model.DiscussionEvent
import com.forkmesh.nativeapp.model.FederatedReply
import com.forkmesh.nativeapp.model.Issue
import com.forkmesh.nativeapp.model.IssueBounty
import com.forkmesh.nativeapp.model.IssueEvent
import com.forkmesh.nativeapp.model.NetworkLeaderboards
import com.forkmesh.nativeapp.model.NetworkStats
import com.forkmesh.nativeapp.model.NotificationPage
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.model.OrgMember
import com.forkmesh.nativeapp.model.OrgProfile
import com.forkmesh.nativeapp.model.OrgRepo
import com.forkmesh.nativeapp.model.OrgSummary
import com.forkmesh.nativeapp.model.OrgTeam
import com.forkmesh.nativeapp.model.PingSettings
import com.forkmesh.nativeapp.model.PublishedPull
import com.forkmesh.nativeapp.model.PullEvent
import com.forkmesh.nativeapp.model.PullRequest
import com.forkmesh.nativeapp.model.RepoActions
import com.forkmesh.nativeapp.model.RepoBlob
import com.forkmesh.nativeapp.model.RepoBranch
import com.forkmesh.nativeapp.model.RepoCommitDetail
import com.forkmesh.nativeapp.model.RepoDiscussion
import com.forkmesh.nativeapp.model.RepoMirror
import com.forkmesh.nativeapp.model.RepoMirrorSet
import com.forkmesh.nativeapp.model.RepoRelease
import com.forkmesh.nativeapp.model.RepoSearchResults
import com.forkmesh.nativeapp.model.RepoTree
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.model.SponsoredAd
import com.forkmesh.nativeapp.model.ChatRetention
import com.forkmesh.nativeapp.model.ChatRetentionException
import com.forkmesh.nativeapp.model.UserStatus
import com.forkmesh.nativeapp.model.UserStatusException
import com.forkmesh.nativeapp.model.auth.AccountDevice
import com.forkmesh.nativeapp.model.dartDoubleTryParse
import com.forkmesh.nativeapp.model.dartIntTryParse
import com.forkmesh.nativeapp.model.modelInt
import com.forkmesh.nativeapp.model.orgErrorMessage
import com.forkmesh.nativeapp.model.chatRetentionErrorMessage
import com.forkmesh.nativeapp.model.userStatusErrorMessage
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.security.OwnerKeys
import java.net.SocketTimeoutException
import java.util.Base64
import java.util.concurrent.Callable
import java.util.concurrent.CompletableFuture
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.ExecutionException
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger

open class ApiService(
    private val settings: ClientSettings,
    private val transport: HttpTransport = HttpTransport.DEFAULT,
    private val clock: () -> Long = { System.currentTimeMillis() },
) {
    companion object {
        const val BUG_REPORT_INBOX_MISSING = "This relay cannot list reported bugs yet. Update the relay and try again."
        const val CACHE_TTL_MS = 60_000L
        const val ACCOUNT_STATUS_TTL_MS = 120_000L
        const val OFFLINE_REPO_TTL_MS = 60_000L
        const val CATALOG_TTL_MS = 30_000L

        const val SPONSORED_AD_TTL_MS = 30 * 60_000L

        const val SPONSORED_AD_FAILURE_TTL_MS = 5 * 60_000L
        private const val SPONSORED_AD_TIMEOUT_MS = 6_000

        const val PULL_LEDGER_REF = "forkmesh/pulls"
        const val BLOB_BATCH = 50
        private const val BLOB_FALLBACK_WIDTH = 2
        private const val RECORD_WIDTH = 4

        private val FAN_OUT: ExecutorService = Executors.newCachedThreadPool { runnable ->
            Thread(runnable, "ApiService-fanout").apply { isDaemon = true }
        }

        private val REPO_KEYS = listOf(
            "repositories", "issues", "pulls", "commits", "discussions", "mirrors",
            "branches", "entries", "tree", "data", "items", "agents",
        )
    }

    @Volatile
    var sessionTokenProvider: (() -> String)? = null

    @Volatile
    var onUnauthorized: (() -> Unit)? = null

    @Volatile
    var onSessionInvalid: ((String) -> Unit)? = null

    @Volatile
    var onRouteMissing: ((String) -> Unit)? = null

    @Volatile
    var ownerKeysProvider: (() -> OwnerKeys?)? = null

    var ownerKeys: OwnerKeys?
        get() = ownerKeysProvider?.invoke()
        set(value) {
            ownerKeysProvider = if (value == null) null else ({ value })
        }

    protected fun sessionToken(): String = sessionTokenProvider?.invoke() ?: ""

    private class CacheEntry<T>(val future: CompletableFuture<T>, val createdAt: Long)

    private val repositoriesCache = HashMap<String, CacheEntry<List<Repository>>>()
    private val networkStatsCache = HashMap<String, CacheEntry<NetworkStats>>()
    private val networkLeaderboardsCache = HashMap<String, CacheEntry<NetworkLeaderboards>>()
    private val sponsoredAdLock = Any()
    private var sponsoredAdEntry: CacheEntry<SponsoredAd?>? = null
    private var sponsoredAdTtlMs = SPONSORED_AD_TTL_MS
    private val offlineRepoUntil = ConcurrentHashMap<String, Long>()
    private val treeCache = HashMap<String, CacheEntry<RepoTree>>()
    private val blobCache = HashMap<String, CacheEntry<RepoBlob>>()
    private val issuesCache = HashMap<String, CacheEntry<List<Issue>>>()
    private val pullsCache = HashMap<String, CacheEntry<List<PublishedPull>>>()
    private val discussionsCache = HashMap<String, CacheEntry<List<RepoDiscussion>>>()
    private val commitsCache = HashMap<String, CacheEntry<List<Map<String, Any?>>>>()
    private val releasesCache = HashMap<String, CacheEntry<List<RepoRelease>>>()
    private val accountAvatarCache = HashMap<String, CompletableFuture<String>>()
    private class AccountStatusEntry(val status: UserStatus, val createdAt: Long, val permanent: Boolean)
    private val accountStatusCache = HashMap<String, AccountStatusEntry>()
    private val enrollmentLock = Any()
    private val publicRouteOwners = ConcurrentHashMap<String, String>()
    private val publicRouteProven: MutableSet<String> = ConcurrentHashMap.newKeySet()

    fun readOwner(owner: String, name: String): String =
        publicRouteOwners["${owner.lowercase()}/${name.lowercase()}"] ?: owner

    private fun getRepoRead(
        owner: String,
        name: String,
        operation: String,
        query: Map<String, String>? = null,
        repeated: List<Pair<String, String>> = emptyList(),
    ): Any? {
        fun target(who: String) = withPairs(base("/api/repo/$who/$name/$operation", query), repeated)
        val public = readOwner(owner, name)
        if (public == owner) return getJson(target(owner))
        val key = "${owner.lowercase()}/${name.lowercase()}"
        val publicTarget = target(public)
        if (key in publicRouteProven) return getJson(publicTarget)
        try {
            val data = getJson(publicTarget)
            publicRouteProven.add(key)
            return data
        } catch (cancelled: InterruptedException) {
            throw cancelled
        } catch (_: Exception) {
        }
        return getJson(target(owner))
    }

    private fun withPairs(target: Target, pairs: List<Pair<String, String>>): Target {
        if (pairs.isEmpty()) return target
        val query = pairs.joinToString("&") { DartUri.queryString(mapOf(it)) }
        return Target(target.url + (if ('?' in target.url) "&" else "?") + query, target.path, target.host)
    }
    private var enrolledFor = ""

    open fun clearRepoCache(owner: String, name: String) {
        val prefix = "$owner/$name:"
        synchronized(treeCache) { treeCache.keys.removeAll { it.startsWith(prefix) } }
        synchronized(blobCache) { blobCache.keys.removeAll { it.startsWith(prefix) } }
        synchronized(issuesCache) { issuesCache.remove("$owner/$name") }
        synchronized(pullsCache) { pullsCache.remove("$owner/$name") }
        synchronized(discussionsCache) { discussionsCache.remove("$owner/$name") }
        synchronized(commitsCache) { commitsCache.remove("$owner/$name") }
        synchronized(releasesCache) { releasesCache.remove("$owner/$name") }
        offlineRepoUntil.remove("$owner/$name")
    }

    private fun <T> cached(cache: HashMap<String, CacheEntry<T>>, key: String, ttlMs: Long, load: () -> T): T {
        val future: CompletableFuture<T>
        val owner: Boolean
        val now = clock()
        synchronized(cache) {
            val existing = cache[key]
            if (existing != null && now - existing.createdAt < ttlMs) {
                future = existing.future
                owner = false
            } else {
                future = CompletableFuture()
                cache[key] = CacheEntry(future, now)
                owner = true
            }
        }
        if (owner) {
            try {
                future.complete(load())
            } catch (error: Throwable) {
                synchronized(cache) {
                    if (cache[key]?.future === future) cache.remove(key)
                }
                future.completeExceptionally(error)
                throw error
            }
        }
        return await(future)
    }

    private fun <T> await(future: CompletableFuture<T>): T = try {
        future.get()
    } catch (wrapped: ExecutionException) {
        throw wrapped.cause ?: wrapped
    }

    private fun <T, R> parallelMap(items: List<T>, width: Int, block: (T) -> R): List<R> {
        if (items.size <= 1 || width <= 1) return items.map(block)
        val next = AtomicInteger(0)
        val out = arrayOfNulls<Any?>(items.size)
        val workers = List(minOf(width, items.size)) {
            FAN_OUT.submit(Callable {
                while (true) {
                    val index = next.getAndIncrement()
                    if (index >= items.size) break
                    out[index] = block(items[index])
                }
            })
        }
        for (worker in workers) {
            try {
                worker.get()
            } catch (wrapped: ExecutionException) {
                throw wrapped.cause ?: wrapped
            }
        }
        @Suppress("UNCHECKED_CAST")
        return out.toList() as List<R>
    }

    private class Target(val url: String, val path: String, val host: String)

    private fun base(path: String, query: Map<String, String>? = null): Target {
        val server = ServerUrl.parse(settings.serverUrl)
        val cleanQuery = if (query.isNullOrEmpty()) null else query
        val cleanPath = if (path.startsWith("/")) path else "/$path"
        return Target(server.http(cleanPath, cleanQuery), DartUri.encodePath(cleanPath), server.host)
    }

    fun rawUri(owner: String, name: String, path: String, ref: String = ""): String =
        base(
            "/api/repo/${readOwner(owner, name)}/$name/raw",
            LinkedHashMap<String, String>().apply {
                put("path", path)
                if (ref.isNotEmpty()) put("ref", ref)
            },
        ).url

    fun cloneUri(owner: String, name: String): String = base("/$owner/$name").url

    val gitOrigin: String get() = ServerUrl.parse(settings.serverUrl).httpOrigin

    fun accountEventsUri(ticket: String, node: String = ""): String =
        ServerUrl.parse(settings.serverUrl).webSocket(
            "/api/nodes/events",
            if (node.isEmpty()) mapOf("ticket" to ticket) else linkedMapOf("ticket" to ticket, "node" to node),
        )

    private fun withAuth(headers: Map<String, String>): Map<String, String> {
        val token = sessionToken()
        if (token.isEmpty()) return headers
        return LinkedHashMap(headers).apply { put("Authorization", "Bearer $token") }
    }

    private fun exchange(
        method: String,
        target: Target,
        headers: Map<String, String>,
        body: ByteArray? = null,
        timeoutMs: Int? = null,
    ): HttpClient.Response {
        val request = HttpClient.Request(method, target.url)
        for ((name, value) in headers) request.header(name, value)
        if (body != null) request.body = body
        if (timeoutMs != null) request.timeouts(timeoutMs, timeoutMs)
        return transport.send(request)
    }

    private fun encode(payload: Any?): ByteArray = Json.stringify(payload).toByteArray(Charsets.UTF_8)

    private fun getJson(target: Target): Any? {
        var response: HttpClient.Response? = null
        for (attempt in 0 until 2) {
            response = exchange(
                "GET",
                target,
                withAuth(mapOf("Accept" to "application/json")),
                timeoutMs = if (target.path.contains("/api/repo/")) 8_000 else 12_000,
            )
            if (response.status < 500 || response.status == 503 || attempt == 1) break
            Thread.sleep(250)
        }
        return decodeResponse(target, response!!)
    }

    private fun postJson(target: Target, payload: Map<String, Any?>): Any? {
        val response = exchange(
            "POST",
            target,
            withAuth(mapOf("Accept" to "application/json", "Content-Type" to "application/json")),
            encode(payload),
            timeoutMs = 12_000,
        )
        return decodeResponse(target, response)
    }

    private fun deleteJson(target: Target, payload: Map<String, Any?>): Any? {
        val response = exchange(
            "DELETE",
            target,
            withAuth(mapOf("Accept" to "application/json", "Content-Type" to "application/json")),
            encode(payload),
            timeoutMs = 12_000,
        )
        return decodeResponse(target, response)
    }

    open fun packagesRequest(method: String, path: String, query: List<Pair<String, String>>, body: Map<String, Any?>?): ApiJsonResult {
        val headers = linkedMapOf("Accept" to "application/json")
        if (body != null) headers["Content-Type"] = "application/json"
        if (method != "GET") headers["Origin"] = gitOrigin
        val response = exchange(method, base(path, query.toMap(LinkedHashMap())), withAuth(headers), body?.let(::encode), timeoutMs = 20_000)
        val decoded: Map<String, Any?> = try {
            val text = response.text()
            @Suppress("UNCHECKED_CAST")
            (if (text.isEmpty()) null else Json.parse(text)) as? Map<String, Any?> ?: emptyMap()
        } catch (_: Json.JsonException) {
            emptyMap()
        }
        if (response.status == 401 && sessionToken().isNotEmpty()) {
            onSessionInvalid?.invoke(decoded.str("error"))
            onUnauthorized?.invoke()
        }
        return ApiJsonResult(response.status, decoded)
    }

    private fun postJsonResult(target: Target, payload: Map<String, Any?>): ApiJsonResult {
        val response = exchange(
            "POST",
            target,
            withAuth(mapOf("Accept" to "application/json", "Content-Type" to "application/json")),
            encode(payload),
            timeoutMs = 20_000,
        )
        val body: Map<String, Any?> = try {
            val text = response.text()
            @Suppress("UNCHECKED_CAST")
            (if (text.isEmpty()) null else Json.parse(text)) as? Map<String, Any?> ?: emptyMap()
        } catch (_: Json.JsonException) {
            emptyMap()
        }
        if (response.status == 401 && sessionToken().isNotEmpty()) {
            val code = body.str("error")
            if (code != "bad_signature" && endsSession(target.path, code)) {
                onSessionInvalid?.invoke(code)
                onUnauthorized?.invoke()
            }
        }
        return ApiJsonResult(response.status, body)
    }

    private fun endsSession(path: String, code: String): Boolean =
        !path.contains("/api/repo/") || code == "invalid_session" || code == "device_revoked"

    private fun errorCode(body: String): String {
        if (body.isEmpty()) return ""
        return try {
            val decoded = Json.parse(body)
            if (decoded is Map<*, *> && decoded["error"] != null) jsonText(decoded["error"]) else ""
        } catch (_: Json.JsonException) {
            ""
        }
    }

    private fun repoKeyFromPath(path: String): String? {
        val parts = path.split('/').filter { it.isNotEmpty() }
        if (parts.size >= 4 && parts[0] == "api" && parts[1] == "repo") return "${parts[2]}/${parts[3]}"
        return null
    }

    private fun decodeResponse(target: Target, response: HttpClient.Response): Any? {
        if (response.status in 200..299) {
            val text = response.text()
            if (text.isEmpty()) return emptyList<Any?>()
            return Json.parse(text)
        }
        if (response.status == 404 || response.status == 405) onRouteMissing?.invoke(target.path)
        if (response.status == 401) {
            val hadToken = sessionToken().isNotEmpty()
            val code = errorCode(response.text())
            if (hadToken && endsSession(target.path, code)) {
                onSessionInvalid?.invoke(code)
                onUnauthorized?.invoke()
            }
            if (code == "device_revoked") {
                throw DartException("This device was signed out from the web. Please sign in again.")
            }
            if (target.path.contains("/api/repo/")) {
                throw DartException(
                    "This repo needs an authenticated/private repo view token. Mobile private-repo browsing is not wired yet.",
                )
            }
            throw DartException("Your session expired. Please sign in again.")
        }
        if (response.status == 503 && target.path.contains("/api/repo/")) {
            val repoKey = repoKeyFromPath(target.path)
            if (repoKey != null) offlineRepoUntil[repoKey] = clock() + OFFLINE_REPO_TTL_MS
            throw DartException(
                "Repo host is offline. Open the Qt desktop node for this owner/repo and make sure it is connected to this Worker, then refresh.",
            )
        }
        if (response.status == 500 && target.path.contains("/api/repo/")) {
            throw DartException(
                "Repo host returned an internal error while serving this view. Refresh or retry in a moment; if it keeps happening, reconnect the desktop host for this repo.",
            )
        }
        throw DartException("HTTP ${response.status} for ${target.path}")
    }

    private fun repoOfflineError(repoKey: String): DartException? {
        val until = offlineRepoUntil[repoKey] ?: return null
        if (clock() < until) {
            return DartException(
                "Repo host is offline. Skipping refetch for a minute; tap Retry after reconnecting the desktop host.",
            )
        }
        offlineRepoUntil.remove(repoKey)
        return null
    }

    private fun asList(data: Any?): List<Any?> {
        if (data is List<*>) return data
        if (data is Map<*, *>) {
            for (key in REPO_KEYS) {
                val value = data[key]
                if (value is List<*>) return value
            }
        }
        return emptyList()
    }

    @Suppress("UNCHECKED_CAST")
    private fun asMap(data: Any?): Map<String, Any?>? = data as? Map<String, Any?>

    open fun publishRepository(payload: Map<String, Any?>): ApiJsonResult =
        postJsonResult(base("/api/repositories"), payload)

    open fun createRepositoryImport(
        sourceUrl: String,
        targetOwner: String,
        mode: String = "import",
        providerToken: String = "",
    ): ApiJsonResult {
        val payload = LinkedHashMap<String, Any?>()
        payload["sourceUrl"] = sourceUrl
        payload["targetOwner"] = targetOwner
        payload["mode"] = mode
        if (providerToken.isNotEmpty()) payload["providerToken"] = providerToken
        if (sessionToken().isNotEmpty()) payload["sessionToken"] = sessionToken()
        return postJsonResult(base("/api/repository-imports"), payload)
    }

    open fun accountEventTicket(): EventTicket {
        if (sessionToken().isEmpty()) return EventTicket(EventTicketStatus.SIGNED_OUT)
        val response = try {
            exchange("GET", base("/api/accounts/event-ticket"), withAuth(mapOf("accept" to "application/json")))
        } catch (_: Exception) {
            return EventTicket(EventTicketStatus.UNAVAILABLE)
        }
        if (response.status == 401 || response.status == 403) return EventTicket(EventTicketStatus.SIGNED_OUT)
        if (response.status < 200 || response.status >= 300) return EventTicket(EventTicketStatus.UNAVAILABLE)
        return try {
            val decoded = Json.parse(response.text()) as? Map<*, *>
                ?: return EventTicket(EventTicketStatus.UNAVAILABLE)
            if (decoded["authenticated"] == false) return EventTicket(EventTicketStatus.SIGNED_OUT)
            val ticket = jsonText(decoded["ticket"])
            if (ticket.isEmpty()) EventTicket(EventTicketStatus.SIGNED_OUT) else EventTicket(EventTicketStatus.OK, ticket)
        } catch (_: Exception) {
            EventTicket(EventTicketStatus.UNAVAILABLE)
        }
    }

    open fun registerPushDevice(deviceToken: String, environment: String): PushRegistration {
        if (deviceToken.isEmpty()) return PushRegistration(ok = false)
        return try {
            val result = postJsonResult(
                base("/api/accounts/register-device"),
                linkedMapOf("deviceToken" to deviceToken, "environment" to environment),
            )
            PushRegistration(
                ok = result.ok,
                statusCode = result.statusCode,
                relayConfigured = result.body["pushConfigured"] as? Boolean,
            )
        } catch (_: Exception) {
            PushRegistration(ok = false)
        }
    }

    open fun sendTestPush(): PushTestResult = try {
        val result = postJsonResult(base("/api/accounts/push-test"), emptyMap())
        PushTestResult.fromJson(result.statusCode, result.body)
    } catch (_: Exception) {
        PushTestResult.unreachable()
    }

    open fun unregisterPushDevice(deviceToken: String): Boolean {
        if (deviceToken.isEmpty()) return false
        return try {
            val response = exchange(
                "DELETE",
                base("/api/accounts/register-device"),
                withAuth(mapOf("content-type" to "application/json", "accept" to "application/json")),
                encode(mapOf("deviceToken" to deviceToken)),
            )
            response.status in 200..299
        } catch (_: Exception) {
            false
        }
    }

    open fun submitBugReport(
        message: String,
        contact: String = "",
        screen: String = "",
        severity: String = "normal",
        platform: String = "",
        appVersion: String = "",
        osVersion: String = "",
        device: String = "",
        packageId: String? = null,
        attachments: List<BugReportAttachment> = emptyList(),
    ): BugReportResult {
        val trimmed = message.trim()
        if (trimmed.isEmpty()) return BugReportResult.failure("Describe the problem first.")
        val payload = LinkedHashMap<String, Any?>()
        payload["message"] = trimmed
        if (contact.trim().isNotEmpty()) payload["contact"] = contact.trim()
        if (screen.isNotEmpty()) payload["screen"] = screen
        payload["severity"] = severity
        payload["source"] = "mobile"
        if (platform.isNotEmpty()) payload["platform"] = platform
        if (appVersion.isNotEmpty()) payload["appVersion"] = appVersion
        if (osVersion.isNotEmpty()) payload["osVersion"] = osVersion
        if (device.isNotEmpty()) payload["device"] = device
        packageId?.trim()?.takeIf { it.isNotEmpty() }?.let { payload["package"] = it }
        if (attachments.isNotEmpty()) {
            payload["attachments"] = attachments.map { attachment ->
                linkedMapOf("name" to attachment.name, "contentType" to attachment.contentType,
                    "data" to Base64.getEncoder().encodeToString(attachment.data))
            }
        }
        val response = try {
            exchange(
                "POST",
                base("/api/bug-reports"),
                withAuth(mapOf("Accept" to "application/json", "Content-Type" to "application/json")),
                encode(payload),
                timeoutMs = 20_000,
            )
        } catch (_: SocketTimeoutException) {
            return BugReportResult.failure("The report timed out. Check your connection and try again.")
        } catch (_: Exception) {
            return BugReportResult.failure("Could not reach ForkMesh. Check your connection and try again.")
        }
        if (response.status in 200..299) {
            var id = ""
            try {
                val decoded = Json.parse(response.text())
                if (decoded is Map<*, *>) id = jsonText(decoded["id"])
            } catch (_: Exception) {
            }
            return BugReportResult.success(id)
        }
        if (response.status == 429) {
            return BugReportResult.failure("You have sent several reports in the last hour. Try again later.")
        }
        if (response.status == 413) {
            return BugReportResult.failure("That report is too long. Shorten it and try again.")
        }
        if (response.status == 400) {
            return BugReportResult.failure("Add a little more detail so this can be reproduced.")
        }
        return BugReportResult.failure("ForkMesh could not accept the report. Try again in a moment.")
    }

    private fun wasCancelled(error: Throwable): Boolean =
        error is InterruptedException || error is java.io.InterruptedIOException && error !is java.net.SocketTimeoutException ||
            error is java.nio.channels.ClosedByInterruptException || error is java.util.concurrent.CancellationException

    open fun bugReportInbox(status: String = "", offset: Int = 0, limit: Int = 0): BugReportInbox {
        val query = LinkedHashMap<String, String>()
        if (status.isNotEmpty()) query["status"] = status
        if (offset > 0) query["offset"] = offset.toString()
        if (limit > 0) query["limit"] = limit.toString()
        val response = try {
            exchange(
                "GET",
                base("/api/bug-reports/inbox", query),
                withAuth(mapOf("Accept" to "application/json")),
                timeoutMs = 20_000,
            )
        } catch (error: Exception) {
            if (wasCancelled(error)) return BugReportInbox.CANCELLED_LOAD
            return BugReportInbox.failure("Could not reach ForkMesh. Check your connection and try again.")
        }
        if (response.status == 401 || response.status == 403) {
            return BugReportInbox.failure("Only ForkMesh administrators can read reported bugs.")
        }
        if (response.status == 404 || response.status == 405) {
            return BugReportInbox.failure(BUG_REPORT_INBOX_MISSING)
        }
        val decoded = if (response.status in 200..299) runCatching { Json.parse(response.text()) }.getOrNull() else null
        @Suppress("UNCHECKED_CAST")
        val map = decoded as? Map<String, Any?>
            ?: return BugReportInbox.failure("Reported bugs could not be loaded. Try again in a moment.")
        return BugReportInbox.from(map)
    }

    open fun setBugReportStatus(id: String, status: String): BugReportStatusChange {
        val response = try {
            exchange(
                "POST",
                base("/api/bug-reports/inbox"),
                withAuth(mapOf("Accept" to "application/json", "Content-Type" to "application/json")),
                encode(linkedMapOf("report" to id, "status" to status)),
                timeoutMs = 20_000,
            )
        } catch (_: Exception) {
            return BugReportStatusChange.failure("Could not reach ForkMesh. Check your connection and try again.")
        }
        when (response.status) {
            401, 403 -> return BugReportStatusChange.failure("Only ForkMesh administrators can triage reported bugs.")
            404 -> return BugReportStatusChange.failure(
                if (errorCode(response.text()) == "report_missing") "That report no longer exists." else BUG_REPORT_INBOX_MISSING,
            )
            405 -> return BugReportStatusChange.failure(BUG_REPORT_INBOX_MISSING)
        }
        val decoded = if (response.ok) runCatching { Json.parse(response.text()) }.getOrNull() else null
        @Suppress("UNCHECKED_CAST")
        val map = decoded as? Map<String, Any?>
            ?: return BugReportStatusChange.failure("The status could not be saved. Try again in a moment.")
        @Suppress("UNCHECKED_CAST")
        val counts = map["counts"] as? Map<String, Any?> ?: emptyMap()
        return BugReportStatusChange(
            ok = true,
            status = jsonText(map["status"]).ifEmpty { status },
            counts = counts.mapValues { (it.value as? Number)?.toInt() ?: 0 },
        )
    }

    open fun bugReportAttachment(reportId: String, attachmentId: String): BugReportFile {
        val response = try {
            exchange(
                "GET",
                base("/api/bug-reports/inbox/attachment", linkedMapOf("report" to reportId, "attachment" to attachmentId)),
                withAuth(mapOf("Accept" to "*/*")),
                timeoutMs = 60_000,
            )
        } catch (_: Exception) {
            return BugReportFile.failure("Could not reach ForkMesh. Check your connection and try again.")
        }
        return when (response.status) {
            in 200..299 -> BugReportFile(ok = true, data = response.body, contentType = response.header("content-type") ?: "")
            401, 403 -> BugReportFile.failure("Only ForkMesh administrators can open report attachments.")
            410 -> BugReportFile.failure("That attachment is no longer available.")
            404 -> BugReportFile.failure(
                if (errorCode(response.text()) == "attachment_missing") "That attachment is no longer available." else BUG_REPORT_INBOX_MISSING,
            )
            405 -> BugReportFile.failure(BUG_REPORT_INBOX_MISSING)
            else -> BugReportFile.failure("The attachment could not be downloaded. Try again in a moment.")
        }
    }

    open fun unreadDirectMessage(): com.forkmesh.nativeapp.push.UnreadDirectMessage {
        val data = asMap(getJson(base("/api/chat/direct-messages", linkedMapOf("limit" to "10"))))
            ?: return com.forkmesh.nativeapp.push.UnreadDirectMessage()
        return com.forkmesh.nativeapp.push.unreadDirectMessage(data["conversations"])
    }

    open fun directMessagePartner(conversation: String): String {
        val data = asMap(getJson(base("/api/chat/direct-messages", linkedMapOf("limit" to "50")))) ?: return ""
        return com.forkmesh.nativeapp.push.CallRings.partner(data["conversations"], conversation)
    }

    open fun accountAvatarPng(name: String): String {
        val key = name.trim().lowercase()
        if (key.isEmpty()) return ""
        val future: CompletableFuture<String>
        val owner: Boolean
        synchronized(accountAvatarCache) {
            val existing = accountAvatarCache[key]
            if (existing != null) {
                future = existing
                owner = false
            } else {
                future = CompletableFuture()
                accountAvatarCache[key] = future
                owner = true
            }
        }
        if (owner) {
            val value = try {
                val data = getJson(base("/api/accounts/${DartUri.encodeComponent(key)}"))
                when {
                    data !is Map<*, *> || data["exists"] == false -> ""
                    (data["kind"]?.let(::jsonText) ?: "user") != "user" -> ""
                    else -> jsonText(data["avatarPng"])
                }
            } catch (_: Exception) {
                ""
            }
            future.complete(value)
        }
        return await(future)
    }

    open fun knownAccountAvatarPng(name: String): String {
        val key = name.trim().lowercase()
        if (key.isEmpty()) return ""
        val future = synchronized(accountAvatarCache) { accountAvatarCache[key] } ?: return ""
        return if (future.isDone) future.getNow("") else ""
    }

    open fun accountUserStatus(name: String): UserStatus {
        val key = name.trim().lowercase()
        if (key.isEmpty()) return UserStatus()
        val now = clock()
        synchronized(accountStatusCache) {
            val existing = accountStatusCache[key]
            if (existing != null && (existing.permanent || now - existing.createdAt < ACCOUNT_STATUS_TTL_MS)) {
                return existing.status
            }
        }
        val entry = try {
            val data = getJson(base("/api/accounts/${DartUri.encodeComponent(key)}"))
            if (data !is Map<*, *> || data["exists"] == false || (data["kind"]?.let(::jsonText) ?: "user") != "user") {
                AccountStatusEntry(UserStatus(), now, permanent = true)
            } else {
                AccountStatusEntry(UserStatus.fromJson(data["userStatus"]), now, permanent = false)
            }
        } catch (_: Exception) {
            AccountStatusEntry(UserStatus(), now, permanent = true)
        }
        synchronized(accountStatusCache) { accountStatusCache[key] = entry }
        return entry.status
    }

    open fun repositories(): List<Repository> = cached(repositoriesCache, "", CATALOG_TTL_MS) {
        val data = getJson(base("/api/repositories"))
        val repos = collapseRepositoryMirrors(
            jsonMaps(asList(data)).map(Repository::fromJson).filter { it.owner.isNotEmpty() && it.name.isNotEmpty() },
        )
        for (repo in repos) {
            val key = "${repo.owner.lowercase()}/${repo.name.lowercase()}"
            if (repo.publicOwner.isNotEmpty() && repo.publicOwner.lowercase() != repo.owner.lowercase()) {
                publicRouteOwners[key] = repo.publicOwner
            } else {
                publicRouteOwners.remove(key)
            }
        }
        repos
    }

    open fun networkStats(): NetworkStats = cached(networkStatsCache, "", CATALOG_TTL_MS) {
        val data = asMap(getJson(base("/api/network/stats", mapOf("payouts" to "1"))))
        if (data != null && hasNetworkStatsShape(data)) {
            NetworkStats.fromJson(data)
        } else {
            throw DartException("Invalid network stats response")
        }
    }

    open fun networkLeaderboards(): NetworkLeaderboards = cached(networkLeaderboardsCache, "", CATALOG_TTL_MS) {
        val data = asMap(getJson(base("/api/network/leaderboards")))
        if (data != null && hasNetworkLeaderboardsShape(data)) {
            NetworkLeaderboards.fromJson(data)
        } else {
            throw DartException("Invalid network leaderboards response")
        }
    }

    open fun sponsoredAd(): SponsoredAd? {
        val future: CompletableFuture<SponsoredAd?>
        val owner: Boolean
        val now = clock()
        synchronized(sponsoredAdLock) {
            val existing = sponsoredAdEntry
            if (existing != null && now - existing.createdAt < sponsoredAdTtlMs) {
                future = existing.future
                owner = false
            } else {
                future = CompletableFuture()
                sponsoredAdEntry = CacheEntry(future, now)
                sponsoredAdTtlMs = SPONSORED_AD_TTL_MS
                owner = true
            }
        }
        if (owner) {
            val ad = try {
                val response = exchange(
                    "GET",
                    base("/api/ads/serve", mapOf("placement" to "mobile")),
                    mapOf("Accept" to "application/json"),
                    timeoutMs = SPONSORED_AD_TIMEOUT_MS,
                )
                if (response.status != 200) throw DartException("ads ${response.status}")
                val data = asMap(response.json())
                if (data == null || data["ok"] != true) throw DartException("ads payload")
                SponsoredAd.fromJson(data["ad"])
            } catch (_: Exception) {
                synchronized(sponsoredAdLock) { sponsoredAdTtlMs = SPONSORED_AD_FAILURE_TTL_MS }
                null
            }
            future.complete(ad)
        }
        return try {
            future.get()
        } catch (_: Exception) {
            null
        }
    }

    open fun sponsoredAdClicked(id: String) {
        if (!SponsoredAd.isValidId(id)) return
        try {
            exchange(
                "POST",
                base("/api/ads/$id/click"),
                mapOf("Accept" to "application/json", "Content-Type" to "application/json"),
                encode(mapOf("placement" to "mobile")),
                timeoutMs = SPONSORED_AD_TIMEOUT_MS,
            )
        } catch (_: Exception) {
        }
    }

    open fun listAccountDesktops(): AccountDesktops {
        if ((sessionTokenProvider?.invoke() ?: "").isEmpty()) return AccountDesktops("", emptyList())
        val data = getJson(base("/api/accounts/desktops")) as? Map<*, *> ?: return AccountDesktops("", emptyList())
        val desktops = jsonMaps(data["desktops"]).mapNotNull { item ->
            val key = jsonText(item["key"])
            val node = jsonText(item["node"]).lowercase()
            if (key.isEmpty() || node.isEmpty()) null
            else AccountDesktop(key, node, (item["lastSeen"] as? Number)?.toLong() ?: 0)
        }
        return AccountDesktops(jsonText(data["account"]), desktops)
    }

    open fun listDevices(): List<AccountDevice> {
        val data = getJson(base("/api/accounts/devices"))
        val list = when (data) {
            is Map<*, *> -> data["devices"]
            is List<*> -> data
            else -> null
        }
        if (list !is List<*>) return emptyList()
        return jsonMaps(list).map(AccountDevice::fromJson)
    }

    open fun listSessions(): List<com.forkmesh.nativeapp.settings.AccountSignIn> =
        com.forkmesh.nativeapp.settings.AccountSignIn.parse(getJson(base(com.forkmesh.nativeapp.settings.AccountSignIns.PATH)))

    open fun revokeSession(sessionId: String): Boolean =
        com.forkmesh.nativeapp.settings.AccountSignIns.currentRevoked(
            deleteJson(base(com.forkmesh.nativeapp.settings.AccountSignIns.revokePath(sessionId)), emptyMap()),
        )

    open fun updateUserStatus(payload: Map<String, Any?> = emptyMap()): UserStatus {
        val result = postJsonResult(base("/api/accounts/user-status"), payload)
        if (!result.ok) {
            val code = result.error.ifEmpty { "http_${result.statusCode}" }
            throw UserStatusException(code, userStatusErrorMessage(code))
        }
        return UserStatus.fromJson(result.body["userStatus"])
    }

    open fun updateChatRetention(payload: Map<String, Any?> = emptyMap()): ChatRetention {
        val result = postJsonResult(base("/api/accounts/chat-retention"), payload)
        if (!result.ok) {
            val code = result.error.ifEmpty { "http_${result.statusCode}" }
            throw ChatRetentionException(code, chatRetentionErrorMessage(code))
        }
        return ChatRetention.fromJson(result.body["chatRetention"])
    }

    open fun syncActivityToday(payload: Map<String, Any?>): ApiJsonResult =
        postJsonResult(base("/api/accounts/activity-today"), payload)

    open fun roomChatPassphrase(): String {
        val data = getJson(
            base(
                "/api/chat/room-key",
                linkedMapOf("owner" to "mainnode", "repo" to "forkmesh", "room" to settings.room),
            ),
        )
        if (data is Map<*, *>) {
            val passphrase = data["passphrase"]
            if (passphrase is String) return passphrase
        }
        throw DartException("room key unavailable")
    }

    open fun notifications(node: String, limit: Int = 40): NotificationPage {
        val cleanNode = node.trim().lowercase()
        if (cleanNode.isEmpty()) return NotificationPage(notifications = emptyList(), unread = 0)
        val data = getJson(base("/api/notifications", linkedMapOf("node" to cleanNode, "limit" to "$limit")))
        return NotificationPage.fromJson(asMap(data) ?: emptyMap())
    }

    open fun markNotificationsRead(node: String, ids: List<String> = emptyList(), all: Boolean = false) {
        val cleanNode = node.trim().lowercase()
        if (cleanNode.isEmpty()) return
        val payload = LinkedHashMap<String, Any?>()
        payload["node"] = cleanNode
        if (all) payload["all"] = true
        if (!all && ids.isNotEmpty()) payload["ids"] = ids
        postJson(base("/api/notifications"), payload)
    }

    open fun deleteAllNotifications(node: String) {
        val cleanNode = node.trim().lowercase()
        if (cleanNode.isEmpty()) return
        deleteJson(base("/api/notifications"), linkedMapOf("node" to cleanNode, "all" to true))
    }

    open fun deleteNotification(node: String, id: String) {
        val cleanNode = node.trim().lowercase()
        val cleanId = id.trim().lowercase()
        if (cleanNode.isEmpty() || cleanId.isEmpty()) return
        deleteJson(base("/api/notifications"), linkedMapOf("node" to cleanNode, "id" to cleanId))
    }

    open fun pingSettings(node: String): PingSettings {
        val cleanNode = node.trim().lowercase()
        if (cleanNode.isEmpty()) throw DartException("Sign in to manage your pings.")
        val data = getJson(base("/api/notifications", linkedMapOf("node" to cleanNode, "settings" to "1")))
        return PingSettings.fromJson(asMap(data) ?: emptyMap())
    }

    open fun savePingSettings(node: String, patch: Map<String, Any?>): PingSettings {
        val cleanNode = node.trim().lowercase()
        if (cleanNode.isEmpty()) throw DartException("Sign in to manage your pings.")
        val payload = LinkedHashMap<String, Any?>()
        payload["node"] = cleanNode
        payload["pingSettings"] = patch
        val result = postJsonResult(base("/api/notifications"), payload)
        if (!result.ok) {
            throw DartException(
                when {
                    result.statusCode == 401 -> "Your session expired. Please sign in again."
                    result.statusCode == 403 || result.error == "forbidden" ->
                        "Only administrators can change platform alerts."
                    result.error.isNotEmpty() -> "Could not save ping settings (${result.error})."
                    else -> "Could not save ping settings (HTTP ${result.statusCode})."
                },
            )
        }
        return PingSettings.fromJson(result.body)
    }

    private fun orgSeg(value: String): String = DartUri.encodeComponent(value.trim().lowercase())

    private fun orgRequest(method: String, target: Target, body: Map<String, Any?>? = null): Any? {
        val headers = LinkedHashMap<String, String>()
        headers["Accept"] = "application/json"
        if (body != null) headers["Content-Type"] = "application/json"
        val response = exchange(method, target, withAuth(headers), body?.let(::encode), timeoutMs = 12_000)
        var decoded: Any? = null
        val text = response.text()
        if (text.isNotEmpty()) {
            try {
                decoded = Json.parse(text)
            } catch (_: Json.JsonException) {
            }
        }
        if (response.status in 200..299) return decoded ?: emptyMap<String, Any?>()
        val code = ((decoded as? Map<*, *>)?.get("error") as? String) ?: ""
        throw OrgApiException(if (code.isNotEmpty()) code else "http_${response.status}", orgErrorMessage(code))
    }

    open fun accountApiCall(method: String, path: String, body: Map<String, Any?>? = null): Map<String, Any?> {
        val headers = LinkedHashMap<String, String>()
        headers["Accept"] = "application/json"
        if (method != "GET") headers["Content-Type"] = "application/json"
        val target = base(path.substringBefore('?'), path.substringAfter('?', "").takeIf { it.isNotEmpty() }?.let(::splitQuery))
        val payload = if (method == "GET") null else encode(body ?: emptyMap<String, Any?>())
        val response = exchange(method, target, withAuth(headers), payload, timeoutMs = 20_000)
        var decoded: Any? = null
        val text = response.text()
        if (text.isNotEmpty()) {
            try {
                decoded = Json.parse(text)
            } catch (_: Json.JsonException) {
            }
        }
        @Suppress("UNCHECKED_CAST")
        val map = decoded as? Map<String, Any?> ?: emptyMap()
        if (response.status in 200..299 && map["ok"] != false) return map
        val code = map["error"] as? String ?: ""
        throw OrgApiException(
            code.ifEmpty { "http_${response.status}" },
            if (code.isNotEmpty()) code.replace('_', ' ') else "The server answered ${response.status}.",
        )
    }

    open fun jsonExchange(
        method: String,
        path: String,
        query: List<Pair<String, String>> = emptyList(),
        body: Map<String, Any?>? = null,
        timeoutMs: Int = 20_000,
        extraHeaders: Map<String, String> = emptyMap(),
    ): ApiJsonResult {
        val headers = LinkedHashMap<String, String>()
        headers["Accept"] = "application/json"
        if (body != null) headers["Content-Type"] = "application/json"
        headers.putAll(extraHeaders)
        val target = base(path, if (query.isEmpty()) null else LinkedHashMap<String, String>().apply { query.forEach { put(it.first, it.second) } })
        val response = exchange(method, target, withAuth(headers), body?.let(::encode), timeoutMs = timeoutMs)
        val text = response.text()
        val map: Map<String, Any?> = try {
            @Suppress("UNCHECKED_CAST")
            (if (text.isEmpty()) null else Json.parse(text)) as? Map<String, Any?> ?: emptyMap()
        } catch (_: Json.JsonException) {
            emptyMap()
        }
        if (response.status == 401 && sessionToken().isNotEmpty() && map.str("error") != "bad_signature" && endsSession(target.path, map.str("error"))) {
            onSessionInvalid?.invoke(map.str("error"))
            onUnauthorized?.invoke()
        }
        return ApiJsonResult(response.status, map)
    }

    private fun splitQuery(query: String): Map<String, String> {
        val out = LinkedHashMap<String, String>()
        for (part in query.split('&')) {
            if (part.isEmpty()) continue
            val key = java.net.URLDecoder.decode(part.substringBefore('='), "UTF-8")
            val value = java.net.URLDecoder.decode(part.substringAfter('=', ""), "UTF-8")
            out[key] = value
        }
        return out
    }

    private fun orgList(data: Any?, key: String): List<Map<String, Any?>> {
        val list = (data as? Map<*, *>)?.get(key) as? List<*> ?: return emptyList()
        return jsonMaps(list)
    }

    open fun myOrgs(): List<OrgSummary> =
        orgList(orgRequest("GET", base("/api/orgs")), "orgs").map(OrgSummary::fromJson).filter { it.name.isNotEmpty() }

    open fun createOrg(name: String, displayName: String = "", description: String = ""): OrgSummary {
        val payload = LinkedHashMap<String, Any?>()
        payload["name"] = name.trim().lowercase()
        if (displayName.trim().isNotEmpty()) payload["displayName"] = displayName.trim()
        if (description.trim().isNotEmpty()) payload["description"] = description.trim()
        val map = asMap(orgRequest("POST", base("/api/orgs"), payload)) ?: emptyMap()
        return OrgSummary(
            name = map["org"]?.let(::jsonText) ?: name,
            role = map["role"]?.let(::jsonText) ?: "owner",
        )
    }

    open fun orgProfile(org: String): OrgProfile =
        OrgProfile.fromJson(asMap(orgRequest("GET", base("/api/orgs/${orgSeg(org)}"))) ?: emptyMap())

    open fun deleteOrg(org: String) {
        orgRequest("DELETE", base("/api/orgs/${orgSeg(org)}"), emptyMap())
    }

    open fun orgMembers(org: String): List<OrgMember> =
        orgList(orgRequest("GET", base("/api/orgs/${orgSeg(org)}/members")), "members")
            .map(OrgMember::fromJson).filter { it.name.isNotEmpty() }

    open fun setOrgMember(org: String, member: String, role: String = "member") {
        orgRequest(
            "POST",
            base("/api/orgs/${orgSeg(org)}/members"),
            linkedMapOf("member" to member.trim().lowercase(), "role" to role.trim().lowercase()),
        )
    }

    open fun removeOrgMember(org: String, member: String) {
        orgRequest("DELETE", base("/api/orgs/${orgSeg(org)}/members"), linkedMapOf("member" to member.trim().lowercase()))
    }

    open fun orgTeams(org: String): List<OrgTeam> =
        orgList(orgRequest("GET", base("/api/orgs/${orgSeg(org)}/teams")), "teams")
            .map(OrgTeam::fromJson).filter { it.team.isNotEmpty() }

    open fun setOrgTeam(org: String, team: String, permission: String = "read") {
        orgRequest(
            "POST",
            base("/api/orgs/${orgSeg(org)}/teams"),
            linkedMapOf("team" to team.trim().lowercase(), "permission" to permission.trim().lowercase()),
        )
    }

    open fun deleteOrgTeam(org: String, team: String) {
        orgRequest("DELETE", base("/api/orgs/${orgSeg(org)}/teams"), linkedMapOf("team" to team.trim().lowercase()))
    }

    open fun orgTeamMembers(org: String, team: String): List<OrgMember> =
        orgList(orgRequest("GET", base("/api/orgs/${orgSeg(org)}/teams/${orgSeg(team)}/members")), "members")
            .map(OrgMember::fromJson).filter { it.name.isNotEmpty() }

    open fun addOrgTeamMember(org: String, team: String, member: String) {
        orgRequest(
            "POST",
            base("/api/orgs/${orgSeg(org)}/teams/${orgSeg(team)}/members"),
            linkedMapOf("member" to member.trim().lowercase()),
        )
    }

    open fun removeOrgTeamMember(org: String, team: String, member: String) {
        orgRequest(
            "DELETE",
            base("/api/orgs/${orgSeg(org)}/teams/${orgSeg(team)}/members"),
            linkedMapOf("member" to member.trim().lowercase()),
        )
    }

    open fun orgRepos(org: String): List<OrgRepo> =
        orgList(orgRequest("GET", base("/api/orgs/${orgSeg(org)}/repos")), "repos")
            .map(OrgRepo::fromJson).filter { it.repo.isNotEmpty() }

    open fun linkOrgRepo(org: String, repo: String, node: String = "") {
        val payload = LinkedHashMap<String, Any?>()
        payload["repo"] = repo.trim().lowercase()
        if (node.trim().isNotEmpty()) payload["node"] = node.trim().lowercase()
        orgRequest("POST", base("/api/orgs/${orgSeg(org)}/repos"), payload)
    }

    open fun unlinkOrgRepo(org: String, repo: String) {
        orgRequest("DELETE", base("/api/orgs/${orgSeg(org)}/repos"), linkedMapOf("repo" to repo.trim().lowercase()))
    }

    open fun registerOwnerKey(owner: OwnerKeys): String = try {
        val data = postJson(base("/api/security/owner-keys"), linkedMapOf("publicBundle" to owner.publicBundle))
        if (data !is Map<*, *> || data["ok"] != true) {
            ""
        } else if (data["keyId"] != owner.keyId || data["privateKeysStored"] == true) {
            ""
        } else {
            owner.keyId
        }
    } catch (_: Exception) {
        ""
    }

    open fun syncOwnerEnrollment(): Boolean {
        val owner = ownerKeys ?: return false
        synchronized(enrollmentLock) {
            val token = sessionToken()
            if (token.isEmpty()) {
                enrolledFor = ""
                return false
            }
            if (enrolledFor == token) return true
            val keyId = registerOwnerKey(owner)
            if (keyId.isNotEmpty()) enrolledFor = token
            return keyId.isNotEmpty()
        }
    }

    private fun ownerAccountPayload(ownerAccount: String): LinkedHashMap<String, Any?> {
        val payload = LinkedHashMap<String, Any?>()
        if (ownerAccount.trim().isNotEmpty()) payload["ownerAccount"] = ownerAccount.trim().lowercase()
        return payload
    }

    open fun agentSessionPage(owner: String, name: String, ownerAccount: String = ""): AgentSessionPage {
        val data = postJson(base("/api/repo/$owner/$name/agents/list"), ownerAccountPayload(ownerAccount))
        val sessions = ArrayList<AgentSession>()
        var sealedForOthers = 0
        val raw = when (data) {
            is Map<*, *> -> asList(data)
            is List<*> -> data
            else -> emptyList()
        }
        for (item in jsonMaps(raw)) sessions.add(AgentSession.fromJson(item))
        if (data is Map<*, *>) {
            val encrypted = data["encryptedAgents"]
            if (encrypted is List<*>) {
                val keys = ownerKeys
                for (entry in jsonMaps(encrypted)) {
                    val envelope = asMap(entry["envelope"] as? Map<*, *>) ?: continue
                    val opened = keys?.openJson(envelope)
                    if (opened == null) {
                        sealedForOthers++
                        continue
                    }
                    sessions.add(AgentSession.fromJson(opened))
                }
            }
            val legacyHeld = data["legacyRowsHeldForOwnerMigration"]
            if (legacyHeld is Number && legacyHeld.toDouble() > 0) {
                sealedForOthers += legacyHeld.toDouble().toInt()
            }
        }
        return AgentSessionPage(sessions = sessions, sealedForOthers = sealedForOthers)
    }

    open fun agentSessions(owner: String, name: String, ownerAccount: String = ""): List<AgentSession> =
        agentSessionPage(owner, name, ownerAccount).sessions

    open fun agentTranscript(owner: String, name: String, id: Int, ownerAccount: String = ""): AgentTranscript {
        val data = asMap(postJson(base("/api/repo/$owner/$name/agents/$id/transcript"), ownerAccountPayload(ownerAccount)))
            ?: return AgentTranscript.fromJson(emptyMap())
        if (data["encrypted"] == true) {
            val envelope = asMap(data["envelope"] as? Map<*, *>)
            val keys = ownerKeys
            if (envelope != null && keys != null) {
                val opened = keys.openJson(envelope)
                if (opened != null) return AgentTranscript.fromJson(opened)
            }
            return AgentTranscript(status = "", transcript = "", sealedForOtherDevice = true)
        }
        return AgentTranscript.fromJson(data)
    }

    private fun signedOwnerPayload(ownerAccount: String, ts: String, sig: String): LinkedHashMap<String, Any?> {
        val payload = ownerAccountPayload(ownerAccount)
        if (ts.trim().isNotEmpty()) payload["ts"] = ts.trim()
        if (sig.trim().isNotEmpty()) payload["sig"] = sig.trim()
        return payload
    }

    open fun repoActions(owner: String, name: String, ownerAccount: String = "", ts: String = "", sig: String = ""): RepoActions {
        val data = postJson(base("/api/repo/$owner/$name/actions/list"), signedOwnerPayload(ownerAccount, ts, sig))
        return RepoActions.fromJson(asMap(data) ?: emptyMap())
    }

    open fun actionLog(owner: String, name: String, id: Int, ownerAccount: String = "", ts: String = "", sig: String = ""): ActionLog {
        val data = postJson(base("/api/repo/$owner/$name/actions/$id/log"), signedOwnerPayload(ownerAccount, ts, sig))
        return ActionLog.fromJson(asMap(data) ?: emptyMap())
    }

    open fun desktopCommand(
        owner: String,
        name: String,
        ownerAccount: String,
        command: String,
        target: String,
        ts: String,
        sig: String,
        payload: Map<String, Any?> = emptyMap(),
    ): DesktopCommandResult {
        val body = linkedMapOf(
            "ownerAccount" to ownerAccount.trim().lowercase(),
            "command" to command.trim(),
            "target" to target.trim(),
            "ts" to ts.trim(),
            "sig" to sig.trim(),
            "payload" to payload,
        )
        val (status, map) = postUnauthenticated(DesktopCommandContract.path(owner, name), body)
        if (status in 200..299) return DesktopCommandResult.fromJson(map)
        throw DartException(DesktopCommandContract.errorMessage(status, map.str("error")))
    }

    open fun desktopCommandRouteAnswered(owner: String, name: String): Boolean? = try {
        val (status, map) = postUnauthenticated(DesktopCommandContract.path(owner, name), emptyMap())
        DesktopCommandContract.routeAnswered(status, map.str("error"))
    } catch (_: Exception) {
        null
    }

    private fun postUnauthenticated(path: String, body: Map<String, Any?>): Pair<Int, Map<String, Any?>> {
        val response = exchange(
            "POST",
            base(path),
            mapOf("Accept" to "application/json", "Content-Type" to "application/json"),
            encode(body),
            timeoutMs = 12_000,
        )
        val text = response.text()
        val map: Map<String, Any?> = try {
            @Suppress("UNCHECKED_CAST")
            (if (text.isEmpty()) null else Json.parse(text)) as? Map<String, Any?> ?: emptyMap()
        } catch (_: Json.JsonException) {
            emptyMap()
        }
        return response.status to map
    }

    open fun issueBountyStatus(owner: String, repo: String, number: Int): IssueBounty = try {
        val data = postJson(base("/api/repo/$owner/$repo/bounty"), linkedMapOf("action" to "status", "number" to number))
        IssueBounty.fromJson(data)
    } catch (error: Exception) {
        if (error.toString().contains("HTTP 404")) IssueBounty() else throw error
    }

    @Suppress("UNUSED_PARAMETER")
    open fun createIssueBounty(
        owner: String,
        repo: String,
        number: Int,
        amountUsd: Double,
        payee: String = "",
        payeeNode: String = "",
        ts: String,
        sig: String,
    ): IssueBounty = throw UnsupportedOperationException(
        "Worker-held issue-bounty wallets are retired. Use an external " +
            "self-custodial wallet, reviewed program, or multisig.",
    )

    @Suppress("UNUSED_PARAMETER")
    open fun bountyWallet(owner: String, repo: String, ts: String, sig: String): BountyWallet =
        throw UnsupportedOperationException(
            "Worker-held bounty wallets are retired and cannot be created or " +
                "prepared by this client.",
        )

    open fun issues(owner: String, name: String): List<Issue> =
        jsonMaps(asList(getJson(base("/api/repo/$owner/$name/issues")))).map(Issue::fromJson)

    open fun pulls(owner: String, name: String): List<PullRequest> =
        jsonMaps(asList(getJson(base("/api/repo/$owner/$name/pulls")))).map(PullRequest::fromJson)

    open fun federatedReplies(owner: String, name: String, kind: String, number: Int): List<FederatedReply> {
        if (kind !in setOf("issue", "pull", "discussion") || number <= 0) return emptyList()
        val data = getJson(
            base("/api/repo/$owner/$name/fedi-comments", linkedMapOf("kind" to kind, "number" to "$number")),
        )
        val raw = when {
            data is Map<*, *> && data["comments"] is List<*> -> data["comments"] as List<*>
            data is Map<*, *> && data["items"] is List<*> -> data["items"] as List<*>
            else -> emptyList<Any?>()
        }
        return jsonMaps(raw).map(FederatedReply::fromJson).filter { it.remoteId.isNotEmpty() || it.backlink.isNotEmpty() }
    }

    open fun commits(owner: String, name: String): List<Map<String, Any?>> =
        cached(commitsCache, "$owner/$name", CACHE_TTL_MS) {
            jsonMaps(asList(getRepoRead(owner, name, "history")))
        }

    open fun history(owner: String, name: String, ref: String): com.forkmesh.nativeapp.code.RepoHistoryRead =
        com.forkmesh.nativeapp.code.RepoHistoryRead.fromJson(
            getRepoRead(owner, name, "history", if (ref.isEmpty()) null else mapOf("ref" to ref)),
        )

    open fun commitDetail(
        owner: String,
        name: String,
        hash: String,
        fallbackCommit: Map<String, Any?> = emptyMap(),
    ): RepoCommitDetail {
        val data = getRepoRead(owner, name, "commit", mapOf("path" to hash))
        return RepoCommitDetail.fromJson(data, fallbackCommit = fallbackCommit)
    }

    open fun tree(owner: String, name: String, path: String = "", ref: String = ""): RepoTree {
        val repoKey = "$owner/$name"
        repoOfflineError(repoKey)?.let { throw it }
        return cached(treeCache, "$repoKey:$ref:$path", CACHE_TTL_MS) {
            val query = LinkedHashMap<String, String>()
            query["path"] = path
            if (ref.isNotEmpty()) query["ref"] = ref
            RepoTree.fromJson(getRepoRead(owner, name, "tree", query), path = path)
        }
    }

    open fun blob(owner: String, name: String, path: String, ref: String = ""): RepoBlob {
        val repoKey = "$owner/$name"
        repoOfflineError(repoKey)?.let { throw it }
        return cached(blobCache, "$repoKey:$ref:$path", CACHE_TTL_MS) {
            val query = LinkedHashMap<String, String>()
            query["path"] = path
            if (ref.isNotEmpty()) query["ref"] = ref
            RepoBlob.fromJson(getRepoRead(owner, name, "blob", query), path = path)
        }
    }

    open fun blobs(owner: String, name: String, paths: List<String>, ref: String = ""): Map<String, RepoBlob> {
        val repoKey = "$owner/$name"
        if (paths.isEmpty() || repoOfflineError(repoKey) != null) return emptyMap()
        val found = LinkedHashMap<String, RepoBlob>()
        for (chunk in paths.chunked(BLOB_BATCH)) {
            val data = try {
                getRepoRead(owner, name, "blobs", if (ref.isEmpty()) null else mapOf("ref" to ref), chunk.map { "path" to it })
            } catch (cancelled: InterruptedException) {
                throw cancelled
            } catch (_: Exception) {
                null
            }
            val map = (data as? Map<*, *>)?.get("blobs") as? Map<*, *> ?: continue
            for (path in chunk) {
                val value = map[path]
                if (value !is Map<*, *> && value !is String) continue
                val blob = RepoBlob.fromJson(value, path = path)
                found[path] = blob
                synchronized(blobCache) {
                    blobCache["$repoKey:$ref:$path"] = CacheEntry(CompletableFuture.completedFuture(blob), clock())
                }
            }
        }
        val missing = paths.filter { it !in found }
        val retried = parallelMap(missing, BLOB_FALLBACK_WIDTH) { path ->
            try {
                blob(owner, name, path, ref)
            } catch (cancelled: InterruptedException) {
                throw cancelled
            } catch (_: Exception) {
                null
            }
        }
        missing.zip(retried).forEach { (path, blob) -> if (blob != null) found[path] = blob }
        return found
    }

    open fun compare(owner: String, name: String, base: String, head: String): com.forkmesh.nativeapp.model.RepoComparison {
        val query = LinkedHashMap<String, String>()
        query["base"] = base
        query["head"] = head
        return com.forkmesh.nativeapp.model.RepoComparison.fromJson(getRepoRead(owner, name, "compare", query))
    }

    open fun rawBytes(owner: String, name: String, path: String, ref: String = "", maxBytes: Int = 1024 * 1024): ByteArray {
        val query = LinkedHashMap<String, String>()
        query["path"] = path
        if (ref.isNotEmpty()) query["ref"] = ref
        val response = exchange("GET", base("/api/repo/${readOwner(owner, name)}/$name/raw", query), withAuth(mapOf("Accept" to "*/*")), timeoutMs = 12_000)
        if (response.status != 200) throw DartException("Could not read $path (HTTP ${response.status}).")
        if (response.body.size > maxBytes) throw DartException("$path is too large to edit on this device.")
        return response.body
    }

    open fun branches(owner: String, name: String): List<RepoBranch> =
        jsonMaps(asList(getRepoRead(owner, name, "branches")))
            .map(RepoBranch::fromJson).filter { it.name.isNotEmpty() }

    open fun mirrors(owner: String, name: String): List<RepoMirror> = mirrorSet(owner, name).mirrors

    open fun mirrorSet(owner: String, name: String): RepoMirrorSet {
        val data = getJson(base("/api/repo/$owner/$name/mirrors"))
        return RepoMirrorSet(
            mirrors = jsonMaps(asList(data)).map(RepoMirror::fromJson).filter { it.label.isNotEmpty() },
            syncStatus = asMap(data as? Map<*, *>)?.let(::mirrorsPayloadSyncStatus),
        )
    }

    open fun releases(owner: String, name: String): List<RepoRelease> =
        cached(releasesCache, "$owner/$name", CACHE_TTL_MS) {
            val downloads = releaseDownloadCounts(owner, name)
            val treeRoot = try {
                val data = getRepoRead(owner, name, "tree", mapOf("path" to ".forkmesh/releases"))
                RepoTree.fromJson(data, path = ".forkmesh/releases")
            } catch (_: Exception) {
                return@cached emptyList()
            }
            val dirs = treeRoot.entries.filter { it.isDirectory }.sortedWith { a, b -> a.name.compareTo(b.name) }
            val paths = dirs.map { ".forkmesh/releases/${it.name}/release.json" }
            val blobs = blobs(owner, name, paths)
            val items = paths.map { path ->
                try {
                    val blob = blobs[path] ?: return@map null
                    val decoded = asMap(Json.parse(blob.content) as? Map<*, *>)
                    decoded?.let { RepoRelease.fromJson(it, downloads = downloads) }
                } catch (_: Exception) {
                    null
                }
            }
            items.filterNotNull().sortedWith { a, b ->
                val byTime = b.createdAtMs.compareTo(a.createdAtMs)
                if (byTime != 0) byTime else b.tag.compareTo(a.tag)
            }
        }

    private fun releaseDownloadCounts(owner: String, name: String): Map<String, Int> = try {
        val data = getJson(base("/api/repo/$owner/$name/releases/downloads"))
        val raw: Map<*, *> = when {
            data is Map<*, *> && data["counts"] is Map<*, *> -> data["counts"] as Map<*, *>
            data is Map<*, *> -> data
            else -> emptyMap<String, Any?>()
        }
        raw.entries.associate { (key, value) -> jsonText(key) to modelInt(value).toInt() }
    } catch (_: Exception) {
        offlineRepoUntil.remove("$owner/$name")
        emptyMap()
    }

    open fun searchRepo(owner: String, name: String, query: String, ref: String = ""): RepoSearchResults {
        val params = LinkedHashMap<String, String>()
        params["q"] = query
        if (ref.isNotEmpty()) params["ref"] = ref
        return RepoSearchResults.fromJson(getRepoRead(owner, name, "search", params))
    }

    open fun publishedIssues(owner: String, name: String): List<Issue> =
        cached(issuesCache, "$owner/$name", CACHE_TTL_MS) {
            val dirByNumber = LinkedHashMap<String, String>()
            for (dir in numberedFolders(owner, name, ".forkmesh/issues")) {
                dirByNumber[dir] = ".forkmesh/issues/$dir"
            }
            for (sub in listOf("open", "closed")) {
                for (dir in numberedFolders(owner, name, ".forkmesh/issues/$sub")) {
                    dirByNumber[dir] = ".forkmesh/issues/$sub/$dir"
                }
            }
            if (dirByNumber.isEmpty()) return@cached legacyMarkdownIssues(owner, name)
            val paths = dirByNumber.entries.map { it.key to "${it.value}/issue-${it.key}.json" }
            val blobs = blobs(owner, name, paths.map { it.second })
            val items = paths.map { (number, path) ->
                try {
                    blobs[path]?.let { issueFromRecordJson(number, it.content) }
                } catch (_: Exception) {
                    null
                }
            }
            items.filterNotNull().sortedByDescending { it.number }
        }

    private fun legacyMarkdownIssues(owner: String, name: String): List<Issue> {
        val dirs = numberedFolders(owner, name, "issues")
        val blobs = blobs(owner, name, dirs.map { "issues/$it/issue.md" })
        val items = parallelMap(dirs, RECORD_WIDTH) { dir ->
            try {
                val blob = blobs["issues/$dir/issue.md"] ?: return@parallelMap null
                val events = issueEvents(owner, name, dir)
                issueFromMarkdown(dir, blob.content, events)
            } catch (_: Exception) {
                null
            }
        }
        return items.filterNotNull().sortedByDescending { it.number }
    }

    private fun issueFromRecordJson(number: String, text: String): Issue? {
        val decoded = asMap(Json.parse(text) as? Map<*, *>) ?: return null
        var open: Map<String, Any?>? = null
        val events = decoded["events"]
        if (events is List<*>) {
            for (event in jsonMaps(events)) {
                if (event["type"] == "open") {
                    open = event
                    break
                }
            }
        }
        var openBody = open?.get("body")
        if (events is List<*> && open != null && open.str("id").isNotEmpty()) {
            for (event in jsonMaps(events)) {
                if (event["type"] == "edit" && event["target"] == open["id"]) openBody = event["body"]
            }
        }
        val merged = LinkedHashMap(decoded)
        merged["number"] = decoded["number"] ?: dartIntTryParse(number) ?: 0L
        merged["title"] = decoded["title"] ?: open?.get("title") ?: "Issue #$number"
        merged["body"] = decoded["body"] ?: openBody ?: ""
        merged["author"] = decoded["authorName"] ?: open?.get("authorName") ?: decoded["author"] ?: ""
        return Issue.fromJson(merged)
    }

    private fun issueEvents(owner: String, name: String, number: String): List<IssueEvent> = try {
        val tree = tree(owner, name, path = "issues/$number")
        val files = tree.entries
            .filter { !it.isDirectory && it.name.endsWith(".md") && it.name != "issue.md" }
            .sortedWith { a, b -> a.name.compareTo(b.name) }
        val blobs = blobs(owner, name, files.map { it.path })
        files.mapNotNull { file -> blobs[file.path]?.let { issueEventFromMarkdown(it.content) } }
    } catch (_: Exception) {
        emptyList()
    }

    open fun publishedPulls(owner: String, name: String): List<PublishedPull> =
        cached(pullsCache, "$owner/$name", CACHE_TTL_MS) {
            val ledger = try {
                tree(owner, name, path = ".forkmesh/pulls", ref = PULL_LEDGER_REF)
            } catch (_: Exception) {
                null
            }
            val dirs = ledger?.entries.orEmpty()
                .filter { it.isDirectory && dartIntTryParse(it.name) != null }
                .map { it.name }
            val indexedNumbers = dirs.mapNotNull { dartIntTryParse(it) }.toSet()
            val branchOnly = ledger?.pullRefs.orEmpty()
                .filterKeys { dartIntTryParse(it) !in indexedNumbers }
            val blobs = blobs(owner, name, dirs.map { ".forkmesh/pulls/$it/pull.md" }, ref = PULL_LEDGER_REF)
            val sources = dirs.map { it to PULL_LEDGER_REF } + branchOnly.map { (number, commit) -> number to commit }
            val items = parallelMap(sources, RECORD_WIDTH) { (dir, ref) ->
                try {
                    if (ref != PULL_LEDGER_REF) return@parallelMap readPull(owner, name, dir, ref)
                    val blob = blobs[".forkmesh/pulls/$dir/pull.md"] ?: return@parallelMap null
                    pullFromMarkdown(dir, blob.content, pullEvents(owner, name, dir, ref))
                } catch (_: Exception) {
                    null
                }
            }
            items.filterNotNull().sortedByDescending { it.number }
        }

    open fun publishedPull(owner: String, name: String, number: Int): PublishedPull? {
        val repoKey = "$owner/$name"
        cachedPull(repoKey, number)?.let { return it }
        repoOfflineError(repoKey)?.let { throw it }
        for (ref in listOf(PULL_LEDGER_REF, "forkmesh/pull/$number")) {
            val pull = try {
                readPull(owner, name, number.toString(), ref)
            } catch (cancelled: InterruptedException) {
                throw cancelled
            } catch (error: Exception) {
                if (repoOfflineError(repoKey) != null) throw error
                null
            } ?: continue
            synchronized(pullsCache) { pullsCache.remove(repoKey) }
            synchronized(treeCache) { treeCache.remove("$repoKey:$PULL_LEDGER_REF:.forkmesh/pulls") }
            return pull
        }
        return null
    }

    private fun cachedPull(repoKey: String, number: Int): PublishedPull? {
        val entry = synchronized(pullsCache) { pullsCache[repoKey] } ?: return null
        if (clock() - entry.createdAt >= CACHE_TTL_MS) return null
        if (!entry.future.isDone || entry.future.isCompletedExceptionally) return null
        return entry.future.getNow(null)?.firstOrNull { it.number == number }
    }

    private fun readPull(owner: String, name: String, number: String, ref: String): PublishedPull {
        val blob = blob(owner, name, ".forkmesh/pulls/$number/pull.md", ref = ref)
        return pullFromMarkdown(number, blob.content, pullEvents(owner, name, number, ref))
    }

    private fun pullEvents(owner: String, name: String, number: String, ref: String): List<PullEvent> = try {
        val tree = tree(owner, name, path = ".forkmesh/pulls/$number", ref = ref)
        val files = tree.entries
            .filter { !it.isDirectory && it.name.endsWith(".md") && it.name != "pull.md" }
            .sortedWith { a, b -> a.name.compareTo(b.name) }
        val blobs = blobs(owner, name, files.map { it.path }, ref = ref)
        files.mapNotNull { file -> blobs[file.path]?.let { pullEventFromMarkdown(it.content) } }
    } catch (_: Exception) {
        emptyList()
    }

    open fun publishedDiscussions(owner: String, name: String): List<RepoDiscussion> =
        cached(discussionsCache, "$owner/$name", CACHE_TTL_MS) {
            val dirs = numberedFolders(owner, name, ".forkmesh/discussions")
            val blobs = blobs(owner, name, dirs.map { ".forkmesh/discussions/$it/discussion.md" })
            val items = parallelMap(dirs, RECORD_WIDTH) { dir ->
                try {
                    val blob = blobs[".forkmesh/discussions/$dir/discussion.md"] ?: return@parallelMap null
                    val events = discussionEvents(owner, name, dir)
                    discussionFromMarkdown(dir, blob.content, events)
                } catch (_: Exception) {
                    null
                }
            }
            items.filterNotNull().sortedByDescending { it.number }
        }

    private fun discussionEvents(owner: String, name: String, number: String): List<DiscussionEvent> = try {
        val tree = tree(owner, name, path = ".forkmesh/discussions/$number")
        val files = tree.entries
            .filter { !it.isDirectory && it.name.endsWith(".md") && it.name != "discussion.md" }
            .sortedWith { a, b -> a.name.compareTo(b.name) }
        val blobs = blobs(owner, name, files.map { it.path })
        files.mapNotNull { file -> blobs[file.path]?.let { discussionEventFromMarkdown(it.content) } }
    } catch (_: Exception) {
        emptyList()
    }

    private fun numberedFolders(owner: String, name: String, path: String, ref: String = ""): List<String> = try {
        tree(owner, name, path = path, ref = ref).entries
            .filter { it.isDirectory && dartIntTryParse(it.name) != null }
            .map { it.name }
    } catch (_: Exception) {
        emptyList()
    }

    private fun issueFromMarkdown(number: String, md: String, events: List<IssueEvent>): Issue {
        val meta = PublishedMarkdown.frontMatter(md)
        fun metaList(key: String): List<String> = (meta[key] ?: "")
            .replace("[", "")
            .replace("]", "")
            .split(',')
            .map { it.trim() }
            .filter { it.isNotEmpty() }

        val title = meta["title"] ?: PublishedMarkdown.firstHeading(md) ?: "Issue #$number"
        val status = meta["status"] ?: if (md.contains("status: closed")) "closed" else "open"
        return Issue(
            number = dartIntTryParse(number)?.toInt() ?: 0,
            title = title,
            status = status,
            body = PublishedMarkdown.bodyWithoutFrontMatter(md),
            author = meta["authorName"] ?: meta["author"] ?: "",
            labels = metaList("labels"),
            milestone = meta["milestone"] ?: "",
            priority = dartIntTryParse(meta["priority"] ?: "")?.toInt() ?: 0,
            assignees = metaList("assignees"),
            events = events,
            votes = events.count { it.type == "vote" },
            bountyUsd = dartDoubleTryParse(meta["bountyUsd"] ?: "") ?: 0.0,
            bountyAddress = meta["bountyAddress"] ?: meta["address"] ?: "",
            bountyStatus = meta["bountyStatus"] ?: "",
            bountyRequiredLamports = dartIntTryParse(meta["bountyRequiredLamports"] ?: "") ?: 0L,
            bountyReceivedLamports = dartIntTryParse(meta["bountyReceivedLamports"] ?: "") ?: 0L,
            bountyAmountSol = dartDoubleTryParse(meta["bountyAmountSol"] ?: "") ?: 0.0,
            bountyPayUri = meta["bountyPayUri"] ?: meta["payUri"] ?: meta["uri"] ?: "",
            bountyPayee = meta["bountyPayee"] ?: meta["payee"] ?: "",
            bountyPayoutSig = meta["bountyPayoutSig"] ?: meta["payoutSig"] ?: "",
        )
    }

    private fun issueEventFromMarkdown(md: String): IssueEvent {
        val meta = PublishedMarkdown.frontMatter(md)
        return IssueEvent(
            type = meta["type"] ?: "comment",
            body = PublishedMarkdown.bodyWithoutFrontMatter(md),
            author = meta["author"] ?: "",
            authorName = meta["authorName"] ?: "",
            status = meta["status"] ?: "",
            ts = dartIntTryParse(meta["ts"] ?: "") ?: 0L,
        )
    }

    private fun pullFromMarkdown(number: String, md: String, events: List<PullEvent>): PublishedPull {
        val meta = PublishedMarkdown.frontMatter(md)
        val body = PublishedMarkdown.bodyWithoutFrontMatter(md)
        val title = meta["title"] ?: PublishedMarkdown.firstHeading(md) ?: "Pull request #$number"
        return PublishedPull(
            number = dartIntTryParse(number)?.toInt() ?: 0,
            title = title,
            status = meta["status"] ?: "open",
            body = body,
            base = meta["base"] ?: "",
            head = meta["head"] ?: "",
            patch = meta["patch"] ?: meta["diff"] ?: PublishedMarkdown.extractPatch(body),
            signed = (meta["sig"] ?: "").isNotEmpty(),
            events = events,
            branchBacked = meta["derive"] == "branch",
        )
    }

    private fun pullEventFromMarkdown(md: String): PullEvent {
        val meta = PublishedMarkdown.frontMatter(md)
        return PullEvent(
            type = meta["type"] ?: "comment",
            body = PublishedMarkdown.bodyWithoutFrontMatter(md),
            author = meta["author"] ?: "",
            authorName = meta["authorName"] ?: "",
            state = meta["state"] ?: "",
            ts = dartIntTryParse(meta["ts"] ?: "") ?: 0L,
        )
    }

    private fun discussionFromMarkdown(number: String, md: String, events: List<DiscussionEvent>): RepoDiscussion {
        val meta = PublishedMarkdown.frontMatter(md)
        return RepoDiscussion(
            number = dartIntTryParse(number)?.toInt() ?: 0,
            title = meta["title"] ?: PublishedMarkdown.firstHeading(md) ?: "Discussion #$number",
            body = PublishedMarkdown.bodyWithoutFrontMatter(md),
            author = meta["authorName"] ?: meta["author"] ?: "",
            category = meta["category"] ?: "general",
            updatedMs = dartIntTryParse(meta["updatedAt"] ?: meta["ts"] ?: "") ?: 0L,
            events = events.filter { it.type != "answer" },
            authorKey = meta["author"] ?: "",
            answer = events.lastOrNull { it.type == "answer" },
        )
    }

    private fun discussionEventFromMarkdown(md: String): DiscussionEvent {
        val meta = PublishedMarkdown.frontMatter(md)
        return DiscussionEvent(
            type = meta["type"] ?: "comment",
            body = PublishedMarkdown.bodyWithoutFrontMatter(md),
            author = meta["author"] ?: "",
            authorName = meta["authorName"] ?: "",
            ts = dartIntTryParse(meta["ts"] ?: "") ?: 0L,
            id = meta["id"] ?: "",
            target = meta["target"] ?: "",
        )
    }

    private fun hasNetworkStatsShape(data: Map<String, Any?>): Boolean {
        val hasWorkerShape = data.containsKey("repos") && data.containsKey("hosts") &&
            data.containsKey("clients") && data.containsKey("onlineNodes") && data.containsKey("payoutNodes")
        if (hasWorkerShape) {
            return listOf("clients", "hosts", "repos").all { isIntegerLike(data[it]) } &&
                isStringList(data["onlineNodes"]) && isMapList(data["payoutNodes"])
        }
        return listOf("nodesOnline", "hostsOnline", "repos").all { data.containsKey(it) && isIntegerLike(data[it]) } &&
            data.containsKey("payoutNodes") && isMapList(data["payoutNodes"])
    }

    private fun hasNetworkLeaderboardsShape(data: Map<String, Any?>): Boolean =
        listOf("fundsMainnodes", "fundsContributors", "fundsProjects").all {
            data.containsKey(it) && isFundsReceivedList(data[it])
        }

    private fun isIntegerLike(value: Any?): Boolean = when (value) {
        is Double -> value.isFinite() && value >= 0 && value == Math.floor(value)
        is Float -> value.isFinite() && value >= 0 && value.toDouble() == Math.floor(value.toDouble())
        is Number -> value.toLong() >= 0
        is String -> (dartIntTryParse(value.trim()) ?: -1) >= 0
        else -> false
    }

    private fun isMapList(value: Any?): Boolean = value is List<*> && value.all { it is Map<*, *> }

    private fun isFundsReceivedList(value: Any?): Boolean = value is List<*> && value.all(::isFundsReceivedEntry)

    private fun isFundsReceivedEntry(value: Any?): Boolean {
        if (value !is Map<*, *>) return false
        val hasName = listOf("name", "node", "account", "project", "repo").any {
            val candidate = value[it]
            candidate is String && candidate.trim().isNotEmpty()
        }
        if (!hasName || !value.containsKey("lamports")) return false
        if (!isIntegerLike(value["lamports"])) return false
        val sol = value["sol"]
        return sol == null || isFiniteNonNegativeNumber(sol)
    }

    private fun isFiniteNonNegativeNumber(value: Any?): Boolean = when (value) {
        is Number -> value.toDouble().let { it.isFinite() && it >= 0 }
        is String -> dartDoubleTryParse(value.trim())?.let { it.isFinite() && it >= 0 } ?: false
        else -> false
    }

    private fun isStringList(value: Any?): Boolean = value is List<*> && value.all { it is String }
}

object PublishedMarkdown {
    fun lines(text: String): List<String> {
        if (text.isEmpty()) return emptyList()
        val out = ArrayList<String>()
        var start = 0
        var i = 0
        while (i < text.length) {
            val c = text[i]
            if (c == '\n' || c == '\r') {
                out.add(text.substring(start, i))
                if (c == '\r' && i + 1 < text.length && text[i + 1] == '\n') i++
                start = i + 1
            }
            i++
        }
        if (start < text.length) out.add(text.substring(start))
        return out
    }

    fun frontMatter(md: String): Map<String, String> {
        val lines = lines(md)
        if (lines.isEmpty() || lines.first().trim() != "---") return emptyMap()
        val out = LinkedHashMap<String, String>()
        for (i in 1 until lines.size) {
            val line = lines[i]
            if (line.trim() == "---") break
            val idx = line.indexOf(':')
            if (idx > 0) out[line.substring(0, idx).trim()] = line.substring(idx + 1).trim()
        }
        return out
    }

    fun firstHeading(md: String): String? {
        for (line in lines(md)) {
            val clean = line.trim()
            if (clean.startsWith("#")) return clean.replaceFirst(Regex("^#+\\s*"), "").trim()
            if (clean.isNotEmpty() && clean != "---" && !clean.contains(':')) return clean
        }
        return null
    }

    fun bodyWithoutFrontMatter(md: String): String {
        val lines = lines(md)
        if (lines.isEmpty() || lines.first().trim() != "---") return md.trim()
        var end = -1
        for (i in 1 until lines.size) {
            if (lines[i].trim() == "---") {
                end = i
                break
            }
        }
        return if (end >= 0) lines.drop(end + 1).joinToString("\n").trim() else md.trim()
    }

    private val FENCED_PATCH = Regex("```(?:diff|patch)\\s*\\n([\\s\\S]*?)```", RegexOption.MULTILINE)

    fun extractPatch(body: String): String {
        val fenced = FENCED_PATCH.find(body)
        if (fenced != null) return fenced.groups[1]?.value?.trimEnd() ?: ""
        val lines = lines(body)
        val start = lines.indexOfFirst { it.startsWith("diff --git ") || it.startsWith("--- ") }
        if (start < 0) return ""
        return lines.subList(start, lines.size).joinToString("\n").trimEnd()
    }
}

data class AccountDesktop(val key: String, val node: String, val lastSeen: Long = 0)

data class AccountDesktops(val account: String, val desktops: List<AccountDesktop>)
