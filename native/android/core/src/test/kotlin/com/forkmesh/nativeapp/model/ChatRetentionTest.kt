package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ChatRetentionTest {
    private val hour = 3_600_000L
    private val day = 24 * hour

    fun testMissingPreferencesMeanSevenDays() {
        val empty = ChatRetention.fromJson(null)
        assertEq(7 * day, empty.defaultMs)
        assertEq(7 * day, empty.effectiveMs("#general"))
        assertEq(null, ChatRetention.fromJsonOrNull("nope"))
        assertEq(7 * day, ChatRetention.fromJson(mapOf("defaultMs" to null)).defaultMs)
    }

    fun testServerPayloadParsesAndClampsIntoOneHourToSevenDays() {
        val parsed = ChatRetention.fromJson(
            mapOf(
                "defaultMs" to 3 * day,
                "chats" to mapOf("#team" to 60_000L, "@peer" to 30 * day, "channel:abc" to 8.0 * hour, "bad" to "x", "" to hour),
                "updatedAt" to 1234L,
            ),
        )
        assertEq(3 * day, parsed.defaultMs)
        assertEq(mapOf("#team" to hour, "@peer" to 7 * day, "channel:abc" to 8 * hour), parsed.chats)
        assertEq(1234L, parsed.updatedAt)
        assertEq(hour, ChatRetention.fromJson(mapOf("defaultMs" to 5L)).defaultMs)
        assertEq(7 * day, ChatRetention(defaultMs = 99 * day).effectiveMs("#x"))
    }

    fun testPerChatOverrideWinsOverTheDefault() {
        val rule = ChatRetention(defaultMs = day, chats = mapOf("#team" to hour))
        assertEq(hour, rule.effectiveMs("#team"))
        assertEq(day, rule.effectiveMs("#general"))
        assertEq(hour, rule.overrideMs("#team"))
        assertEq(null, rule.overrideMs("#general"))
    }

    fun testExpiryIsInclusiveAtTheCutoff() {
        val rule = ChatRetention(defaultMs = hour)
        val now = 10 * day
        expect(rule.isExpired(now - hour, "#general", now))
        expect(!rule.isExpired(now - hour + 1, "#general", now))
        expect(rule.isExpired(0, "#general", now))
        expect(!ChatRetention().isExpired(now - 6 * day, "#general", now))
    }

    fun testLabelsUsePresetNamesAndFallBackToUnits() {
        assertEq(listOf("7 days", "3 days", "1 day", "8 hours", "1 hour"), ChatRetention.PRESETS.map { it.label })
        assertEq(listOf(604_800_000L, 259_200_000L, 86_400_000L, 28_800_000L, 3_600_000L), ChatRetention.PRESETS.map { it.ms })
        assertEq("2 days", ChatRetention.label(2 * day))
        assertEq("5 hours", ChatRetention.label(5 * hour))
        assertEq("90 minutes", ChatRetention.label(90 * 60_000L))
        assertEq("1 hour", ChatRetention.label(1L))
        assertEq("3 days", ChatRetention(defaultMs = 3 * day).defaultLabel)
    }

    fun testCanonicalChatKeys() {
        val id = "a".repeat(32)
        assertEq("channel:$id", ChatRetention.officeKey(id))
        assertEq("channel:$id", ChatRetention.key("#office/design", id))
        assertEq("#office/design", ChatRetention.key("#office/design"))
        assertEq("#general", ChatRetention.key("#general"))
        assertEq("@peer-1", ChatRetention.key("@peer-1"))
        assertEq("chat_retention/alice", ChatRetention.prefsKey(" Alice "))
    }

    fun testLocalCacheRoundTrips() {
        val rule = ChatRetention(defaultMs = 8 * hour, chats = mapOf("@peer" to hour), updatedAt = 77)
        assertEq(rule, ChatRetention.decode(rule.encode()))
        assertEq(null, ChatRetention.decode(""))
        assertEq(null, ChatRetention.decode("{broken"))
        assertEq(null, ChatRetention.decode(null))
    }

    fun testRequestBodiesMatchTheServerContract() {
        assertEq(mapOf("defaultMs" to hour), ChatRetention.defaultBody(hour))
        assertEq(mapOf("defaultMs" to null), ChatRetention.defaultBody(null))
        assertEq(mapOf("chat" to "#team", "retentionMs" to null), ChatRetention.chatBody("#team", null))
        assertEq("That disappearing-message time is not valid.", chatRetentionErrorMessage("invalid_chat_retention"))
    }
}
