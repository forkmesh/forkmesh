package com.forkmesh.nativeapp.push

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class PingIconsTest {
    fun testEachKindHasItsOwnLittleIcon() {
        assertEq("🚀", pingIcon("release_published"))
        assertEq("📣", pingIcon("mention"))
        assertEq(PING_DEFAULT_ICON, pingIcon("no_such_kind"))
        expect(PING_KIND_ICONS.values.all { it.isNotBlank() && it.trim() == it }, "icons are trimmed glyphs")
    }

    fun testDesktopAgentPingsShowHowTheRunEnded() {
        assertEq("🤖", pingIcon("desktop_ping", mapOf("category" to "agent-finished")))
        assertEq("⚠️", pingIcon("desktop_ping", mapOf("category" to "agent-failed")))
        assertEq("💻", pingIcon("desktop_ping"))
        assertEq("❌", pingIcon("desktop_ping", mapOf("category" to "ci-failed")))
        assertEq("✅", pingIcon("operational_alert", mapOf("state" to "up")))
        assertEq("📟", pingIcon("operational_alert", mapOf("state" to "down")))
        assertEq("⚠️", pingIcon("pull_attention", mapOf("reason" to "conflict")))
    }

    fun testTheIconSitsBeforeTheHeadlineOnce() {
        assertEq("🔔 Hello", withPingIcon(" Hello ", PING_DEFAULT_ICON))
        assertEq("🔔 Hello", withPingIcon("🔔 Hello", PING_DEFAULT_ICON))
        assertEq("", withPingIcon("", PING_DEFAULT_ICON))
        assertEq("Hello", withPingIcon("Hello", ""))
    }
}
