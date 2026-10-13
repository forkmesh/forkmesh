package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.testing.assertEq

class AccountSignInsTest {
    private val now = 1_800_000_000_000L

    fun testParsesTheSessionList() {
        val parsed = AccountSignIn.parse(
            mapOf(
                "sessions" to listOf(
                    mapOf("id" to "a1", "deviceLabel" to "Desktop app", "ipAddress" to "10.0.0.2", "lastSeenAt" to (now - 5 * 60_000).toDouble(), "current" to false),
                    mapOf("id" to "b2", "deviceLabel" to "  ", "createdAt" to "${now - 2 * 3_600_000}", "current" to true),
                    mapOf("deviceLabel" to "no id"),
                    "junk",
                ),
            ),
        )
        assertEq(listOf("a1", "b2"), parsed.map { it.id })
        assertEq("Desktop app", parsed[0].title)
        assertEq("desktop_mac_outlined", parsed[0].icon)
        assertEq("Active 5m ago · 10.0.0.2", parsed[0].meta(now))
        assertEq("Unknown device (this app)", parsed[1].title)
        assertEq("devices_other", parsed[1].icon)
        assertEq("Signed in 2h ago", parsed[1].meta(now))
        assertEq("", AccountSignIn("x", "Browser").meta(now))
        assertEq("language", AccountSignIn("x", "Browser").icon)
        assertEq("phone_iphone", AccountSignIn("x", "ForkMesh mobile").icon)
        assertEq(emptyList<AccountSignIn>(), AccountSignIn.parse(listOf(1)))
    }

    fun testRevokeHelpers() {
        val list = listOf(AccountSignIn("a", "Phone", current = true), AccountSignIn("b", "Browser"), AccountSignIn("c", "Desktop app"))
        assertEq(2, AccountSignIns.others(list))
        assertEq(listOf("a"), AccountSignIns.remaining(list, AccountSignIns.OTHERS).map { it.id })
        assertEq(listOf("a", "c"), AccountSignIns.remaining(list, "b").map { it.id })
        assertEq("/api/accounts/sessions/a%20b%2Fc", AccountSignIns.revokePath("a b/c"))
        assertEq(true, AccountSignIns.currentRevoked(mapOf("currentRevoked" to true)))
        assertEq(false, AccountSignIns.currentRevoked(mapOf("ok" to true)))
        assertEq("Signed-in sessions", AccountSignIns.headline(null))
        assertEq("1 active session", AccountSignIns.headline(list.take(1)))
        assertEq("3 active sessions", AccountSignIns.headline(list))
        assertEq("Loading sessions…", AccountSignIns.emptyText(loading = false, loaded = false))
        assertEq("No active sessions found.", AccountSignIns.emptyText(loading = false, loaded = true))
        assertEq("Log out of this app?", AccountSignIns.confirmTitle(list[0]))
        assertEq("Browser will be signed out and must sign in again to use your account.", AccountSignIns.confirmBody(list[1]))
        assertEq("Logged out of every other session.", AccountSignIns.revokedText(AccountSignIns.OTHERS))
        assertEq("Session logged out.", AccountSignIns.revokedText("b"))
    }
}
