package com.forkmesh.nativeapp.notes

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.TestSettings
import com.forkmesh.nativeapp.testing.assertEq
import java.time.ZoneId
import java.time.ZoneOffset
import java.time.ZonedDateTime

class NoteRowsTest {
    private val york = ZoneId.of("America/New_York")
    private fun at(year: Int, month: Int, day: Int, hour: Int, minute: Int, zone: ZoneId = york): Long =
        ZonedDateTime.of(year, month, day, hour, minute, 0, 0, zone).toInstant().toEpochMilli()
    private val now = at(2026, 10, 31, 15, 30)

    fun testLastEditFormats() {
        assertEq("Today 3:14 PM", NoteTimes.lastEdit(at(2026, 10, 31, 15, 14), now, york))
        assertEq("Today 12:00 AM", NoteTimes.lastEdit(at(2026, 10, 31, 0, 0), now, york))
        assertEq("Yesterday 9:02 AM", NoteTimes.lastEdit(at(2026, 10, 30, 9, 2), now, york))
        assertEq("Oct 29, 3:14 PM", NoteTimes.lastEdit(at(2026, 10, 29, 15, 14), now, york))
        assertEq("Jan 1, 12:05 AM", NoteTimes.lastEdit(at(2026, 1, 1, 0, 5), now, york))
        assertEq("Oct 29, 2025", NoteTimes.lastEdit(at(2025, 10, 29, 15, 14), now, york))
        assertEq("Yesterday 11:59 PM", NoteTimes.lastEdit(at(2026, 12, 31, 23, 59), at(2027, 1, 1, 8, 0), york))
        assertEq("Dec 30, 2026", NoteTimes.lastEdit(at(2026, 12, 30, 10, 0), at(2027, 1, 1, 8, 0), york))
        assertEq("", NoteTimes.lastEdit(0, now, york))
    }

    fun testLastEditUsesTheGivenZone() {
        val instant = at(2026, 10, 31, 2, 0, ZoneOffset.UTC)
        assertEq("Yesterday 10:00 PM", NoteTimes.lastEdit(instant, now, york))
        assertEq("Today 2:00 AM", NoteTimes.lastEdit(instant, now, ZoneOffset.UTC))
    }

    fun testLastEditNamesSomeoneElse() {
        val note = mapOf("updatedAt" to at(2026, 10, 31, 15, 14), "lastEditedBy" to "bo")
        assertEq("Today 3:14 PM · by bo", NoteRows.lastEdit(note, "ana", now, york))
        assertEq("Today 3:14 PM", NoteRows.lastEdit(note, " BO ", now, york))
        assertEq("Today 3:14 PM", NoteRows.lastEdit(note - "lastEditedBy", "ana", now, york))
        assertEq("", NoteRows.lastEdit(mapOf("lastEditedBy" to "bo"), "ana", now, york))
    }

    fun testRowInfo() {
        val shares = listOf(
            mapOf("type" to "user", "name" to "ana"),
            mapOf("type" to "organization", "name" to "crew"),
            mapOf("type" to "user", "name" to ""),
            mapOf("type" to "user", "name" to "bo"),
            mapOf("type" to "user", "name" to "cy"),
            mapOf("type" to "user", "name" to "di"),
        )
        val info = NoteRows.info(mapOf(
            "unread" to true, "lastEditedBy" to "ana", "myLastViewedAt" to 5.0, "unreadComments" to 2.0, "openComments" to 3L, "shares" to shares,
        ))
        assertEq(true, info.unread)
        assertEq("ana", info.lastEditedBy)
        assertEq(5L, info.myLastViewedAt)
        assertEq(2, info.unreadComments)
        assertEq(3, info.openComments)
        assertEq(NoteReach.Shared, info.reach)
        assertEq(listOf(NoteShareFace("ana", false), NoteShareFace("crew", true), NoteShareFace("bo", false)), info.faces)
        assertEq(2, info.extraFaces)
        assertEq("+2", info.extraLabel)
        val bare = NoteRows.info(emptyMap())
        assertEq(false, bare.unread)
        assertEq(0, bare.openComments)
        assertEq(NoteReach.Private, bare.reach)
        assertEq("", bare.extraLabel)
        assertEq(NoteReach.Public, NoteRows.reach(mapOf("visibility" to "public", "shares" to shares)))
        assertEq("Private", NoteReach.Private.label)
        assertEq("1 open comment thread", NoteRows.commentCountLabel(1))
        assertEq("4 open comment threads", NoteRows.commentCountLabel(4))
    }

    fun testSeenClearsUnreadLocally() {
        val notes = listOf(
            mapOf("id" to "a", "unread" to true, "updatedAt" to 100L, "unreadComments" to 2L, "myLastViewedAt" to 10L),
            mapOf("id" to "b", "unread" to true, "updatedAt" to 300L),
            mapOf("id" to "c", "unread" to true, "updatedAt" to 100L),
        )
        val shown = NoteRows.applySeen(notes, mapOf("a" to 100L, "b" to 200L))
        assertEq(false, shown[0]["unread"])
        assertEq(0L, shown[0]["unreadComments"])
        assertEq(100L, shown[0]["myLastViewedAt"])
        assertEq(true, shown[1]["unread"])
        assertEq(true, shown[2]["unread"])
        assertEq(notes, NoteRows.applySeen(notes, emptyMap()))
        assertEq(false, NoteRows.info(NoteRows.markRead(notes[1], 300L)).unread)
    }

    fun testAccessSummary() {
        val note = mapOf(
            "title" to "Plan", "role" to "editor", "updatedAt" to at(2026, 10, 31, 15, 14), "lastEditedBy" to "bo",
            "shares" to listOf(mapOf("type" to "user", "name" to "bo", "role" to "editor"), mapOf("type" to "organization", "name" to "crew", "role" to "viewer")),
        )
        val summary = NoteAccess.summary(note, "ana", york)
        assertEq("Plan", summary.title)
        assertEq("You can edit", summary.role)
        assertEq(listOf("bo · Can edit", "crew (organization) · Can view"), summary.sharedWith)
        assertEq(NoteReach.Shared, summary.reach)
        assertEq("Last edited Saturday, October 31, 2026 at 3:14 PM by bo", summary.lastEdit)
        assertEq(false, summary.isOwner)
        assertEq("Last edited Saturday, October 31, 2026 at 3:14 PM by you", NoteAccess.lastEdit(note, "BO", york))
        assertEq("Last edited Saturday, October 31, 2026 at 3:14 PM", NoteAccess.lastEdit(note - "lastEditedBy", "ana", york))
        assertEq("", NoteAccess.lastEdit(emptyMap(), "ana", york))
        assertEq("You own this note", NoteAccess.roleText(mapOf("role" to "owner")))
        assertEq("You own this note", NoteAccess.roleText(emptyMap()))
        assertEq("View only", NoteAccess.roleText(mapOf("role" to "viewer")))
        assertEq(listOf("Not shared"), NoteAccess.shareLines(mapOf("shares" to listOf(mapOf("name" to "")))))
        val mine = NoteAccess.summary(mapOf("title" to " ", "visibility" to "public"), "ana", york)
        assertEq("Untitled note", mine.title)
        assertEq(true, mine.isOwner)
        assertEq(NoteReach.Public, mine.reach)
        assertEq("Thursday, January 1, 2026 at 12:05 AM", NoteTimes.full(at(2026, 1, 1, 0, 5), york))
    }

    fun testRecordViewRequest() {
        val calls = mutableListOf<String>()
        val api = object : ApiService(TestSettings()) {
            override fun accountApiCall(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
                calls.add("$method $path ${body ?: ""}")
                return emptyMap()
            }
        }
        NotesApi(api).recordView("n1")
        assertEq(listOf("POST /api/notes/n1/activity {kind=view}"), calls)
    }
}
