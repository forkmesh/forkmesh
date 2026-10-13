package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.account.showsUserStatus
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.jsonOf
import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.LocalDate
import java.time.LocalTime
import java.time.ZoneId
import java.time.ZonedDateTime
import java.util.Locale

class UserStatusTest {
    private val zone = ZoneId.of("America/New_York")

    private val now = at(2026, 9, 18, 14, 30)

    private fun at(year: Int, month: Int, day: Int, hour: Int, minute: Int): Long =
        ZonedDateTime.of(year, month, day, hour, minute, 0, 0, zone).toInstant().toEpochMilli()

    private fun label(expiresAt: Long, use24Hour: Boolean = false) =
        userStatusExpiryLabel(expiresAt, now, zone, use24Hour, Locale.US)

    fun testParsesAndSerializesTheWireShape() {
        val status = UserStatus.fromJson(
            Json.parse("""{"away":true,"emoji":"🌴","text":"Vacationing","expiresAt":1790000000000}"""),
        )
        assertEq(UserStatus(away = true, emoji = "🌴", text = "Vacationing", expiresAt = 1_790_000_000_000L), status)
        assertEq(
            jsonOf("away" to true, "emoji" to "🌴", "text" to "Vacationing", "expiresAt" to 1_790_000_000_000L),
            status.toJson(),
        )
        assertEq(status, UserStatus.fromJson(Json.parse(Json.stringify(status.toJson()))))
    }

    fun testMissingOrMalformedFieldsReadAsTheEmptyStatus() {
        assertEq(UserStatus(), UserStatus.fromJson(null))
        assertEq(UserStatus(), UserStatus.fromJson("nope"))
        assertEq(UserStatus(), UserStatus.fromJson(emptyMap<String, Any?>()))
        assertEq(null, UserStatus.fromJsonOrNull(null))
        assertEq(UserStatus(), UserStatus.fromJsonOrNull(emptyMap<String, Any?>()))
        assertEq(123L, UserStatus.fromJson(mapOf("expiresAt" to 123.0)).expiresAt)
        assertEq(456L, UserStatus.fromJson(mapOf("expiresAt" to "456")).expiresAt)
        assertEq(false, UserStatus.fromJson(mapOf("away" to "true")).away)
        assertEq(0L, UserStatus.fromJson(mapOf("expiresAt" to -5)).expiresAt)
    }

    fun testExpiryHidesTheCustomStatusButKeepsAway() {
        val status = UserStatus(away = true, emoji = "📅", text = "In a meeting", expiresAt = now)
        expect(status.isExpired(now))
        expect(!status.isExpired(now - 1))
        expect(!UserStatus(emoji = "🌴", expiresAt = 0).isExpired(Long.MAX_VALUE))
        assertEq(UserStatus(away = true), status.visible(now))
        assertEq(status, status.visible(now - 1))
        assertEq("", status.displayEmoji(now))
        assertEq("📅", status.displayEmoji(now - 1))
    }

    fun testLabelsAndNameDecoration() {
        val status = UserStatus(emoji = "🌴", text = "Vacationing")
        assertEq("🌴 Vacationing", userStatusLabel(status, now))
        assertEq("🌴", userStatusLabel(UserStatus(emoji = "🌴"), now))
        assertEq("Heads down", userStatusLabel(UserStatus(text = "Heads down"), now))
        assertEq("", userStatusLabel(UserStatus(away = true), now))
        assertEq("", userStatusLabel(null, now))
        assertEq("", userStatusLabel(status.copy(expiresAt = now - 1), now))
        assertEq("alice 🌴", nameWithStatus("alice", status, now))
        assertEq("alice", nameWithStatus("alice", status.copy(expiresAt = now), now))
        assertEq("alice", nameWithStatus("alice", null, now))
    }

    fun testCleanStatusEmojiMatchesTheWorker() {
        for (ok in listOf("🌴", "❤️", "👍🏽", "👩‍💻", "🏳️‍🌈", "🇺🇸", "1️⃣", "#⃣", "🏴󠁧󠁢󠁳󠁣󠁴󠁿", "☕")) {
            assertEq(ok, cleanStatusEmoji(ok), "accepts $ok")
        }
        assertEq("🎉", cleanStatusEmoji("  🎉 "))
        for (bad in listOf("", "  ", "hi", "🌴🌴", "a🎉", "🎉a", "🇺", "🏴󠁧󠁢", "👩‍", ":)")) {
            assertEq("", cleanStatusEmoji(bad), "rejects $bad")
        }
    }

    fun testCleanStatusTextIsOneBoundedLine() {
        assertEq("In a meeting", cleanUserStatusText("  In \n a\t  meeting \r\n"))
        assertEq("x".repeat(USER_STATUS_TEXT_MAX), cleanUserStatusText("x".repeat(150)))
        assertEq("🌴".repeat(USER_STATUS_TEXT_MAX), cleanUserStatusText("🌴".repeat(120)))
    }

    fun testDraftErrorsMirrorTheWorkerRefusals() {
        assertEq(null, userStatusDraftError("🌴", "Vacationing", 0, now))
        assertEq(null, userStatusDraftError("", "", 0, now))
        assertEq("bad_status_emoji", userStatusDraftError("hi", "", 0, now))
        assertEq("status_text_too_long", userStatusDraftError("", "x".repeat(101), 0, now))
        assertEq(null, userStatusDraftError("", "🌴".repeat(100), 0, now))
        assertEq("bad_status_expiry", userStatusDraftError("🌴", "", -1, now))
        assertEq("status_expiry_in_past", userStatusDraftError("🌴", "", now, now))
        assertEq("status_expiry_too_far", userStatusDraftError("🌴", "", now + USER_STATUS_MAX_DURATION_MS + 1, now))
        assertEq(null, userStatusDraftError("🌴", "", now + USER_STATUS_MAX_DURATION_MS, now))
        assertEq("Pick a single emoji for your status.", userStatusErrorMessage("bad_status_emoji"))
        assertEq("Choose a clear-after time in the future.", userStatusErrorMessage("status_expiry_in_past"))
        assertEq(
            "Could not update your status. Check your connection and try again.",
            userStatusErrorMessage("http_500"),
        )
    }

    fun testRequestBodies() {
        assertEq(
            jsonOf("emoji" to "🌴", "text" to "On holiday", "expiresAt" to 0L),
            userStatusSetBody(" 🌴 ", " On \n holiday ", 0),
        )
        assertEq(jsonOf("away" to true), userStatusAwayBody(true))
        assertEq(jsonOf("clear" to true), userStatusClearBody())
    }

    fun testClearAfterTimes() {
        assertEq(0L, statusExpiresAt(StatusClearAfter.Never, now, zone))
        assertEq(now + 30 * 60_000L, statusExpiresAt(StatusClearAfter.ThirtyMinutes, now, zone))
        assertEq(now + 60 * 60_000L, statusExpiresAt(StatusClearAfter.OneHour, now, zone))
        assertEq(now + 4 * 60 * 60_000L, statusExpiresAt(StatusClearAfter.FourHours, now, zone))
        assertEq(at(2026, 9, 19, 0, 0), statusExpiresAt(StatusClearAfter.Today, now, zone))
        assertEq(at(2026, 9, 21, 0, 0), statusExpiresAt(StatusClearAfter.ThisWeek, now, zone))
        assertEq(at(2026, 9, 28, 0, 0), statusExpiresAt(StatusClearAfter.ThisWeek, at(2026, 9, 21, 9, 0), zone))
        assertEq(at(2026, 9, 21, 0, 0), statusExpiresAt(StatusClearAfter.ThisWeek, at(2026, 9, 20, 23, 0), zone))
        assertEq(42L, statusExpiresAt(StatusClearAfter.Custom, now, zone, customAtMs = 42))
        assertEq(
            at(2026, 10, 2, 17, 45),
            statusCustomExpiresAt(LocalDate.of(2026, 10, 2), LocalTime.of(17, 45), zone),
        )
        assertEq(StatusClearAfter.Never, statusClearAfterFor(0))
        assertEq(StatusClearAfter.Custom, statusClearAfterFor(now + 1))
    }

    fun testExpiryLabels() {
        assertEq("", label(0))
        assertEq("", label(now))
        assertEq("Clears in 1 minute", label(now + 30_000))
        assertEq("Clears in 25 minutes", label(now + 25 * 60_000L))
        assertEq("Clears today at 6:30 PM", label(now + 4 * 60 * 60_000L))
        assertEq("Clears today at 18:30", label(now + 4 * 60 * 60_000L, use24Hour = true))
        assertEq("Clears at the end of today", label(statusExpiresAt(StatusClearAfter.Today, now, zone)))
        assertEq("Clears at the end of Sunday", label(statusExpiresAt(StatusClearAfter.ThisWeek, now, zone)))
        assertEq("Clears tomorrow at 9:00 AM", label(at(2026, 9, 19, 9, 0)))
        assertEq("Clears Tuesday at 9:00 AM", label(at(2026, 9, 22, 9, 0)))
        assertEq("Clears Oct 2 at 10:00 AM", label(at(2026, 10, 2, 10, 0)))
        assertEq("Clears Jan 5, 2027 at 10:00 AM", label(at(2027, 1, 5, 10, 0)))
    }

    fun testQuickPicksAndPresets() {
        assertEq(listOf("📅", "🚌", "🤒", "🌴", "🏡", "🎧", "🍔", "💻", "🎉", "🔕"), USER_STATUS_QUICK_EMOJI)
        assertEq(
            listOf(
                "📅 In a meeting — 1 hour",
                "🚌 Commuting — 30 minutes",
                "🤒 Out sick — Today",
                "🌴 Vacationing — Don't clear",
                "🏡 Working remotely — Today",
            ),
            USER_STATUS_PRESETS.map { it.label },
        )
        for (emoji in USER_STATUS_QUICK_EMOJI + USER_STATUS_PRESETS.map { it.emoji }) {
            assertEq(emoji, cleanStatusEmoji(emoji), "quick pick $emoji passes the Worker's rule")
        }
    }

    fun testSessionAndMemberCarryTheStatus() {
        val session = AuthSession.fromJson(
            mapOf(
                "nodeName" to "alice",
                "userStatus" to mapOf("away" to true, "emoji" to "🎧", "text" to "Focusing", "expiresAt" to 0),
            ),
        )
        assertEq(UserStatus(away = true, emoji = "🎧", text = "Focusing"), session.userStatus)
        assertEq(session.userStatus, AuthSession.fromJson(session.toJson()).userStatus)
        assertEq(UserStatus(), AuthSession.fromJson(mapOf("nodeName" to "bob")).userStatus)
        assertEq(null, Member(id = "1", name = "bob").userStatus)
        assertEq("🎧", Member(id = "1", name = "alice", userStatus = session.userStatus).userStatus?.displayEmoji(now))
    }

    fun testStatusControlsNeedARealSession() {
        val real = AuthSession.fromJson(mapOf("nodeName" to "alice"))
        expect(showsUserStatus(real, guest = false))
        expect(!showsUserStatus(null, guest = false))
        expect(!showsUserStatus(null, guest = true))
        expect(!showsUserStatus(AuthSession.fromJson(mapOf("nodeName" to "p", "sessionKind" to "preview")), guest = false))
    }
}
