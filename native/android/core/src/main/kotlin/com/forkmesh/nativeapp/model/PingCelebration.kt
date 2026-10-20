package com.forkmesh.nativeapp.model

import kotlin.math.abs

object PingCelebration {
    const val FRESH_MS: Long = 5 * 60 * 1000

    fun isCelebration(ping: ForkNotification): Boolean =
        (ping.meta["celebrate"] as? String).orEmpty().isNotBlank()

    fun badgeLine(ping: ForkNotification): String? {
        if ((ping.meta["celebrate"] as? String).orEmpty().trim() != "badge") return null
        val name = (ping.meta["badgeName"] as? String).orEmpty().trim()
        if (name.isEmpty()) return "You earned a new badge!"
        val tier = (ping.meta["tier"] as? String).orEmpty().trim().lowercase()
        if (tier in setOf("bronze", "silver", "gold")) return "You reached ${tier.replaceFirstChar { it.uppercase() }} $name!"
        return "You earned $name!"
    }

    fun fresh(pings: List<ForkNotification>, seen: Set<String>, now: Long): List<ForkNotification> =
        pings.filter { ping ->
            ping.id.isNotEmpty() && ping.id !in seen && ping.isUnread && isCelebration(ping) &&
                abs(now - ping.ts) <= FRESH_MS
        }
}
