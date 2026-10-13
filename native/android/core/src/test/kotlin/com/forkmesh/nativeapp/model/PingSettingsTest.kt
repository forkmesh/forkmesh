package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class PingSettingsTest {
    @Suppress("UNCHECKED_CAST")
    private fun parse(text: String): PingSettings = PingSettings.fromJson(Json.parse(text) as Map<String, Any?>)

    fun testParsesTheFullAdminPayload() {
        val settings = parse(
            """
            {"ok": true, "emailNotifications": false,
             "notificationPreferences": {"mention": true, "subscribed": false, "general_chat": false, "junk": "yes"},
             "notificationDelivery": "instant",
             "notificationPauseUntil": 1770000360000,
             "pingMutedKinds": ["repo_activity", "tip_sent"],
             "notificationQuietHours": {"enabled": true, "start": 1380, "end": 390, "days": [4, 0, 9, 2, 2], "timezone": "Europe/Berlin"},
             "notificationQuietUntil": 0,
             "pingKindCatalog": [
               {"key": "conversations", "label": "Conversations", "kinds": [
                 {"kind": "mention", "label": "Mentions", "email": true},
                 {"kind": "chat_webhook", "label": "Chat webhook posts", "email": false}]},
               {"key": "empty", "label": "Empty", "kinds": []},
               {"key": "admin", "label": "Administration", "kinds": [{"kind": "operational_alert", "label": "Operational alerts"}]}],
             "statusMonitorAlerts": [
               {"id": "website", "label": "Website", "pings": true, "emails": false, "continual": true},
               {"id": "", "label": "dropped"},
               {"id": "d1", "pings": false, "emails": true}]}
            """.trimIndent(),
        )
        assertEq(false, settings.emailNotifications)
        assertEq(mapOf("mention" to true, "subscribed" to false, "general_chat" to false), settings.notificationPreferences)
        assertEq("instant", settings.notificationDelivery)
        assertEq(1_770_000_360_000L, settings.notificationPauseUntil)
        assertEq(setOf("repo_activity", "tip_sent"), settings.pingMutedKinds)
        assertEq(PingQuietHours(true, 1380, 390, listOf(0, 2, 4), "Europe/Berlin"), settings.notificationQuietHours)
        assertEq(listOf("conversations", "admin"), settings.pingKindCatalog.map { it.key })
        assertEq(PingKind("chat_webhook", "Chat webhook posts", false), settings.pingKindCatalog[0].kinds[1])
        assertEq(false, settings.pingKindCatalog[1].kinds.single().email)
        assertEq(
            listOf(
                StatusMonitorAlert("website", "Website", pings = true, emails = false, continual = true),
                StatusMonitorAlert("d1", "d1", pings = false, emails = true, continual = false),
            ),
            settings.statusMonitorAlerts,
        )
    }

    fun testDefaultsForAnOldWorkerAndANonAdmin() {
        val settings = parse("""{"ok": true}""")
        assertEq(true, settings.emailNotifications)
        assertEq("digest", settings.notificationDelivery)
        assertEq(0L, settings.notificationPauseUntil)
        assertEq(emptySet<String>(), settings.pingMutedKinds)
        assertEq(PingQuietHours(false, 22 * 60, 7 * 60, listOf(0, 1, 2, 3, 4, 5, 6), "UTC"), settings.notificationQuietHours)
        assertEq(PING_KIND_FALLBACK_CATALOG, settings.pingKindCatalog)
        expect(settings.pingKindCatalog.none { it.key == "admin" })
        assertEq(null, settings.statusMonitorAlerts)
    }

    fun testOutOfRangeQuietMinutesFallBackAndDeliveryIsNormalised() {
        val settings = parse(
            """{"notificationDelivery": "weekly", "notificationQuietHours": {"start": 1440, "end": -5}, "pingMutedKinds": {"mention": false, "subscribed": true}}""",
        )
        assertEq("digest", settings.notificationDelivery)
        assertEq(22 * 60, settings.notificationQuietHours.start)
        assertEq(7 * 60, settings.notificationQuietHours.end)
        assertEq(listOf(0, 1, 2, 3, 4, 5, 6), settings.notificationQuietHours.days)
        assertEq(setOf("mention"), settings.pingMutedKinds)
    }

    fun testFallbackCatalogHasUniqueKindsWithTheServerEmailFlags() {
        val kinds = PING_KIND_FALLBACK_CATALOG.flatMap { it.kinds }
        assertEq(kinds.size, kinds.map { it.kind }.toSet().size)
        assertEq(true, kinds.first { it.kind == "host_offline" }.email)
        assertEq(false, kinds.first { it.kind == "repo_activity" }.email)
        assertEq(
            listOf("Conversations", "Repositories", "Organizations", "Money & rewards", "Devices & apps"),
            PING_KIND_FALLBACK_CATALOG.map { it.label },
        )
    }
}
