package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.model.FederatedReply
import com.forkmesh.nativeapp.model.RepoDiscussion

val DISCUSSION_CATEGORY_LABELS: Map<String, String> = linkedMapOf(
    "general" to "General",
    "ideas" to "Ideas",
    "help" to "Help",
    "show-and-tell" to "Show & tell",
)

object DiscussionCard {
    fun subtitle(discussion: RepoDiscussion): String =
        "#${discussion.number} · ${discussion.category}" +
            if (discussion.author.isNotEmpty()) " · ${discussion.author}" else ""
}

object DiscussionDetail {
    const val NO_DESCRIPTION = "No description provided."
    const val TIMELINE_EMPTY = "No discussion replies have been published yet."
    const val REPLY_COPY = "Replies are signed and sent to the discussion inbox, pending the repo owner applying them."
    const val REPLY_HINT = "Write a signed reply"

    fun chips(discussion: RepoDiscussion, repoFullName: String): List<CollabChip> = buildList {
        add(CollabChip("forum_outlined", "discussion"))
        add(CollabChip("category_outlined", discussion.category))
        if (discussion.author.isNotEmpty()) add(CollabChip("person_outline", discussion.author))
        add(CollabChip("folder_outlined", repoFullName))
    }

    fun body(discussion: RepoDiscussion): String = discussion.body.ifEmpty { NO_DESCRIPTION }
}

object FederatedThread {
    const val TITLE = "Fediverse thread"
    const val INTRO =
        "Remote ActivityPub replies are shown with their original instance and backlink. They are separate from ForkMesh’s signed native event log."
    const val UNAVAILABLE = "Remote replies are currently unavailable."
    const val EMPTY = "No remote ActivityPub replies yet."
    const val NOT_NATIVE = "Remote ActivityPub · not a signed native event"

    fun author(reply: FederatedReply): String = when {
        reply.authorName.isNotEmpty() -> reply.authorName
        reply.author.isNotEmpty() -> reply.author
        else -> "Remote participant"
    }

    fun provenance(reply: FederatedReply): String =
        listOf(reply.sourceSoftware, reply.sourceInstance).filter { it.isNotEmpty() }.joinToString(" · ")

    fun unavailable(reply: FederatedReply): Boolean = reply.tombstone || reply.moderated

    private val URI_HEAD = Regex("^([A-Za-z][A-Za-z0-9+.\\-]*):(?://([^/?#]*))?")

    fun canOpenBacklink(backlink: String): Boolean {
        val match = URI_HEAD.find(backlink) ?: return false
        if (match.groupValues[1].lowercase() != "https") return false
        val authority = match.groups[2]?.value ?: return false
        val at = authority.lastIndexOf('@')
        if (at > 0) return false
        val hostPort = authority.substring(at + 1)
        val host = if (hostPort.startsWith("[")) {
            hostPort.substringBefore(']', "").removePrefix("[")
        } else {
            hostPort.substringBefore(':')
        }
        return host.isNotEmpty()
    }
}
