package com.forkmesh.nativeapp.collab

import com.forkmesh.nativeapp.api.DartException

const val PENDING_NOTE = "Submitted to the inbox - pending the repo owner applying it."

const val THREAD_SUBSCRIPTION_NOTE =
    "Thread notifications are signed with your node key and delivered through the mainnode notification inbox."

fun dartErrorText(error: Throwable): String = when (error) {
    is DartException -> error.toString()
    is UnsupportedOperationException -> "Unsupported operation: ${error.message ?: ""}"
    is IllegalArgumentException, is IllegalStateException -> error.message ?: error.toString()
    else -> error.toString()
}

fun runFailureText(error: Throwable): String = "Failed: ${dartErrorText(error)}"

fun badgeTone(text: String): CollabTone {
    val normalized = text.lowercase()
    if (normalized.contains("open") || normalized.contains("live") || normalized.contains("signed")) return CollabTone.Success
    if (normalized.contains("closed") || normalized.contains("offline") || normalized.contains("request")) return CollabTone.Danger
    if (normalized.contains("merged")) return CollabTone.Accent
    return CollabTone.Secondary
}

object ThreadSubscription {
    const val SUBSCRIBED = "Subscribed to thread notifications."
    const val UNSUBSCRIBED = "Unsubscribed from thread notifications."
    const val GUEST_ACTION = "follow threads"

    fun signedAccountNode(sessionNodeName: String?, sessionPubkey: String?, identityPublicKey: String?): String {
        if (sessionNodeName == null || identityPublicKey == null) return ""
        val node = sessionNodeName.trim().lowercase()
        if (node.isEmpty()) return ""
        if (sessionPubkey != identityPublicKey) return ""
        return node
    }

    fun label(guest: Boolean, canSign: Boolean, subscribed: Boolean): String = when {
        guest -> "Sign in for thread alerts"
        !canSign -> "Thread alerts unavailable"
        subscribed -> "Unsubscribe thread"
        else -> "Subscribe thread"
    }

    fun icon(guest: Boolean, canSign: Boolean, subscribed: Boolean): String = when {
        guest || !canSign -> "lock_outline"
        subscribed -> "notifications_off_outlined"
        else -> "notifications_active_outlined"
    }

    fun toggled(subscribed: Boolean): String = if (subscribed) SUBSCRIBED else UNSUBSCRIBED
}
