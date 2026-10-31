package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.security.DeviceIdentity
import java.security.MessageDigest

open class InboxService(
    private val settings: ClientSettings,
    private val identity: () -> DeviceIdentity?,
    private val transport: HttpTransport = HttpTransport.DEFAULT,
    private val clock: () -> Long = { System.currentTimeMillis() },
) {
    private fun signer(): DeviceIdentity =
        identity() ?: throw DartException("This device's signing key is not ready yet. Try again in a moment.")

    private fun repoEndpoint(owner: String, name: String, leaf: String): String =
        ServerUrl.parse(settings.serverUrl).http("/api/repo/$owner/$name/$leaf")

    private fun sha256Hex(content: String): String =
        MessageDigest.getInstance("SHA-256").digest(content.toByteArray(Charsets.UTF_8))
            .joinToString("") { "%02x".format(it.toInt() and 0xff) }

    private fun join(vararg parts: Any?): String = parts.joinToString(NUL) { jsonText(it) }

    private fun listJoin(value: Any?): String = (value as? List<*>)?.joinToString(",") { jsonText(it) } ?: ""

    private fun intText(value: Any?): String = "${(value as? Number)?.let { if (it is Double || it is Float) it.toDouble().toLong() else it.toLong() } ?: 0}"

    fun issueContent(type: String, ev: Map<String, Any?>): String {
        val attachments = listJoin(ev["attachments"])
        return when (type) {
            "open" -> join(ev["title"] ?: "", ev["body"] ?: "", attachments)
            "comment", "edit" -> join(ev["body"] ?: "", attachments)
            "title" -> jsonText(ev["title"])
            "status" -> jsonText(ev["status"])
            "labels" -> listJoin(ev["labels"])
            "milestone" -> jsonText(ev["milestone"])
            "priority" -> intText(ev["priority"])
            "progress" -> intText(ev["progress"])
            "assignees" -> listJoin(ev["assignees"])
            "delete" -> jsonText(ev["target"])
            "vote" -> ""
            else -> ""
        }
    }

    fun eventSigningPrefix(kind: String, owner: String, name: String): String = EventSigning.prefix(kind, owner, name)

    private fun signedIssueEvent(owner: String, name: String, number: Int, type: String, fields: Map<String, Any?>): LinkedHashMap<String, Any?> {
        val identity = signer()
        val author = identity.publicKeyB64url
        val ts = clock()
        val ev = LinkedHashMap<String, Any?>()
        ev["type"] = type
        ev.putAll(fields)
        val contentHash = sha256Hex(issueContent(type, ev))
        val canonical = eventSigningPrefix("issue-event", owner, name) + "$type\n$number\n$author\n$ts\n$contentHash"
        val sig = identity.sign(canonical.toByteArray(Charsets.UTF_8))
        ev["author"] = author
        ev["ts"] = ts
        ev["sig"] = sig
        return ev
    }

    open fun submitNewIssue(
        owner: String,
        name: String,
        title: String,
        body: String,
        labels: List<String> = emptyList(),
        milestone: String = "",
        priority: Int = 0,
        assignees: List<String> = emptyList(),
    ) {
        val event = signedIssueEvent(owner, name, 0, "open", linkedMapOf("title" to title, "body" to body, "attachments" to emptyList<String>()))
        post(
            repoEndpoint(owner, name, "issues"),
            linkedMapOf(
                "number" to 0,
                "titleIfNew" to title,
                "event" to event,
                "meta" to linkedMapOf(
                    "labels" to labels,
                    "milestone" to milestone,
                    "priority" to priority,
                    "assignees" to assignees,
                ),
            ),
        )
    }

    open fun commentOnIssue(owner: String, name: String, number: Int, body: String) {
        val event = signedIssueEvent(owner, name, number, "comment", linkedMapOf("body" to body, "attachments" to emptyList<String>()))
        post(repoEndpoint(owner, name, "issues"), linkedMapOf("number" to number, "event" to event))
    }

    open fun voteOnIssue(owner: String, name: String, number: Int) {
        val event = signedIssueEvent(owner, name, number, "vote", emptyMap())
        post(repoEndpoint(owner, name, "issues"), linkedMapOf("number" to number, "event" to event))
    }

    open fun setIssueStatus(owner: String, name: String, number: Int, status: String) {
        val event = signedIssueEvent(owner, name, number, "status", linkedMapOf("status" to status))
        post(repoEndpoint(owner, name, "issues"), linkedMapOf("number" to number, "event" to event))
    }

    open fun submitNewPull(
        owner: String,
        name: String,
        title: String,
        base: String,
        head: String,
        patch: String,
        description: String = "",
        authorName: String = "",
        commits: String = "",
    ): Int {
        val identity = signer()
        val author = identity.publicKeyB64url
        val ts = clock()
        val fields = listOf(title, base, head, patch) + if (commits.isNotEmpty()) listOf(commits) else emptyList()
        val contentHash = sha256Hex(fields.joinToString(NUL))
        val canonical = eventSigningPrefix("pull-event", owner, name) + "$author\n$ts\n$contentHash"
        val sig = identity.sign(canonical.toByteArray(Charsets.UTF_8))
        val answer = post(
            repoEndpoint(owner, name, "pulls"),
            linkedMapOf(
                "pull" to linkedMapOf(
                    "title" to title,
                    "base" to base,
                    "head" to head,
                    "patch" to patch,
                    *(if (commits.isNotEmpty()) arrayOf("commits" to commits) else emptyArray()),
                    *(if (description.isNotEmpty()) arrayOf("description" to description) else emptyArray()),
                    "author" to author,
                    *(if (authorName.isNotEmpty()) arrayOf("authorName" to authorName) else emptyArray()),
                    "ts" to ts,
                    "sig" to sig,
                ),
            ),
        )
        return (answer["number"] as? Number)?.toInt() ?: 0
    }

    open fun openRemotePull(
        fork: com.forkmesh.nativeapp.model.RemoteFork,
        title: String,
        base: String,
        head: String,
        patch: String,
        commits: String = "",
        description: String = "",
        authorName: String = "",
    ): Int {
        val url = fork.pullsUrl ?: throw DartException("invalid_remote_fork")
        val identity = signer()
        val author = identity.publicKeyB64url
        val ts = clock()
        val contentHash = sha256Hex(listOf(title, base, head, patch, commits).joinToString(NUL))
        val canonical = eventSigningPrefix("pull-event", fork.owner, fork.name) + "$author\n$ts\n$contentHash"
        val sig = identity.sign(canonical.toByteArray(Charsets.UTF_8))
        val body = linkedMapOf(
            "owner" to fork.owner,
            "repo" to fork.name,
            "pull" to linkedMapOf(
                "title" to title,
                "base" to base,
                "head" to head,
                "patch" to patch,
                "commits" to commits,
                "description" to description,
                "author" to author,
                "authorName" to authorName,
                "ts" to ts,
                "sig" to sig,
            ),
        )
        val answer = try {
            post(url, body)
        } catch (error: DartException) {
            throw error
        } catch (error: InterruptedException) {
            throw error
        } catch (_: Exception) {
            throw DartException("remote_unreachable")
        }
        return (answer["number"] as? Number)?.toInt() ?: 0
    }

    private fun pullCommentAnchorFields(ev: Map<String, Any?>): List<String> {
        val patchset = jsonText(ev["patchset"])
        val anchorLines = jsonText(ev["anchorLines"])
        if (patchset.isEmpty() && anchorLines.isEmpty()) return emptyList()
        return listOf(patchset, anchorLines)
    }

    fun pullCommentContent(type: String, ev: Map<String, Any?>): String = when (type) {
        "comment" -> jsonText(ev["body"])
        "review" -> join(ev["state"] ?: "", ev["body"] ?: "")
        "line-comment" -> (
            listOf(jsonText(ev["path"]), jsonText(ev["side"]), intText(ev["line"]), jsonText(ev["body"])) +
                pullCommentAnchorFields(ev)
            ).joinToString(NUL)
        "thread-comment" -> (
            listOf(
                jsonText(ev["threadId"]),
                jsonText(ev["path"]),
                jsonText(ev["side"]),
                intText(ev["lineStart"]),
                intText(ev["lineEnd"]),
                jsonText(ev["body"]),
                jsonText(ev["suggestionPatch"]),
            ) + pullCommentAnchorFields(ev)
            ).joinToString(NUL)
        "thread-reply" -> join(ev["threadId"] ?: "", ev["parentId"] ?: "", ev["body"] ?: "")
        "thread-state" -> join(ev["threadId"] ?: "", ev["state"] ?: "", ev["body"] ?: "")
        "suggestion-state" -> join(ev["threadId"] ?: "", ev["state"] ?: "", ev["appliedCommit"] ?: "", ev["body"] ?: "")
        else -> ""
    }

    private fun signedPullComment(owner: String, name: String, number: Int, type: String, fields: Map<String, Any?>): LinkedHashMap<String, Any?> {
        val identity = signer()
        val author = identity.publicKeyB64url
        val ts = clock()
        val ev = LinkedHashMap<String, Any?>()
        ev["type"] = type
        ev.putAll(fields)
        val contentHash = sha256Hex(pullCommentContent(type, ev))
        val canonical = eventSigningPrefix("pull-comment", owner, name) + "$type\n$number\n$author\n$ts\n$contentHash"
        val sig = identity.sign(canonical.toByteArray(Charsets.UTF_8))
        ev["author"] = author
        ev["ts"] = ts
        ev["sig"] = sig
        return ev
    }

    open fun commentOnPull(owner: String, name: String, number: Int, body: String) {
        val event = signedPullComment(owner, name, number, "comment", linkedMapOf("body" to body))
        post(repoEndpoint(owner, name, "pulls"), linkedMapOf("number" to number, "event" to event))
    }

    open fun reviewPull(owner: String, name: String, number: Int, state: String, body: String) {
        val event = signedPullComment(owner, name, number, "review", linkedMapOf("state" to state, "body" to body))
        post(repoEndpoint(owner, name, "pulls"), linkedMapOf("number" to number, "event" to event))
    }

    fun discussionContent(type: String, ev: Map<String, Any?>): String = when (type) {
        "open" -> join(ev["title"] ?: "", ev["body"] ?: "", ev["category"] ?: "")
        "comment" -> jsonText(ev["body"])
        "answer" -> jsonText(ev["target"])
        else -> ""
    }

    private fun signedDiscussionEvent(owner: String, name: String, number: Int, type: String, fields: Map<String, Any?>): LinkedHashMap<String, Any?> {
        val identity = signer()
        val author = identity.publicKeyB64url
        val ts = clock()
        val ev = LinkedHashMap<String, Any?>()
        ev["type"] = type
        ev.putAll(fields)
        val contentHash = sha256Hex(discussionContent(type, ev))
        val canonical = eventSigningPrefix("discussion-event", owner, name) + "$type\n$number\n$author\n$ts\n$contentHash"
        val sig = identity.sign(canonical.toByteArray(Charsets.UTF_8))
        ev["author"] = author
        ev["ts"] = ts
        ev["sig"] = sig
        return ev
    }

    open fun submitNewDiscussion(owner: String, name: String, title: String, body: String, category: String = "general") {
        val event = signedDiscussionEvent(owner, name, 0, "open", linkedMapOf("title" to title, "body" to body, "category" to category))
        post(
            repoEndpoint(owner, name, "discussions"),
            linkedMapOf("number" to 0, "titleIfNew" to title, "event" to event),
        )
    }

    open fun commentOnDiscussion(owner: String, name: String, number: Int, body: String) {
        val event = signedDiscussionEvent(owner, name, number, "comment", linkedMapOf("body" to body))
        post(repoEndpoint(owner, name, "discussions"), linkedMapOf("number" to number, "event" to event))
    }

    open fun markDiscussionAnswer(owner: String, name: String, number: Int, target: String, sessionToken: String) {
        val event = signedDiscussionEvent(
            owner, name, number, "answer", linkedMapOf("id" to "answer-android-${clock()}", "target" to target),
        )
        val body = linkedMapOf<String, Any?>("number" to number, "event" to event)
        if (sessionToken.isNotEmpty()) body["sessionToken"] = sessionToken
        post(repoEndpoint(owner, name, "discussions"), body)
    }

    open fun setThreadSubscription(owner: String, name: String, node: String, source: String, number: Int, subscribed: Boolean) {
        val cleanNode = node.trim().lowercase()
        require(cleanNode.isNotEmpty()) { "Invalid argument (node): expected account node name: \"$node\"" }
        require(source == "issue" || source == "pull") { "Invalid argument (source): expected issue or pull: \"$source\"" }
        require(number > 0) { "Invalid argument (number): expected positive thread number: $number" }
        val identity = signer()
        val ts = "${clock()}"
        val canonical = "forkmesh-subscribe-v1\n$owner/$name\n$source\n$number\n${if (subscribed) "1" else "0"}\n$ts"
        val sig = identity.sign(canonical.toByteArray(Charsets.UTF_8))
        post(
            repoEndpoint(owner, name, "subscribe"),
            linkedMapOf(
                "node" to cleanNode,
                "source" to source,
                "number" to number,
                "subscribed" to subscribed,
                "ts" to ts,
                "sig" to sig,
            ),
        )
    }

    private fun post(url: String, body: Map<String, Any?>): Map<*, *> {
        val request = HttpClient.Request.post(url)
            .header("Content-Type", "application/json")
            .timeouts(20_000, 20_000)
        request.body = Json.stringify(body).toByteArray(Charsets.UTF_8)
        val response = transport.send(request)
        if (response.status == 200 || response.status == 201) {
            return try {
                Json.parse(response.text()) as? Map<*, *> ?: emptyMap<String, Any?>()
            } catch (_: Exception) {
                emptyMap<String, Any?>()
            }
        }
        var detail = "HTTP ${response.status}"
        try {
            val decoded = Json.parse(response.text())
            if (decoded is Map<*, *> && decoded["error"] != null) detail = jsonText(decoded["error"])
        } catch (_: Exception) {
        }
        throw DartException(detail)
    }

    private companion object {
        const val NUL = "\u0000"
    }
}
