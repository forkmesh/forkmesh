import Foundation
import XCTest

@testable import FMApi

final class MeetingRouteTests: XCTestCase {
    private let room = "0123456789abcdef0123456789abcdef"
    private let key = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"

    private func parse(_ link: String) -> ParsedLink? {
        RouteParser.parse(URL(string: link)!)
    }

    func testUniversalLinksOpenTheMeeting() {
        let meeting = Route.meeting(room: room, key: key)
        XCTAssertEqual(parse("https://app.forkmesh.com/call?\(room)#\(key)")?.stack, [meeting])
        XCTAssertEqual(parse("https://forkmesh.com/call/?\(room)#\(key)")?.stack, [meeting])
        XCTAssertEqual(parse("https://app.forkmesh.com/call?\(room.uppercased())#\(key.uppercased())")?.stack, [meeting])
        let link = parse("https://app.forkmesh.com/call?\(room)#\(key)")
        XCTAssertEqual(link?.project, RouteParser.projectId(forHost: "app.forkmesh.com"))
        XCTAssertEqual(link?.explicitProject, false)
    }

    func testCustomSchemeOpensTheMeeting() {
        let meeting = Route.meeting(room: room, key: key)
        XCTAssertEqual(parse("forkmesh://call?\(room)#\(key)")?.stack, [meeting])
        XCTAssertEqual(parse("forkmesh://CALL/?\(room.uppercased())#\(key)")?.stack, [meeting])
        XCTAssertNil(parse("forkmesh://call?\(room)")?.stack)
        XCTAssertNil(parse("forkmesh://call?nothex#\(key)")?.stack)
    }

    func testInvalidLinksFallBackToTheBrowser() {
        XCTAssertNil(parse("https://app.forkmesh.com/call"))
        XCTAssertNil(parse("https://app.forkmesh.com/call?\(room)"))
        XCTAssertNil(parse("https://app.forkmesh.com/call#\(key)"))
        XCTAssertNil(parse("https://app.forkmesh.com/call?\(room.dropLast())#\(key)"))
        XCTAssertNil(parse("https://app.forkmesh.com/call?\(room)#\(key)00"))
        XCTAssertNil(parse("https://app.forkmesh.com/call?\(room.dropLast())g#\(key)"))
        XCTAssertNil(parse("https://app.forkmesh.com/call?room=\(room)#\(key)"))
        XCTAssertNil(parse("https://app.forkmesh.com/call/extra?\(room)#\(key)"))
        XCTAssertNil(parse("https://example.com/call?\(room)#\(key)"))
        XCTAssertNil(parse("http://app.forkmesh.com/call?\(room)#\(key)"))
    }

    func testRouteFromAnyHttpsHost() {
        let meeting = Route.meeting(room: room, key: key)
        XCTAssertEqual(MeetingLink.route(from: URL(string: "https://code.example.org/call?\(room)#\(key)")!), meeting)
        XCTAssertEqual(MeetingLink.route(from: URL(string: "https://code.example.org:8443/call/?\(room)#\(key.uppercased())")!), meeting)
        XCTAssertNil(MeetingLink.route(from: URL(string: "http://code.example.org/call?\(room)#\(key)")!))
        XCTAssertNil(MeetingLink.route(from: URL(string: "https://code.example.org/meet?\(room)#\(key)")!))
        XCTAssertNil(MeetingLink.route(from: URL(string: "https://code.example.org/call?\(room)")!))
        XCTAssertNil(MeetingLink.route(from: URL(string: "https://meet.example.com/you")!))
    }

    func testPageURL() {
        let page = MeetingLink.pageURL(origin: URL(string: "https://app.forkmesh.com")!, room: room, key: key)
        XCTAssertEqual(page.absoluteString, "https://app.forkmesh.com/call?\(room)#\(key)")
        let ported = MeetingLink.pageURL(origin: URL(string: "https://Code.Example.org:8443/")!, room: room, key: key)
        XCTAssertEqual(ported.absoluteString, "https://code.example.org:8443/call?\(room)#\(key)")
        let plain = MeetingLink.pageURL(origin: URL(string: "https://code.example.org:443")!, room: room, key: key)
        XCTAssertEqual(plain.absoluteString, "https://code.example.org/call?\(room)#\(key)")
        XCTAssertEqual(MeetingLink.route(from: page), .meeting(room: room, key: key))
    }

    func testOrigin() {
        XCTAssertEqual(MeetingLink.origin(of: URL(string: "https://App.ForkMesh.com/some/path?x=1")!)?.absoluteString, "https://app.forkmesh.com")
        XCTAssertEqual(MeetingLink.origin(of: URL(string: "https://code.example.org:8443/")!)?.absoluteString, "https://code.example.org:8443")
        XCTAssertEqual(MeetingLink.origin(of: URL(string: "https://code.example.org:443/")!)?.absoluteString, "https://code.example.org")
        XCTAssertNil(MeetingLink.origin(of: URL(string: "http://localhost:8787/")!))
    }

    func testIsMeetingPage() {
        let origin = URL(string: "https://app.forkmesh.com")!
        XCTAssertTrue(MeetingLink.isMeetingPage(URL(string: "https://app.forkmesh.com/call?\(room)#\(key)")!, origin: origin))
        XCTAssertTrue(MeetingLink.isMeetingPage(URL(string: "https://APP.forkmesh.com:443/call/")!, origin: origin))
        XCTAssertFalse(MeetingLink.isMeetingPage(URL(string: "https://evil.example.com/call?\(room)#\(key)")!, origin: origin))
        XCTAssertFalse(MeetingLink.isMeetingPage(URL(string: "https://app.forkmesh.com/dashboard")!, origin: origin))
        XCTAssertFalse(MeetingLink.isMeetingPage(URL(string: "https://app.forkmesh.com/call/x")!, origin: origin))
        XCTAssertFalse(MeetingLink.isMeetingPage(URL(string: "http://app.forkmesh.com/call")!, origin: origin))
        XCTAssertFalse(MeetingLink.isMeetingPage(URL(string: "https://app.forkmesh.com:8443/call")!, origin: origin))
        XCTAssertTrue(MeetingLink.sameOrigin(scheme: "https", host: "App.ForkMesh.com", port: nil, origin: origin))
        XCTAssertFalse(MeetingLink.sameOrigin(scheme: "https", host: "", port: nil, origin: origin))
    }

    func testSaveNameAndText() {
        XCTAssertEqual(MeetingLink.saveName("Intro call 2026-11-04.txt"), "Intro call 2026-11-04.txt")
        XCTAssertEqual(MeetingLink.saveName(nil), MeetingLink.defaultSaveName)
        XCTAssertEqual(MeetingLink.saveName("   "), MeetingLink.defaultSaveName)
        XCTAssertEqual(MeetingLink.saveName("../../etc/passwd"), "-..-etc-passwd")
        XCTAssertEqual(MeetingLink.saveName("..."), MeetingLink.defaultSaveName)
        XCTAssertEqual(MeetingLink.saveName("a\nb:c\\d"), "a-b-c-d")
        XCTAssertEqual(MeetingLink.saveName(String(repeating: "x", count: 300)).count, MeetingLink.maxSaveNameLength)
        XCTAssertEqual(MeetingLink.saveText("hello"), "hello")
        let long = String(repeating: "é", count: MeetingLink.maxSaveBytes)
        let capped = MeetingLink.saveText(long)
        XCTAssertLessThanOrEqual(capped.utf8.count, MeetingLink.maxSaveBytes)
        XCTAssertEqual(capped.count, MeetingLink.maxSaveBytes / 2)
    }

    func testMeetingRouteContract() throws {
        let route = Route.meeting(room: room, key: key)
        XCTAssertEqual(route.kind, .meeting)
        XCTAssertEqual(Route.Kind.meeting.rawValue, "meeting")
        XCTAssertEqual(Route.Kind.meeting.owner, "WS13")
        XCTAssertEqual(route.title, "Meeting")
        XCTAssertEqual(RouteCatalog.title(.meeting), "Meeting")
        XCTAssertEqual(RouteCatalog.symbol(.meeting), "video")
        XCTAssertNil(route.homeTab)
        XCTAssertFalse(route.isModal)
        XCTAssertNil(PackageGate.packageId(for: route))
        XCTAssertTrue(PackageGate.shows(route, installed: []))
        XCTAssertFalse(HubCatalog.routes.contains { $0.kind == .meeting })
        XCTAssertEqual(RouteParser.stack(for: route), [route])
        let data = try JSONEncoder().encode([route])
        XCTAssertEqual(try JSONDecoder().decode([Route].self, from: data), [route])
    }
}
