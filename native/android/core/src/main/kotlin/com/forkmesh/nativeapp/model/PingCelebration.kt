package com.forkmesh.nativeapp.model

import kotlin.math.abs

object PingCelebration {
    const val FRESH_MS: Long = 5 * 60 * 1000

    fun isCelebration(ping: ForkNotification): Boolean =
        (ping.meta["celebrate"] as? String).orEmpty().isNotBlank()

    fun fresh(pings: List<ForkNotification>, seen: Set<String>, now: Long): List<ForkNotification> =
        pings.filter { ping ->
            ping.id.isNotEmpty() && ping.id !in seen && ping.isUnread && isCelebration(ping) &&
                abs(now - ping.ts) <= FRESH_MS
        }
}
