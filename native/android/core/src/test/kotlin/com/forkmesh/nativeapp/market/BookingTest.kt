package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.api.ApiJsonResult
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.navigation.RouteCodec
import com.forkmesh.nativeapp.navigation.RouteParser
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.LocalDate
import java.time.ZoneId
import java.util.Locale

class BookingTest {
    private val id = "a".repeat(32)
    private val london = ZoneId.of("Europe/London")
    private val nov3 = 1_793_696_400_000L

    private fun bookingJson(bookingId: String = id, start: Long = nov3, extra: Map<String, Any?> = emptyMap()): Map<String, Any?> =
        linkedMapOf<String, Any?>(
            "id" to bookingId, "status" to "confirmed", "typeId" to "30min", "typeTitle" to "Meeting", "minutes" to 30,
            "startAt" to start, "endAt" to start + 1_800_000, "createdAt" to start - 86_400_000,
            "guest" to mapOf("name" to "Grace Hopper", "email" to "grace@example.org", "phone" to "", "timezone" to "America/New_York", "notes" to "Compilers"),
            "location" to mapOf("kind" to "video", "detail" to "https://meet.example.com/ada"), "calendarEventId" to "e".repeat(32),
        ) + extra

    private fun overviewJson(): Map<String, Any?> = linkedMapOf(
        "ok" to true, "installed" to true, "actor" to "ada", "slug" to "ada", "enabled" to true,
        "link" to "https://forkmesh.com/meet?ada", "links" to mapOf("30min" to "https://forkmesh.com/meet?ada/30min"),
        "origin" to "https://forkmesh.com",
        "settings" to mapOf(
            "displayName" to "Ada Lovelace", "headline" to "Let's talk", "timezone" to "Europe/London", "timeFormat" to "24h", "accent" to "violet",
            "availability" to mapOf("mon" to listOf(listOf("09:00", "17:00")), "tue" to listOf(listOf("08:00", "24:00")), "sat" to emptyList<Any>()),
            "minNoticeMinutes" to 120, "maxDaysAhead" to 30, "dailyLimit" to 4, "replyTo" to "ada@example.com", "notifyEmail" to "ada@example.com",
            "emailOwner" to true, "addToCalendar" to false, "checkCalendar" to true,
            "types" to listOf(mapOf("id" to "30min", "title" to "Meeting", "minutes" to 30, "location" to mapOf("kind" to "phone", "detail" to ""), "enabled" to false)),
        ),
        "options" to mapOf("durations" to listOf(15, 30), "accents" to mapOf("violet" to "#7c3aed")),
        "upcoming" to listOf(bookingJson()), "past" to listOf(bookingJson("b".repeat(32), nov3 - 86_400_000 * 3, mapOf("status" to "cancelled", "cancelledBy" to "guest"))),
        "stats" to mapOf("upcoming" to 1, "next7Days" to 1, "booked30Days" to 2, "cancelled30Days" to 1, "bookedMinutes7Days" to 30),
    )

    fun testOverviewDecodes() {
        val overview = BookingOverview.fromJson(overviewJson())
        expect(overview.installed)
        assertEq("forkmesh.com/meet?", overview.prefix)
        assertEq("forkmesh.com/meet?ada", Booking.displayLink(overview.link))
        assertEq("Ada Lovelace", overview.settings.displayName)
        expect(overview.settings.twentyFour)
        assertEq(listOf(BookingWindow("08:00", "23:59")), overview.settings.windows("tue"))
        assertEq(emptyList<BookingWindow>(), overview.settings.windows("sun"))
        assertEq("phone", overview.settings.types.single().location.kind)
        expect(!overview.settings.types.single().enabled)
        expect(!overview.settings.addToCalendar)
        assertEq(listOf(15, 30), overview.options.durations)
        assertEq(listOf(0, 5, 10, 15, 20, 30, 60), overview.options.steps)
        assertEq("Grace Hopper", overview.upcoming.single().guest.name)
        assertEq("https://meet.example.com/ada", overview.upcoming.single().where())
        assertEq("Cancelled by guest", overview.past.single().badge(nov3))
        expect(overview.inPast("b".repeat(32)))
        assertEq(30, overview.stats.bookedMinutes7Days)
        val missing = BookingOverview.fromJson(mapOf("installed" to false, "suggestedSlug" to "ada", "origin" to "https://example.org"))
        expect(!missing.installed)
        assertEq("example.org/meet?", missing.prefix)
    }

    fun testLabels() {
        assertEq("15 min", Booking.minutesLabel(15))
        assertEq("1 h", Booking.minutesLabel(60))
        assertEq("1 h 30 min", Booking.minutesLabel(90))
        assertEq("Any time", Booking.noticeLabel(0))
        assertEq("30 minutes before", Booking.noticeLabel(30))
        assertEq("1 hour before", Booking.noticeLabel(60))
        assertEq("4 hours before", Booking.noticeLabel(240))
        assertEq("2 days before", Booking.noticeLabel(2880))
        assertEq("Meeting length", Booking.stepLabel(0))
        assertEq("No limit", Booking.dailyLimitLabel(0))
        assertEq("09:00 – 09:30", Booking.timeRange(nov3, nov3 + 1_800_000, london, true, Locale.US))
        assertEq("9:00 AM – 9:30 AM", Booking.timeRange(nov3, nov3 + 1_800_000, london, false, Locale.US))
        assertEq("Tuesday, November 3", Booking.dayLabel(LocalDate.of(2026, 11, 3), Locale.US))
    }

    fun testGroupingByDay() {
        val items = listOf(bookingJson(), bookingJson("c".repeat(32), nov3 + 3_600_000), bookingJson("d".repeat(32), nov3 + 86_400_000))
            .mapNotNull(BookingItem::fromJson)
        val groups = Booking.groupByDay(items, london)
        assertEq(listOf(LocalDate.of(2026, 11, 3), LocalDate.of(2026, 11, 4)), groups.map { it.first })
        assertEq(2, groups.first().second.size)
    }

    fun testSettingsDraftBuildsThePutBody() {
        var settings = BookingOverview.fromJson(overviewJson()).settings
        settings = settings.withDay("wed", true).addWindow("wed").window("wed", 1) { it.copy(end = "18:30") }
        settings = settings.addWindow("mon").window("mon", 1) { it.copy(start = "18:00", end = "17:00") }
        settings = settings.withDay("tue", false).addType().type(1) { it.copy(title = " Deep dive ", minutes = 60) }.removeType(0)
        val body = settings.body()
        val week = body["availability"] as Map<*, *>
        assertEq(listOf(listOf("09:00", "17:00"), listOf("13:00", "18:30")), week["wed"])
        assertEq(listOf(listOf("09:00", "17:00")), week["mon"])
        assertEq(emptyList<Any>(), week["tue"])
        assertEq(7, week.size)
        val types = body["types"] as List<*>
        assertEq(1, types.size)
        assertEq("Deep dive", (types.single() as Map<*, *>)["title"])
        assertEq(60, (types.single() as Map<*, *>)["minutes"])
        assertEq(4, body["dailyLimit"])
        val endOfDay = BookingOverview.fromJson(overviewJson()).settings.availabilityBody()["tue"]
        assertEq(listOf(listOf("08:00", "24:00")), endOfDay)
    }

    fun testErrorsAndSlugs() {
        assertEq("That link is taken. Try another.", BookingErrors.describe(PackagesException("slug_taken", 409)))
        assertEq("A video call link must start with https://.", BookingErrors.sentence("invalid_meeting_url"))
        expect(Booking.slugIsValid("ada-chats"))
        expect(!Booking.slugIsValid("Ada"))
        expect(!Booking.slugIsValid("-ada"))
        assertEq("Available", BookingSlugCheck("x", true, "").sentence)
        assertEq("That link is reserved. Try another.", BookingSlugCheck("admin", false, "slug_reserved").sentence)
    }

    fun testClientCallsTheBookingApi() {
        val calls = mutableListOf<Triple<String, String, Map<String, Any?>?>>()
        val client = PackagesClient { method, path, _, body ->
            calls.add(Triple(method, path, body))
            ApiJsonResult(200, if (path.endsWith("/slug/fresh")) mapOf("available" to true) else overviewJson())
        }
        client.installBooking(" Ada ", "Europe/London")
        client.saveBooking(mapOf("enabled" to false))
        client.cancelBooking(id, " Sorry ", false)
        expect(client.checkBookingSlug("fresh").available)
        assertEq("invalid_slug", client.checkBookingSlug("Bad Slug").reason)
        assertEq(listOf("POST /api/booking/install", "PUT /api/booking/page", "POST /api/booking/bookings/$id/cancel", "GET /api/booking/slug/fresh"),
            calls.map { "${it.first} ${it.second}" })
        assertEq(mapOf("slug" to "ada", "timezone" to "Europe/London"), calls[0].third)
        assertEq(mapOf("reason" to "Sorry", "notifyGuest" to false), calls[2].third)
    }

    fun testRoutes() {
        assertEq(listOf(Route.Booking(id)), RouteParser.parse("https://app.forkmesh.com/dashboard/booking?booking=$id")!!.stack)
        assertEq(listOf(Route.Booking(null)), RouteParser.parse("https://app.forkmesh.com/dashboard/booking?booking=nope")!!.stack)
        assertEq(Route.Booking(id), RouteCodec.decode(RouteCodec.encode(Route.Booking(id))))
        assertEq("booking", PackageGate.ROUTE_KINDS[Route.Kind.Booking])
        assertEq("booking", PackageGate.HUB_ITEMS["booking"])
    }
}
