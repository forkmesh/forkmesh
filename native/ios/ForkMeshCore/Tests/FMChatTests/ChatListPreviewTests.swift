import FMAccount
import FMApi
import FMBase
import FMNet
import Foundation
import XCTest

@testable import FMChat

final class ChatListPreviewTests: XCTestCase {
    func message(_ id: String, _ text: String, sender: String = "Pat", isSelf: Bool = false, file: String = "", at: TimeInterval = 0)
        -> ChatMessage
    {
        ChatMessage(
            id: id, conversation: "#general", senderId: isSelf ? "me" : "pat-node", senderName: sender, text: text,
            timestamp: Date(timeIntervalSince1970: 1_000 + at), isSelf: isSelf, fileName: file)
    }

    func testFormattingIsReducedToOneLineOfPlainText() {
        XCTAssertEqual(ChatListPreviews.plain("*bold* and _italic_ ~gone~ `code`"), "bold and italic gone code")
        XCTAssertEqual(ChatListPreviews.plain("> quoted line\nanswer\n\n  more  "), "quoted line answer more")
        XCTAssertEqual(ChatListPreviews.plain("```\nlet x = 1\n```"), "let x = 1")
        XCTAssertEqual(ChatListPreviews.plain("snake_case_name stays"), "snake_case_name stays")
        let long = ChatListPreviews.plain(String(repeating: "word ", count: 100))
        XCTAssertEqual(long.count, ChatListPreviews.maxChars)
        XCTAssertTrue(long.hasSuffix("\u{2026}"))
    }

    func testLinesNameTheSenderInChannelsAndYouForYourOwn() {
        XCTAssertEqual(ChatListPreviews.line(message("1", "hi"), text: "hi", direct: false), "Pat: hi")
        XCTAssertEqual(ChatListPreviews.line(message("1", "hi"), text: "hi", direct: true), "hi")
        XCTAssertEqual(ChatListPreviews.line(message("1", "hi", isSelf: true), text: "hi", direct: true), "You: hi")
        XCTAssertEqual(ChatListPreviews.line(message("1", "hi", isSelf: true), text: "hi", direct: false), "You: hi")
        XCTAssertEqual(ChatListPreviews.line(message("1", "", file: "plan.pdf"), text: "", direct: true), "\u{1F4CE} plan.pdf")
        XCTAssertEqual(ChatListPreviews.line(message("1", "", file: "plan.pdf"), text: "", direct: false), "Pat: \u{1F4CE} plan.pdf")
        XCTAssertEqual(ChatListPreviews.line(message("1", "see this", file: "plan.pdf"), text: "see this", direct: true), "see this")
        XCTAssertEqual(ChatListPreviews.line(message("1", "  "), text: "  ", direct: true), "")
    }

    func testThePreviewIsTheNewestMessageThatIsNotSkipped() {
        let list = [message("1", "first", at: 1), message("2", "second", at: 2), message("3", "", at: 3), message("4", "gone", at: 4)]
        let preview = ChatListPreviews.preview(list, direct: false, text: { $0.text }, skip: { $0.id == "4" })
        XCTAssertEqual(preview?.text, "Pat: second")
        XCTAssertEqual(preview?.at, Date(timeIntervalSince1970: 1_002))
        XCTAssertNil(ChatListPreviews.preview([], direct: true, text: { $0.text }))
        let edited = ChatListPreviews.preview(list, direct: true, text: { $0.id == "4" ? "fixed" : $0.text })
        XCTAssertEqual(edited?.text, "fixed")
    }

    func testTimesAreShortForTheList() {
        let utc = TimeZone(identifier: "UTC")!
        let now = Date(timeIntervalSince1970: 1_791_900_000)
        XCTAssertEqual(ChatListPreviews.timeLabel(now.addingTimeInterval(-60), now: now, timeZone: utc), ChatHub.timeLabel(now.addingTimeInterval(-60), timeZone: utc))
        XCTAssertEqual(ChatListPreviews.timeLabel(now.addingTimeInterval(120), now: now, timeZone: utc), ChatHub.timeLabel(now.addingTimeInterval(120), timeZone: utc))
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = utc
        let threeDays = now.addingTimeInterval(-3 * 86_400)
        let weekday = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"][calendar.component(.weekday, from: threeDays) - 1]
        XCTAssertEqual(ChatListPreviews.timeLabel(threeDays, now: now, timeZone: utc), weekday)
        let earlier = calendar.date(from: DateComponents(year: 2026, month: 10, day: 3, hour: 9))!
        let later = calendar.date(from: DateComponents(year: 2026, month: 10, day: 20, hour: 9))!
        XCTAssertEqual(ChatListPreviews.timeLabel(earlier, now: later, timeZone: utc), "Oct 3")
        let lastYear = calendar.date(from: DateComponents(year: 2025, month: 3, day: 14, hour: 9))!
        XCTAssertEqual(ChatListPreviews.timeLabel(lastYear, now: later, timeZone: utc), "Mar 14, 2025")
    }

    @MainActor private func hub() async throws -> (ChatHub, RelayHarness) {
        let h = try await RelayHarness.start()
        let mesh = LocalMeshService(settings: h.device.settings, identity: h.device.identity, secureStore: h.device.secure)
        return (ChatHub(relay: h.relay, mesh: mesh), h)
    }

    private func peerFrame(_ type: String, _ fields: JSONObject, id: String = UUID().uuidString, sender: String = "peer-1")
        -> JSONObject
    {
        var frame: JSONObject = [
            "type": .string(type), "id": .string(id), "senderId": .string(sender), "sender": "Pat",
            "ts": .int(Date().epochMillis),
        ]
        for (key, value) in fields { frame[key] = value }
        return frame
    }

    func testQuietRowsSayWhatTheConversationIs() {
        XCTAssertEqual(
            ChatListPreviews.fallback(topic: "", isPrivate: false, members: 0, mesh: true, direct: false),
            "Phones and desktops on this network")
        XCTAssertEqual(
            ChatListPreviews.fallback(topic: " Weekly *design* review ", isPrivate: true, members: 4, mesh: false, direct: false),
            "Weekly design review")
        XCTAssertEqual(
            ChatListPreviews.fallback(topic: "", isPrivate: false, members: 0, mesh: false, direct: false),
            "Public \u{00B7} no messages yet")
        XCTAssertEqual(
            ChatListPreviews.fallback(topic: "", isPrivate: true, members: 1, mesh: false, direct: false), "Private \u{00B7} 1 member")
        XCTAssertEqual(
            ChatListPreviews.fallback(topic: "", isPrivate: true, members: 0, mesh: false, direct: false),
            "Private \u{00B7} invited people only")
        XCTAssertEqual(ChatListPreviews.fallback(topic: "", isPrivate: false, members: 0, mesh: false, direct: true), "No messages yet")
    }

    func testConnectedAppsGroupTheirCommandsByApp() {
        let apps = ChatListPreviews.apps([
            AppCommand(command: "deploy", appName: "Shipper", appSlug: "shipper"),
            AppCommand(command: "hello", appName: "Hello App", appSlug: "hello-app"),
            AppCommand(command: "rollback", appName: "Shipper", appSlug: "shipper"),
            AppCommand(command: "status", appName: "Shipper", appSlug: "shipper"),
            AppCommand(command: "logs", appName: "Shipper", appSlug: "shipper"),
            AppCommand(command: "ping"),
        ])
        XCTAssertEqual(apps.map(\.name), ["Apps", "Hello App", "Shipper"])
        XCTAssertEqual(apps.map(\.id), ["apps", "hello-app", "shipper"])
        XCTAssertEqual(apps[1].detail, "/hello")
        XCTAssertEqual(apps[2].detail, "/deploy /rollback /status +1")
        XCTAssertEqual(ChatListPreviews.apps([]), [])
    }

    @MainActor func testAChannelRowShowsItsNewestTopLevelMessage() async throws {
        let (hub, h) = try await hub()
        XCTAssertNil(hub.listPreview("#general"))
        try h.sendPlain(peerFrame("chat", ["channel": "#general", "text": "*lunch* at noon?"], id: "m-1"))
        let shown = await waitUntil { hub.listPreview("#general")?.text == "Pat: lunch at noon?" }
        XCTAssertTrue(shown)
        try await h.sendProtected(peerFrame("thread-reply", ["channel": "#general", "rootId": "m-1", "text": "in a thread"], id: "r-1"))
        let threaded = await waitUntil { hub.extras.replies(to: "m-1").count == 1 }
        XCTAssertTrue(threaded)
        XCTAssertEqual(hub.listPreview("#general")?.text, "Pat: lunch at noon?", "thread replies stay out of the list line")
        try await h.sendProtected(peerFrame("edit", ["conversation": "#general", "target": "m-1", "text": "lunch at one?"]))
        let edited = await waitUntil { hub.listPreview("#general")?.text == "Pat: lunch at one?" }
        XCTAssertTrue(edited)
        _ = await hub.deliver("on my way", to: "#general")
        XCTAssertEqual(hub.listPreview("#general")?.text, "You: on my way")
        let mine = try XCTUnwrap(hub.messages(in: "#general").last)
        hub.hide(mine)
        XCTAssertEqual(hub.listPreview("#general")?.text, "Pat: lunch at one?")
        XCTAssertNil(hub.listPreview("#random"))
        await h.close()
    }
}
