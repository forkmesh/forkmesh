package com.forkmesh.nativeapp.planning

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.model.OrgApiException

object PlanningErrors {
    fun text(error: Throwable, fallback: String): String {
        if (error is OrgApiException) {
            return when (error.code) {
                "not_found" -> "That was not found. It may have been deleted."
                "forbidden", "org_member_required" -> "You do not have access to that."
                "unauthorized", "invalid_session" -> "Sign in again to continue."
                "http_0", "" -> fallback
                else -> if (error.message.isEmpty()) fallback else error.message.replaceFirstChar { it.uppercase() } + "."
            }
        }
        return fallback
    }
}

object TimerPings {
    const val MAX_AGE_MS: Long = 15 * 60 * 1_000

    fun body(timer: PlanningTimer): Map<String, Any?>? {
        if (timer.kind == PlanningTimerKind.STOPWATCH || timer.finishedAtMs <= 0) return null
        val id = timer.id.filter { it.isLetterOrDigit() || it == '-' }.take(40)
        if (id.isEmpty()) return null
        return linkedMapOf(
            "timerId" to id,
            "kind" to timer.kind.wire,
            "endsAt" to timer.finishedAtMs,
            "label" to timer.label.take(80),
        )
    }

    fun post(rung: List<PlanningTimer>, api: ApiService, nowMillis: Long): Int {
        var sent = 0
        for (timer in rung) {
            if (nowMillis - timer.finishedAtMs > MAX_AGE_MS) continue
            val body = body(timer) ?: continue
            try {
                api.accountApiCall("POST", "/api/notifications/timer", body)
                sent++
            } catch (_: Exception) {
            }
        }
        return sent
    }
}
