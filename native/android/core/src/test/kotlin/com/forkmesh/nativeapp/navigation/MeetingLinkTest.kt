package com.forkmesh.nativeapp.navigation

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class MeetingLinkTest {
    private val room = "0123456789abcdef0123456789abcdef"
    private val key = "ab".repeat(32)
    private val meeting = Route.Meeting(room, key)

    fun testUniversalAndCustomLinksOpenTheMeeting() {
        assertEq(listOf(meeting), RouteParser.parse("https://app.forkmesh.com/call?$room#$key")!!.stack)
        assertEq(listOf(meeting), RouteParser.parse("https://forkmesh.com/call/?${room.uppercase()}#${key.uppercase()}")!!.stack)
        assertEq("app.forkmesh.com", RouteParser.parse("https://forkmesh.com/call?$room#$key")!!.project)
        assertEq(listOf(meeting), RouteParser.parse("forkmesh://call?$room#$key")!!.stack)
        expect(RouteParser.parse("https://app.forkmesh.com/call?$room") == null)
        expect(RouteParser.parse("https://app.forkmesh.com/call?nothex#$key") == null)
        expect(RouteParser.parse("https://app.forkmesh.com/call?$room#short") == null)
        expect(RouteParser.parse("https://evil.example/call?$room#$key") == null)
        expect(RouteParser.parse("https://app.forkmesh.com/call/extra") == null)
    }

    fun testBookingLinksOnAnyServerAndThePage() {
        assertEq(meeting, MeetingLink.route("https://meet.example.org/call?$room#$key"))
        expect(MeetingLink.route("http://meet.example.org/call?$room#$key") == null)
        expect(MeetingLink.route("https://meet.example.org/other?$room#$key") == null)
        assertEq("https://app.forkmesh.com/call?$room#$key", MeetingLink.pageUrl("https://app.forkmesh.com", meeting))
        expect(MeetingLink.isMeetingPage("https://app.forkmesh.com/call?$room#$key", "https://app.forkmesh.com"))
        expect(!MeetingLink.isMeetingPage("https://app.forkmesh.com/chat", "https://app.forkmesh.com"))
        expect(!MeetingLink.isMeetingPage("https://evil.example/call?$room#$key", "https://app.forkmesh.com"))
        expect(MeetingLink.sameOrigin("https://app.forkmesh.com", "https://app.forkmesh.com"))
        expect(!MeetingLink.sameOrigin("https://app.forkmesh.com.evil.example", "https://app.forkmesh.com"))
    }

    fun testTheRouteRoundTripsAndHasATitle() {
        assertEq(meeting, RouteCodec.decode(RouteCodec.encode(meeting)))
        assertEq("Meeting", RouteCatalog.title(Route.Kind.Meeting))
        assertEq("videocam", RouteCatalog.icon(Route.Kind.Meeting))
        expect(GuestGate.action(meeting, guest = true) == null)
    }
}
