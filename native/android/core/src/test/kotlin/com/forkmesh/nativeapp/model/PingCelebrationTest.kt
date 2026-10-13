package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.testing.assertEq

class PingCelebrationTest {
    private val now = 1_800_000_000_000L
    private val party = mapOf("celebrate" to "crm-email-tracked")

    fun testOnlyNewUnreadRecentCelebrationPingsParty() {
        val pings = listOf(
            ForkNotification("fresh", "plugin_activity", "Email tracked", ts = now - 30_000, meta = party),
            ForkNotification("seen", "plugin_activity", "Seen", ts = now - 10_000, meta = party),
            ForkNotification("read", "plugin_activity", "Read", ts = now - 10_000, readAt = now, meta = party),
            ForkNotification("stale", "plugin_activity", "Stale", ts = now - 6 * 60_000, meta = party),
            ForkNotification("plain", "mention", "Mention", ts = now),
            ForkNotification("", "plugin_activity", "Nameless", ts = now, meta = party),
        )
        assertEq(listOf("fresh"), PingCelebration.fresh(pings, setOf("seen"), now).map { it.id })
    }
}
