package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.api.EventSigning
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.security.DeviceIdentity
import java.security.MessageDigest
import java.security.SecureRandom

data class CollabNotice(val message: String, val isError: Boolean = false)

class CollabException(message: String) : Exception(message) {
    override fun toString(): String = message ?: ""
}

object CollabContent {
    private const val NUL = "\u0000"

    private fun text(value: Any?): String = if (value == null) "" else jsonText(value)

    private fun intText(value: Any?): String = when (value) {
        null -> "0"
        is Double -> if (value.isFinite()) value.toLong().toString() else "0"
        is Float -> if (value.isFinite()) value.toLong().toString() else "0"
        is Number -> value.toLong().toString()
        else -> jsonText(value).trim().toLongOrNull()?.toString() ?: "0"
    }

    private fun joined(value: Any?): String = (value as? List<*>)?.joinToString(",") { text(it) } ?: ""

    fun issue(type: String, ev: Map<String, Any?>): String {
        val attachments = joined(ev["attachments"])
        return when (type) {
            "open" -> listOf(text(ev["title"]), text(ev["body"]), attachments).joinToString(NUL)
            "comment", "edit" -> listOf(text(ev["body"]), attachments).joinToString(NUL)
            "title" -> text(ev["title"])
            "status" -> text(ev["status"])
            "labels" -> joined(ev["labels"])
            "milestone" -> text(ev["milestone"])
            "dates" -> listOf(intText(ev["startDate"]), intText(ev["endDate"])).joinToString(NUL)
            "priority" -> intText(ev["priority"])
            "progress" -> intText(ev["progress"])
            "assignees" -> joined(ev["assignees"])
            "claim" -> listOf(text(ev["assignee"]), text(ev["claimState"])).joinToString(NUL)
            "reporter" -> text(ev["reporter"])
            "reaction" -> listOf(text(ev["target"]), text(ev["reaction"])).joinToString(NUL)
            "delete" -> text(ev["target"])
            else -> ""
        }
    }

    fun pull(type: String, ev: Map<String, Any?>): String {
        val patchset = text(ev["patchset"])
        val lines = text(ev["anchorLines"])
        val anchor = if (patchset.isEmpty() && lines.isEmpty()) emptyList() else listOf(patchset, lines)
        return when (type) {
            "comment" -> text(ev["body"])
            "review" -> listOf(text(ev["state"]), text(ev["body"])).joinToString(NUL)
            "line-comment" -> (listOf(text(ev["path"]), text(ev["side"]), intText(ev["line"]), text(ev["body"])) + anchor).joinToString(NUL)
            "thread-comment" -> (
                listOf(
                    text(ev["threadId"]), text(ev["path"]), text(ev["side"]), intText(ev["lineStart"]),
                    intText(ev["lineEnd"]), text(ev["body"]), text(ev["suggestionPatch"]),
                ) + anchor
                ).joinToString(NUL)
            "thread-reply" -> listOf(text(ev["threadId"]), text(ev["parentId"]), text(ev["body"])).joinToString(NUL)
            "thread-state" -> listOf(text(ev["threadId"]), text(ev["state"]), text(ev["body"])).joinToString(NUL)
            "suggestion-state" ->
                listOf(text(ev["threadId"]), text(ev["state"]), text(ev["appliedCommit"]), text(ev["body"])).joinToString(NUL)
            else -> ""
        }
    }

    fun discussion(type: String, ev: Map<String, Any?>): String = when (type) {
        "open" -> listOf(text(ev["title"]), text(ev["body"]), text(ev["category"])).joinToString(NUL)
        "comment" -> text(ev["body"])
        else -> ""
    }

    val PULL_EVENT_TYPES = setOf(
        "comment", "review", "line-comment", "thread-comment", "thread-reply", "thread-state", "suggestion-state",
    )

    val BRANCH_EVENT_TYPES = setOf("thread-comment", "thread-reply", "thread-state")

    val ISSUE_EVENT_TYPES = setOf(
        "open", "comment", "edit", "title", "status", "labels", "milestone", "dates", "priority", "progress",
        "assignees", "claim", "reporter", "reaction", "delete", "vote",
    )
}

enum class CollabRecordKind(val raw: String, val leaf: String, val signingKind: String) {
    Issue("issue", "issues", "issue-event"),
    Pull("pull", "pulls", "pull-comment"),
    Discussion("discussion", "discussions", "discussion-event"),
    ;

    fun content(type: String, ev: Map<String, Any?>): String = when (this) {
        Issue -> CollabContent.issue(type, ev)
        Pull -> CollabContent.pull(type, ev)
        Discussion -> CollabContent.discussion(type, ev)
    }

    val allowed: Set<String>
        get() = when (this) {
            Issue -> CollabContent.ISSUE_EVENT_TYPES
            Pull -> CollabContent.PULL_EVENT_TYPES
            Discussion -> setOf("open", "comment")
        }
}

class CollabSigner(
    val identity: DeviceIdentity,
    private val now: () -> Long = System::currentTimeMillis,
    val authorName: String = "",
    private val random: () -> String = ::randomHex,
) {
    val author: String get() = identity.publicKeyB64url

    fun canonical(
        kind: CollabRecordKind,
        owner: String,
        name: String,
        type: String,
        number: Int,
        ts: Long,
        event: Map<String, Any?>,
    ): String {
        val hash = sha256Hex(kind.content(type, event))
        return "${EventSigning.prefix(kind.signingKind, owner, name)}$type\n$number\n$author\n$ts\n$hash"
    }

    fun canonicalBranch(owner: String, name: String, branch: String, type: String, ts: Long, event: Map<String, Any?>): String {
        if (EventSigning.repoKey(owner, name).isEmpty()) throw CollabException("Branch comments need a repository owner and name.")
        val hash = sha256Hex(CollabContent.pull(type, event))
        return "${EventSigning.prefix(BRANCH_SIGNING_KIND, owner, name)}$type\n$branch\n$author\n$ts\n$hash"
    }

    fun sign(
        kind: CollabRecordKind,
        owner: String,
        name: String,
        number: Int,
        type: String,
        fields: Map<String, Any?>,
    ): LinkedHashMap<String, Any?> {
        if (type !in kind.allowed) throw CollabException("Unsupported ${kind.raw} event: $type.")
        return signed(type, fields) { ts, ev -> canonical(kind, owner, name, type, number, ts, ev) }
    }

    fun signBranch(owner: String, name: String, branch: String, type: String, fields: Map<String, Any?>): LinkedHashMap<String, Any?> {
        if (type !in CollabContent.BRANCH_EVENT_TYPES) throw CollabException("Unsupported branch event: $type.")
        if (branch.isEmpty()) throw CollabException("Pick a branch to comment on.")
        return signed(type, fields) { ts, ev -> canonicalBranch(owner, name, branch, type, ts, ev) }
    }

    private fun signed(type: String, fields: Map<String, Any?>, canonical: (Long, Map<String, Any?>) -> String): LinkedHashMap<String, Any?> {
        val ts = now()
        val ev = LinkedHashMap<String, Any?>()
        ev["type"] = type
        ev.putAll(fields)
        if (ev.str("id").isEmpty()) ev["id"] = ReviewComposer.eventId(type, ts, random())
        val sig = identity.sign(canonical(ts, ev).toByteArray(Charsets.UTF_8))
        ev["author"] = author
        if (authorName.isNotEmpty()) ev["authorName"] = authorName
        ev["ts"] = ts
        ev["sig"] = sig
        return ev
    }

    companion object {
        const val BRANCH_SIGNING_KIND = "branch-comment"
        private val secure = SecureRandom()

        fun randomHex(): String {
            val bytes = ByteArray(9)
            secure.nextBytes(bytes)
            return bytes.joinToString("") { "%02x".format(it.toInt() and 0xff) }
        }

        fun sha256Hex(content: String): String =
            MessageDigest.getInstance("SHA-256").digest(content.toByteArray(Charsets.UTF_8))
                .joinToString("") { "%02x".format(it.toInt() and 0xff) }
    }
}

typealias CollabJsonCall = (method: String, path: String, query: List<Pair<String, String>>, body: Map<String, Any?>?) -> ApiJsonResult

object CollabHttp {
    fun errorMessage(result: ApiJsonResult): String {
        val message = result.body.str("message").trim()
        if (message.isNotEmpty()) return message
        return when (val code = result.body.str("error").trim()) {
            "bad_signature" -> "The relay could not verify this device's signature."
            "inbox_full", "author_quota" -> "The repository's inbox is full. Try again later."
            "event_too_large" -> "That is longer than the relay accepts."
            "unauthorized", "invalid_session", "account_required" -> "Sign in again and retry."
            "not_authorized", "forbidden" -> "Only the repository owner can do that."
            "not_found" -> "That repository or record was not found."
            "" -> "The relay answered HTTP ${result.statusCode}."
            else -> code.replace('_', ' ')
        }
    }

    fun json(
        call: CollabJsonCall,
        method: String,
        path: String,
        query: List<Pair<String, String>> = emptyList(),
        body: Map<String, Any?>? = null,
    ): Map<String, Any?> {
        val result = call(method, path, query, body)
        if (!result.ok || result.body["ok"] == false) throw CollabException(errorMessage(result))
        return result.body
    }
}

class CollabRecordWriter(
    private val call: CollabJsonCall,
    val signer: CollabSigner?,
    val owner: String,
    val name: String,
) {
    private fun requireSigner(): CollabSigner =
        signer ?: throw CollabException("Sign in on this device to write to this repository.")

    fun send(
        kind: CollabRecordKind,
        number: Int,
        type: String,
        fields: Map<String, Any?>,
        extra: Map<String, Any?> = emptyMap(),
    ): Map<String, Any?> {
        val event = requireSigner().sign(kind, owner, name, number, type, fields)
        val body = linkedMapOf<String, Any?>("number" to number.toLong(), "event" to event)
        body.putAll(extra)
        CollabHttp.json(call, "POST", "/api/repo/$owner/$name/${kind.leaf}", body = body)
        return event
    }

    fun sendPull(number: Int, fields: ReviewEventFields): Map<String, Any?> = send(CollabRecordKind.Pull, number, fields.type, fields.fields)

    fun sendBranch(branch: String, fields: ReviewEventFields): Map<String, Any?> {
        val event = requireSigner().signBranch(owner, name, branch, fields.type, fields.fields)
        val result = call("POST", "/api/repo/$owner/$name/${CollabRecordKind.Pull.leaf}", emptyList(), linkedMapOf("branch" to branch, "event" to event))
        if (result.statusCode != 201 || result.body["ok"] == false) throw CollabException(CollabHttp.errorMessage(result))
        return event
    }

    fun submitReview(number: Int, events: List<ReviewEventFields>): Int {
        var sent = 0
        for (fields in events) {
            sendPull(number, fields)
            sent++
        }
        return sent
    }

    fun sendIssue(number: Int, edit: IssueEdit): Map<String, Any?> = send(CollabRecordKind.Issue, number, edit.type, edit.fields)

    fun applyIssue(number: Int, edits: List<IssueEdit>) {
        for (edit in edits) sendIssue(number, edit)
    }

    fun openDiscussion(title: String, body: String, category: String) {
        send(
            CollabRecordKind.Discussion,
            0,
            "open",
            linkedMapOf("title" to title, "body" to body, "category" to category),
            linkedMapOf("titleIfNew" to title),
        )
    }

    fun commentOnDiscussion(number: Int, body: String) {
        send(CollabRecordKind.Discussion, number, "comment", linkedMapOf("body" to body))
    }
}
