package com.forkmesh.nativeapp.ci

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.model.RepoRelease

data class RepoRef(val owner: String, val name: String) {
    val slug: String get() = "$owner/$name"
}

class CiError(val status: Int, val code: String) : Exception() {
    override val message: String
        get() = when {
            code == "forbidden" || code == "not_authorized" || status == 403 -> "Only the repository owner and organization writers can do that."
            code == "actions_node_unavailable" || status == 503 -> "The node that runs this repository's actions is not reachable right now."
            code == "invalid_session" || status == 401 -> "Sign in again to manage this repository."
            status == 404 || code == "not_found" -> "This repository was not found, or it is private."
            code == "request_too_large" -> "That request is too large."
            code == "invalid_url" -> "Use a public https:// address."
            code == "no_events" -> "Choose at least one event."
            code == "duplicate_url" -> "A webhook for that address already exists."
            code == "too_many_webhooks" -> "This repository already has the most webhooks it can have."
            code == "invalid_request" -> "The server refused that request as invalid."
            code == "not_due" -> "This repository was scanned in the last day."
            code == "lease_held" -> "A scan is already running."
            status == 429 || code == "retry_backoff" -> "Too many attempts. Try again later."
            code.isNotEmpty() -> code.replace('_', ' ').split(' ').joinToString(" ") { word ->
                word.lowercase().replaceFirstChar { it.uppercase() }
            } + "."
            else -> "The server answered $status."
        }

    override fun toString(): String = message
}

interface CiClient {
    fun actionsRuns(repo: RepoRef): ActionsSummary
    fun actionsControl(repo: RepoRef, request: ActionsControlRequest)
    fun webhooks(repo: RepoRef, action: String, fields: Map<String, Any?>): Map<String, Any?>
    fun securityScans(repo: RepoRef, operation: String, query: List<Pair<String, String>>): Map<String, Any?>
    fun securityTriage(repo: RepoRef, scanId: String, findingId: String, status: String, note: String): Map<String, Any?>
    fun releases(owner: String, name: String): List<RepoRelease>
    fun mirrorsPayload(repo: RepoRef): Map<String, Any?>
    fun clearRepoCache(owner: String, name: String)
    fun announceRelease(
        repo: RepoRef,
        tag: String,
        title: String,
        body: String,
        account: String,
        sign: (ByteArray) -> String,
        nowMillis: Long,
    )
}

object CiRequests {
    fun repoPath(repo: RepoRef, tail: String): String = "/api/repo/${repo.owner}/${repo.name}/$tail"

    fun decode(result: ApiJsonResult): Map<String, Any?> {
        val map = result.body
        if (result.ok && map["error"] == null) return map
        throw CiError(result.statusCode, map.str("error").ifEmpty { map.str("reason") })
    }

    fun announceCanonical(account: String, ts: String): String = "forkmesh-issues-pull-v1\n$account\n$ts"

    fun postBody(title: String, body: String, account: String): Map<String, Any?> = linkedMapOf(
        "kind" to "discussion",
        "eventType" to "open",
        "manual" to true,
        "title" to title.take(240),
        "body" to body.take(4000),
        "author" to account,
    )

    fun announceBody(tag: String, title: String, body: String, account: String): Map<String, Any?> = linkedMapOf(
        "kind" to "release",
        "eventType" to "publish",
        "manual" to true,
        "tag" to tag,
        "title" to title.take(240),
        "body" to body.take(4000),
        "author" to account,
    )
}

class ApiCiClient(private val api: ApiService) : CiClient {
    private fun send(
        method: String,
        path: String,
        query: List<Pair<String, String>> = emptyList(),
        body: Map<String, Any?>? = null,
    ): Map<String, Any?> = CiRequests.decode(api.jsonExchange(method, path, query, body))

    override fun actionsRuns(repo: RepoRef): ActionsSummary =
        ActionsSummary.fromJson(send("GET", CiRequests.repoPath(repo, "actions/runs")))

    override fun actionsControl(repo: RepoRef, request: ActionsControlRequest) {
        send("POST", CiRequests.repoPath(repo, "actions/control"), body = request.body)
    }

    override fun webhooks(repo: RepoRef, action: String, fields: Map<String, Any?>): Map<String, Any?> =
        send("POST", CiRequests.repoPath(repo, "webhooks"), body = LinkedHashMap(fields).apply { put("action", action) })

    override fun securityScans(repo: RepoRef, operation: String, query: List<Pair<String, String>>): Map<String, Any?> =
        send("GET", CiRequests.repoPath(repo, "security-scans/$operation"), query)

    override fun securityTriage(repo: RepoRef, scanId: String, findingId: String, status: String, note: String): Map<String, Any?> {
        val body = linkedMapOf<String, Any?>("findingId" to findingId, "status" to status)
        if (scanId.isNotEmpty()) body["scanId"] = scanId
        if (note.isNotEmpty()) body["note"] = note
        return send("PATCH", CiRequests.repoPath(repo, "security-scans/triage"), body = body)
    }

    override fun releases(owner: String, name: String): List<RepoRelease> = api.releases(owner, name)

    override fun mirrorsPayload(repo: RepoRef): Map<String, Any?> = send("GET", CiRequests.repoPath(repo, "mirrors"))

    override fun clearRepoCache(owner: String, name: String) = api.clearRepoCache(owner, name)

    override fun announceRelease(
        repo: RepoRef,
        tag: String,
        title: String,
        body: String,
        account: String,
        sign: (ByteArray) -> String,
        nowMillis: Long,
    ) {
        val ts = nowMillis.toString()
        val sig = sign(CiRequests.announceCanonical(account, ts).toByteArray(Charsets.UTF_8))
        send(
            "POST",
            CiRequests.repoPath(repo, "ap-publish"),
            listOf("owner" to account, "ts" to ts, "sig" to sig),
            CiRequests.announceBody(tag, title, body, account),
        )
    }

    fun announcePost(repo: RepoRef, title: String, body: String, account: String, sign: (ByteArray) -> String, nowMillis: Long) {
        val ts = nowMillis.toString()
        val sig = sign(CiRequests.announceCanonical(account, ts).toByteArray(Charsets.UTF_8))
        send(
            "POST",
            CiRequests.repoPath(repo, "ap-publish"),
            listOf("owner" to account, "ts" to ts, "sig" to sig),
            CiRequests.postBody(title, body, account),
        )
    }
}
