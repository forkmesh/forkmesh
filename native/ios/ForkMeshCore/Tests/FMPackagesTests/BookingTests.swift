import FMApi
import FMBase
import XCTest

@testable import FMPackages

final class BookingTests: XCTestCase {
    let us = Locale(identifier: "en_US_POSIX")
    let london = TimeZone(identifier: "Europe/London")!
    let monday: Int64 = 1_793_611_800_000

    func overview() -> JSONObject {
        [
            "ok": true, "installed": true, "actor": "alice", "slug": "alice", "enabled": true,
            "link": "https://forkmesh.com/meet?alice", "origin": "https://forkmesh.com",
            "links": ["intro": "https://forkmesh.com/meet?alice/intro", "call": "https://forkmesh.com/meet?alice/call"],
            "settings": [
                "displayName": "Ada Lovelace", "headline": "Let's talk", "welcome": "Hi", "timezone": "Europe/London",
                "timeFormat": "24h", "accent": "violet",
                "availability": [
                    "mon": [["09:00", "12:00"], ["13:00", "24:00"]], "tue": [["09:00", "17:00"]], "wed": [], "thu": [],
                    "fri": [["10:00"]], "sat": [], "sun": [],
                ],
                "minNoticeMinutes": 120, "maxDaysAhead": 30, "bufferMinutes": 15, "slotStepMinutes": 0, "dailyLimit": 4,
                "replyTo": "ada@example.com", "notifyEmail": "ada@example.com", "emailOwner": false,
                "addToCalendar": true, "checkCalendar": true,
                "types": [
                    [
                        "id": "intro", "title": "Intro call", "minutes": 20, "description": "Hello",
                        "location": ["kind": "video", "detail": "https://meet.example.com/ada"], "enabled": true,
                    ],
                    ["id": "call", "title": "Phone call", "minutes": 90, "location": ["kind": "phone"], "enabled": false],
                ],
            ],
            "options": [
                "durations": [15, 30, 60], "steps": [0, 15], "buffers": [0, 15], "notices": [0, 120], "horizons": [30, 60],
                "locations": ["video", "phone", "bogus"],
                "accents": ["slate": "#475569", "blue": "#2563eb", "violet": "#7c3aed"], "timeFormats": ["12h", "24h"],
            ],
            "upcoming": [
                [
                    "id": .string(hexA), "status": "confirmed", "typeId": "intro", "typeTitle": "Intro call", "minutes": 20,
                    "startAt": .int(monday), "endAt": .int(monday + 1_200_000), "createdAt": 1,
                    "guest": ["name": "Grace Hopper", "email": "grace@example.org", "timezone": "America/New_York", "notes": "Compilers"],
                    "location": ["kind": "video", "detail": "https://meet.example.com/ada"], "calendarEventId": "e1",
                    "rescheduledFrom": 5,
                ],
                ["id": "bad", "startAt": 1],
                [
                    "id": .string(hexB), "typeTitle": "Phone call", "minutes": 30, "startAt": .int(monday + 86_400_000),
                    "endAt": .int(monday + 86_400_000 + 1_800_000), "guest": ["name": "Alan", "phone": "+44 20 7946 0000"],
                    "location": ["kind": "phone", "detail": ""],
                ],
            ],
            "past": [
                [
                    "id": .string(String(repeating: "c", count: 32)), "status": "cancelled", "cancelledBy": "owner",
                    "cancelReason": "Sick", "startAt": .int(monday - 86_400_000), "endAt": .int(monday - 86_400_000 + 600_000),
                ],
            ],
            "stats": ["upcoming": 2, "next7Days": 2, "booked30Days": 3, "cancelled30Days": 1, "bookedMinutes7Days": 50],
        ]
    }

    func testOverviewDecodes() {
        let data = BookingOverview(json: overview())
        XCTAssertTrue(data.installed)
        XCTAssertEqual(data.displayLink, "forkmesh.com/meet?alice")
        XCTAssertEqual(data.linkPrefix, "forkmesh.com/meet?")
        XCTAssertEqual(data.settings.displayName, "Ada Lovelace")
        XCTAssertEqual(data.settings.timeFormat, "24h")
        XCTAssertEqual(data.settings.availability["mon"], [BookingWindow(start: "09:00", end: "12:00"), BookingWindow(start: "13:00", end: "24:00")])
        XCTAssertEqual(data.settings.availability["fri"], [])
        XCTAssertEqual(data.settings.types.map(\.id), ["intro", "call"])
        XCTAssertFalse(data.settings.types[1].enabled)
        XCTAssertEqual(data.settings.types[1].location, BookingLocation(kind: "phone", detail: ""))
        XCTAssertFalse(data.settings.emailOwner)
        XCTAssertEqual(data.options.durations, [15, 30, 60])
        XCTAssertEqual(data.options.locations, ["video", "phone"])
        XCTAssertEqual(data.options.accents.map(\.id), ["blue", "violet", "slate"])
        XCTAssertEqual(data.options.accent("violet")?.rgb?.red ?? 0, 124.0 / 255, accuracy: 0.0001)
        XCTAssertEqual(data.upcoming.map(\.id), [hexA, hexB])
        XCTAssertEqual(data.link(for: data.settings.types[0]), "https://forkmesh.com/meet?alice/intro")
        XCTAssertEqual(data.stats.hoursBooked, "50 min booked")
        XCTAssertEqual(data.find(String(repeating: "c", count: 32))?.cancelReason, "Sick")

        let empty = BookingOverview(json: ["ok": true, "installed": false, "actor": "bob", "suggestedSlug": "bob"])
        XCTAssertFalse(empty.installed)
        XCTAssertEqual(empty.suggestedSlug, "bob")
        XCTAssertEqual(empty.linkPrefix, "forkmesh.com/meet?")
        XCTAssertEqual(empty.options.durations.first, 10)
        XCTAssertEqual(empty.options.accents.count, 6)
    }

    func testBookingRowsBadgesAndPlaces() {
        let data = BookingOverview(json: overview())
        let first = data.upcoming[0]
        XCTAssertEqual(first.guest.name, "Grace Hopper")
        XCTAssertEqual(first.whereText, "https://meet.example.com/ada")
        XCTAssertEqual(first.badges(now: monday - 1), ["Moved"])
        XCTAssertEqual(first.badges(now: monday + 3_600_000), ["Done", "Moved"])
        XCTAssertTrue(first.canCancel(now: monday))
        XCTAssertFalse(first.canCancel(now: monday + 3_600_000))
        XCTAssertEqual(data.upcoming[1].whereText, "+44 20 7946 0000")
        XCTAssertEqual(data.upcoming[1].status, "confirmed")
        let cancelled = data.past[0]
        XCTAssertEqual(cancelled.badges(now: monday), ["Cancelled by you"])
        XCTAssertEqual(cancelled.whereText, "")
        XCTAssertFalse(cancelled.canCancel(now: 0))
        var guest = cancelled
        guest.cancelledBy = "guest"
        XCTAssertEqual(guest.badges(now: monday), ["Cancelled by guest"])
    }

    func testLabels() {
        XCTAssertEqual(BookingText.displayLink("https://forkmesh.com/meet?alice"), "forkmesh.com/meet?alice")
        XCTAssertEqual(BookingText.displayLink("http://localhost:8787/meet?a"), "localhost:8787/meet?a")
        XCTAssertEqual(BookingText.minutesLabel(15), "15 min")
        XCTAssertEqual(BookingText.minutesLabel(60), "1 h")
        XCTAssertEqual(BookingText.minutesLabel(90), "1 h 30 min")
        XCTAssertEqual(BookingText.noticeLabel(0), "Any time")
        XCTAssertEqual(BookingText.noticeLabel(30), "30 minutes before")
        XCTAssertEqual(BookingText.noticeLabel(60), "1 hour before")
        XCTAssertEqual(BookingText.noticeLabel(240), "4 hours before")
        XCTAssertEqual(BookingText.noticeLabel(1440), "1 day before")
        XCTAssertEqual(BookingText.noticeLabel(10080), "7 days before")
        XCTAssertEqual(BookingText.stepLabel(0), "Meeting length")
        XCTAssertEqual(BookingText.bufferLabel(0), "No gap")
        XCTAssertEqual(BookingText.dailyLimitLabel(0), "No limit")
        XCTAssertEqual(BookingText.locationLabel("custom"), "Other")
        XCTAssertEqual(BookingText.accentName("violet"), "Violet")
        XCTAssertEqual(BookingText.errorSentence("slug_taken"), "That link is taken. Try another.")
        XCTAssertEqual(BookingText.errorSentence("booking_locked"), "That meeting already happened or was cancelled.")
        XCTAssertEqual(BookingText.describe(PackagesError("invalid_meeting_url")), "A video call link must start with https://.")
        XCTAssertTrue(BookingText.isSlug("alice-2"))
        XCTAssertFalse(BookingText.isSlug("Alice"))
        XCTAssertFalse(BookingText.isSlug("-alice"))
        XCTAssertFalse(BookingText.isSlug(""))
        XCTAssertEqual(BookingSlugCheck(slug: "x", available: true, reason: "").message, "Available")
        XCTAssertEqual(BookingSlugCheck(slug: "x", available: false, reason: "slug_reserved").message, "That link is reserved. Try another.")
        XCTAssertEqual(BookingSlugCheck(slug: "x", available: false, reason: "").message, "Not available")
    }

    func testDaysGroupInTheTimeZone() {
        let data = BookingOverview(json: overview())
        let days = BookingText.days(data.upcoming, timeZone: london, locale: us)
        XCTAssertEqual(days.map(\.title), ["Monday, November 2", "Tuesday, November 3"])
        XCTAssertEqual(days.map(\.bookings.count), [1, 1])
        XCTAssertEqual(BookingText.timeRange(data.upcoming[0], timeFormat: "12h", timeZone: london, locale: us), "9:30 AM – 9:50 AM")
        XCTAssertEqual(BookingText.timeRange(data.upcoming[0], timeFormat: "24h", timeZone: london, locale: us), "09:30 – 09:50")
        let newYork = BookingText.days(data.upcoming, timeZone: TimeZone(identifier: "America/New_York")!, locale: us)
        XCTAssertEqual(newYork.first?.title, "Monday, November 2")
    }

    func testDraftRoundTripsAvailability() {
        var draft = BookingDraft(overview: BookingOverview(json: overview()))
        XCTAssertEqual(draft.windows("mon").last, BookingWindow(start: "13:00", end: "23:59"))
        XCTAssertTrue(draft.isOn("tue"))
        XCTAssertFalse(draft.isOn("wed"))
        draft.setDay("wed", on: true)
        XCTAssertEqual(draft.windows("wed"), [BookingWindow(start: "09:00", end: "17:00")])
        draft.addWindow("wed")
        XCTAssertEqual(draft.windows("wed").last, BookingWindow(start: "13:00", end: "17:00"))
        draft.setWindow("wed", at: 1, start: "18:00")
        draft.setDay("tue", on: false)
        draft.removeWindow("mon", at: 0)
        draft.removeWindow("mon", at: 5)
        draft.slug = " Alice-New "
        draft.dailyLimit = -3
        draft.addType()
        draft.types[2].title = " Deep dive "
        draft.removeType(at: 1)
        let body = draft.body
        XCTAssertEqual(body.str("slug"), "alice-new")
        XCTAssertEqual(body.int("dailyLimit"), 0)
        let week = body.obj("availability")
        XCTAssertEqual(week.list("mon").map(\.strings), [["13:00", "24:00"]])
        XCTAssertEqual(week.list("wed").map(\.strings), [["09:00", "17:00"]])
        XCTAssertEqual(week.list("tue").count, 0)
        XCTAssertEqual(week.list("fri").count, 0)
        XCTAssertEqual(week.keys, ["mon", "tue", "wed", "thu", "fri", "sat", "sun"])
        XCTAssertEqual(body.maps("types").map { $0.str("title") }, ["Intro call", "Deep dive"])
        XCTAssertEqual(body.maps("types").last?.str("id"), "")
        XCTAssertEqual(body.maps("types").first?.obj("location").str("detail"), "https://meet.example.com/ada")
        XCTAssertEqual(body.str("timeFormat"), "24h")
        XCTAssertFalse(body.bool("emailOwner"))
        XCTAssertTrue(body.bool("checkCalendar"))
        XCTAssertEqual(BookingDraft.minutes("24:00"), 1440)
        XCTAssertNil(BookingDraft.minutes("nine"))
        XCTAssertEqual(BookingDraft.clock(545), "09:05")
    }

    func testActivityDecodesAndSummarises() {
        var json = overview()
        let old = BookingOverview(json: json).activity
        XCTAssertEqual(old, PageActivity())
        XCTAssertTrue(old.isEmpty)
        XCTAssertEqual(old.summary(now: monday), "No visits in the last 30 days")
        XCTAssertEqual(old.newest, [])
        var recent: [JSONValue] = [
            [
                "kind": "form", "type": "intro", "detail": "", "label": "Opened the details form for Intro call",
                "visitor": "a3f9c1", "at": .int(monday - 7_200_000),
            ],
            ["kind": "view", "type": "", "label": "Opened your page", "visitor": "a3f9c1", "at": .int(monday - 7_300_000)],
            ["kind": "view", "label": "", "at": 1],
        ]
        for step in 1...4 {
            recent.append(["kind": "slot", "label": .string("Picked a time \(step)"), "visitor": "b0b0b0", "at": .int(monday - 86_400_000 * Int64(step))])
        }
        json["activity"] = [
            "days": 30, "views": 42, "visitors": 12, "interactions": 9, "starts": 5, "submits": 3, "leaves": 7,
            "lastActivityAt": .int(monday - 7_200_000), "recent": .array(recent),
        ]
        let activity = BookingOverview(json: json).activity
        XCTAssertEqual(activity.views, 42)
        XCTAssertEqual(activity.visitors, 12)
        XCTAssertEqual(activity.interactions, 9)
        XCTAssertEqual(activity.starts, 5)
        XCTAssertEqual(activity.submits, 3)
        XCTAssertEqual(activity.leaves, 7)
        XCTAssertFalse(activity.isEmpty)
        XCTAssertEqual(activity.recent.count, 6)
        XCTAssertEqual(activity.recent[0], PageVisit(kind: "form", type: "intro", label: "Opened the details form for Intro call", visitor: "a3f9c1", at: monday - 7_200_000))
        XCTAssertEqual(activity.summary(now: monday), "Last 30 days: 42 views · 12 visitors · 5 started · 3 sent · last activity 2h ago")
        XCTAssertEqual(activity.newest.map(\.label), ["Opened the details form for Intro call", "Opened your page", "Picked a time 1", "Picked a time 2", "Picked a time 3"])
        XCTAssertEqual(activity.newest.map { $0.when(now: monday) }, ["2h ago", "2h ago", "1d ago", "2d ago", "3d ago"])
        XCTAssertEqual(PageVisit(label: "x").when(now: monday), "")

        var single = PageActivity()
        single.days = 1
        single.views = 1
        single.visitors = 1
        XCTAssertEqual(single.summary(now: monday), "Last day: 1 view · 1 visitor · 0 started · 0 sent")
        var clicks = PageActivity()
        clicks.interactions = 2
        XCTAssertEqual(clicks.summary(now: monday), "Last 30 days: 0 views · 0 visitors · 0 started · 0 sent")
        XCTAssertEqual(PageActivity(json: ["days": 0, "views": -4]).summary(now: monday), "No visits in the last day")
    }

    func testNotifyVisitsRoundTrips() {
        var json = overview()
        let missing = BookingOverview(json: json)
        XCTAssertTrue(missing.settings.notifyVisits)
        XCTAssertTrue(BookingSettings().notifyVisits)
        XCTAssertEqual(BookingDraft(overview: missing).body.optBool("notifyVisits"), true)
        var settings = json.obj("settings")
        settings["notifyVisits"] = false
        json["settings"] = .object(settings)
        let off = BookingOverview(json: json)
        XCTAssertFalse(off.settings.notifyVisits)
        var draft = BookingDraft(overview: off)
        XCTAssertFalse(draft.notifyVisits)
        XCTAssertEqual(draft.body.optBool("notifyVisits"), false)
        draft.notifyVisits = true
        XCTAssertEqual(draft.body.optBool("notifyVisits"), true)
    }

    @MainActor func testClientPathsAndBodies() async throws {
        let transport = FakePackagesTransport()
        let client = PackagesClient(transport: transport)
        transport.on("GET", "/api/booking", overview())
        let data = try await client.bookingOverview()
        XCTAssertEqual(data.slug, "alice")

        transport.on("POST", "/api/booking/install", overview())
        _ = try await client.installBooking(slug: " Alice ", timezone: "Europe/London")
        XCTAssertEqual(transport.last("POST", "/api/booking/install")?.body?.str("slug"), "alice")
        XCTAssertEqual(transport.last("POST", "/api/booking/install")?.body?.str("timezone"), "Europe/London")

        transport.on("PUT", "/api/booking/page", overview())
        _ = try await client.saveBooking(["enabled": false])
        XCTAssertEqual(transport.last("PUT", "/api/booking/page")?.body?.optBool("enabled"), false)

        transport.on("POST", "/api/booking/bookings/\(hexA)/cancel", overview())
        _ = try await client.cancelBooking(id: hexA, reason: "Sorry", notifyGuest: false)
        let cancel = transport.last("POST", "/api/booking/bookings/\(hexA)/cancel")?.body
        XCTAssertEqual(cancel?.str("reason"), "Sorry")
        XCTAssertEqual(cancel?.optBool("notifyGuest"), false)
        do {
            _ = try await client.cancelBooking(id: "../x", reason: "", notifyGuest: true)
            XCTFail("expected a refusal")
        } catch {}

        transport.on("GET", "/api/booking/slug/fresh", ["ok": true, "slug": "fresh", "available": true, "reason": ""])
        let fresh = try await client.checkBookingSlug("Fresh")
        XCTAssertTrue(fresh.available)
        let local = try await client.checkBookingSlug("not ok")
        XCTAssertEqual(local.reason, "invalid_slug")
        XCTAssertFalse(transport.paths.contains("GET /api/booking/slug/not ok"))

        transport.on("GET", "/api/booking/export", ["ok": true, "filename": "forkmesh-booking-alice.json", "export": ["slug": "alice"]])
        let export = try await client.exportBooking()
        XCTAssertEqual(export.filename, "forkmesh-booking-alice.json")
        XCTAssertTrue(export.json.contains("\"slug\""))

        transport.on("POST", "/api/booking/uninstall", ["ok": true, "uninstalled": true])
        try await client.uninstallBooking()
        XCTAssertEqual(transport.last("POST", "/api/booking/uninstall")?.body, JSONObject())

        transport.routes["PUT /api/booking/page"] = nil
        transport.on("PUT", "/api/booking/page", status: 409, ["error": "slug_taken"])
        do {
            _ = try await client.saveBooking(["slug": "bob"])
            XCTFail("expected slug_taken")
        } catch {
            XCTAssertEqual(BookingText.describe(error), "That link is taken. Try another.")
        }
    }

    @MainActor func testModelInstallSaveCancelAndErrors() async {
        let transport = FakePackagesTransport()
        let model = BookingModel(client: PackagesClient(transport: transport))
        transport.on("GET", "/api/booking", ["ok": true, "installed": false, "suggestedSlug": "alice"])
        await model.refresh()
        XCTAssertFalse(model.installed)
        XCTAssertEqual(model.overview?.suggestedSlug, "alice")

        transport.on("POST", "/api/booking/install", overview())
        let created = await model.install(slug: "alice", timezone: "Europe/London")
        XCTAssertTrue(created)
        XCTAssertTrue(model.installed)
        XCTAssertEqual(model.notice, "Your booking page is live. Copy the link and send it to anyone.")

        transport.on("PUT", "/api/booking/page", overview())
        let paused = await model.setEnabled(false)
        XCTAssertTrue(paused)
        XCTAssertTrue(model.notice.hasPrefix("Your page is paused."))

        transport.on("POST", "/api/booking/bookings/\(hexA)/cancel", overview())
        let booking = model.overview!.upcoming[0]
        _ = await model.cancel(booking, reason: "", notifyGuest: true)
        XCTAssertEqual(model.notice, "Cancelled. We emailed Grace Hopper.")

        transport.on("POST", "/api/booking/bookings/\(hexB)/cancel", status: 409, ["error": "booking_locked"])
        let failed = await model.cancel(model.overview!.upcoming[1], reason: "", notifyGuest: false)
        XCTAssertFalse(failed)
        XCTAssertEqual(model.errorText, "Could not cancel: That meeting already happened or was cancelled.")
        XCTAssertEqual(model.notice, "")
    }

    func testPackageRoutesAndMenu() {
        XCTAssertEqual(PackageRoutes.route(for: MarketPackage(id: "booking", name: "Booking Page")), .booking(id: nil))
        XCTAssertEqual(PluginMenu.packageId(for: .booking(id: hexA)), "booking")
        XCTAssertTrue(PluginMenu.addsButton(for: .booking(id: nil)))
        XCTAssertFalse(PackageGate.organizationPackages.contains("booking"))
    }
}
