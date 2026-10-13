import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

@MainActor
final class AppsCallRecorder {
    var calls: [(String, String, JSONObject?)] = []
    var replies: [String: JSONObject] = [:]
    var failures: [String: Error] = [:]

    var client: AppsClient {
        AppsClient { [self] method, path, body in
            calls.append((method, path, body))
            let key = "\(method) \(path)"
            if let failure = failures[key] { throw failure }
            return replies[key] ?? [:]
        }
    }
}

@MainActor
final class AppsTests: XCTestCase {
    static let installed: JSONObject = [
        "ok": true, "account": "alice",
        "installs": [
            [
                "installId": "i1",
                "app": ["id": "a1", "slug": "hello-app", "name": "Hello App", "botName": "Hello App (app)",
                        "description": "Says hi", "developer": "dev"],
                "target": ["kind": "user", "name": "alice"],
                "scopes": [["scope": "commands", "label": "Slash commands", "hint": "Run /commands"], ["scope": ""]],
                "installedBy": "alice", "createdAt": 1000, "lastUsedAt": 0, "canRevoke": true,
            ],
            ["installId": "", "app": ["id": "x"]],
        ],
        "organizations": [
            [
                "name": "acme", "role": "admin", "canManage": true,
                "installs": [
                    ["installId": "i2", "app": ["id": "a2", "slug": "ci", "name": "CI"],
                     "target": ["kind": "org", "name": "acme"], "scopes": [], "lastUsedAt": 1_000, "canRevoke": true]
                ],
                "requests": [
                    ["id": "r1", "app": ["id": "a3", "slug": "notes", "name": "Notes"],
                     "scopes": [["scope": "chat:read", "label": "Read chat", "hint": ""]], "requester": "bob", "createdAt": 5]
                ],
            ],
            ["name": "globex", "role": "member", "canManage": false, "installs": [], "requests": []],
            ["role": "member"],
        ],
    ]

    func testInstalledAppsAreParsedWithOrganizationsAndRequests() async throws {
        let recorder = AppsCallRecorder()
        recorder.replies["GET /api/apps/installed"] = AppsTests.installed
        let apps = try await recorder.client.installed()
        XCTAssertEqual(apps.account, "alice")
        XCTAssertEqual(apps.installs.map(\.installId), ["i1"])
        let hello = apps.installs[0]
        XCTAssertEqual(hello.app.title, "Hello App")
        XCTAssertEqual(hello.app.botName, "Hello App (app)")
        XCTAssertEqual(hello.targetLabel, "Just me")
        XCTAssertEqual(hello.scopes.map(\.title), ["Slash commands"])
        XCTAssertEqual(hello.lastUsedText(nowMillis: 10_000), "Never used")
        XCTAssertTrue(hello.canRevoke)
        XCTAssertEqual(apps.organizations.map(\.name), ["acme", "globex"])
        let acme = apps.organizations[0]
        XCTAssertTrue(acme.canManage)
        XCTAssertEqual(acme.installs[0].targetLabel, "acme")
        XCTAssertEqual(acme.installs[0].lastUsedText(nowMillis: 1_000 + 3 * 3_600_000), "Last used 3h ago")
        XCTAssertEqual(acme.requests.map(\.requester), ["bob"])
        XCTAssertEqual(acme.requests[0].scopes.map(\.title), ["Read chat"])
        XCTAssertEqual(apps.pendingRequests, 1)
        XCTAssertFalse(apps.isEmpty)
        XCTAssertTrue(InstalledApps(json: ["installs": [], "organizations": [["name": "x"]]]).isEmpty)
    }

    func testBlocksAreParsedDefensively() {
        let blocks: [JSONValue] = [
            ["type": "section", "text": "Hello", "fields": .array((0..<12).map { .string("f\($0)") })],
            ["type": "section", "text": ["type": "plain_text", "text": "Wrapped"]],
            ["type": "section"],
            ["type": "context", "text": "small print"],
            ["type": "divider"],
            ["type": "image", "url": "https://example.com/x.png"],
            [
                "type": "actions",
                "elements": [
                    ["type": "button", "action_id": "wave", "text": "Wave", "value": "w", "style": "primary"],
                    ["type": "button", "action_id": "wave", "text": "Again"],
                    ["type": "button", "action_id": "stop", "text": "Stop", "style": "loud"],
                    ["type": "button", "action_id": "blank", "text": ""],
                    ["type": "static_select", "action_id": "pick",
                     "options": [["text": "One", "value": "1"], ["text": "", "value": "2"], ["text": "Three", "value": ""]]],
                    ["type": "static_select", "action_id": "empty", "options": []],
                    ["type": "datepicker", "action_id": "date"],
                ],
            ],
            ["type": "actions", "elements": [["type": "overflow", "action_id": "o"]]],
            "not a block",
        ]
        let parsed = AppsChat.blocks(blocks)
        XCTAssertEqual(parsed.count, 5)
        guard case let .section(text, fields) = parsed[0] else { return XCTFail("\(parsed[0])") }
        XCTAssertEqual(text, "Hello")
        XCTAssertEqual(fields.count, AppsChat.maxFields)
        XCTAssertEqual(parsed[1], .section(text: "Wrapped", fields: []))
        XCTAssertEqual(parsed[2], .context(text: "small print"))
        XCTAssertEqual(parsed[3], .divider)
        XCTAssertEqual(
            parsed[4],
            .actions([
                .button(actionId: "wave", text: "Wave", value: "w", style: .primary),
                .button(actionId: "stop", text: "Stop", value: "", style: .plain),
                .select(actionId: "pick", placeholder: "Choose", options: [AppSelectOption(text: "One", value: "1")]),
            ]))
        let many: [JSONValue] = Array(repeating: ["type": "divider"], count: 30)
        XCTAssertEqual(AppsChat.blocks(many).count, AppsChat.maxBlocks)
    }

    func testBotFramesBecomeAppMessages() {
        let frame: JSONObject = [
            "type": "chat", "id": "m1", "senderId": "app:hello-app", "sender": "Hello App (app)", "text": "Hi",
            "app": ["id": "a1", "slug": "hello-app", "name": "Hello App"],
            "blocks": [["type": "actions", "elements": [["type": "button", "action_id": "wave", "text": "Wave"]]]],
            "appMessageId": "m1",
        ]
        let message = AppsChat.message(frame, channelId: "c1")
        XCTAssertEqual(message?.appMessageId, "m1")
        XCTAssertEqual(message?.channelId, "c1")
        XCTAssertEqual(message?.appName, "Hello App")
        XCTAssertEqual(message?.interactive, true)
        XCTAssertEqual(message?.ephemeral, false)
        var person = frame
        person["senderId"] = "alice-node"
        XCTAssertNil(AppsChat.message(person, channelId: "c1"))
        var plain = frame
        plain["blocks"] = []
        XCTAssertNil(AppsChat.message(plain, channelId: "c1"))
    }

    func testSlashCommandsAndMentionsMatchTheServer() {
        XCTAssertEqual(AppsChat.parseSlash("  /Hello  world  ")?.command, "hello")
        XCTAssertEqual(AppsChat.parseSlash("/hello world\nline")?.arguments, "world\nline")
        XCTAssertEqual(AppsChat.parseSlash("/deploy-2")?.arguments, "")
        XCTAssertNil(AppsChat.parseSlash("/x"))
        XCTAssertNil(AppsChat.parseSlash("/9lives"))
        XCTAssertNil(AppsChat.parseSlash("hello /there"))
        XCTAssertEqual(AppsChat.mentionedSlugs("hey @Hello-App and @ci-bot, @hello-app again"), ["hello-app", "ci-bot"])
        XCTAssertEqual(AppsChat.mentionedSlugs("mail me@hello-app.com or a/@ci-bot"), [])
        XCTAssertEqual(AppsChat.mentionedSlugs("@hello- @ci"), [])
        XCTAssertTrue(AppsChat.shouldForwardMentions("ping @ci-bot", mentionable: ["CI-Bot"]))
        XCTAssertFalse(AppsChat.shouldForwardMentions("ping @bob", mentionable: ["ci-bot"]))
    }

    func testCommandCatalogSuggestionsAndCopy() {
        let catalog = AppCommandCatalog(json: [
            "commands": [
                ["command": "hello", "description": "Say hi", "usageHint": "[name]",
                 "app": ["name": "Hello App", "slug": "hello-app"], "installId": "i1"],
                ["command": "hello", "description": "dup", "app": ["name": "Other"]],
                ["command": "Bad Command", "app": ["name": "x"]],
                ["command": "help-me", "app": ["slug": "helper"]],
            ],
            "mentionable": ["Hello-App"],
        ])
        XCTAssertEqual(catalog.commands.map(\.command), ["hello", "help-me"])
        XCTAssertEqual(catalog.mentionable, ["hello-app"])
        XCTAssertEqual(catalog.commands[0].usage, "/hello [name]")
        XCTAssertEqual(catalog.commands[0].summary, "Say hi · Hello App")
        XCTAssertEqual(catalog.commands[1].summary, "helper")
        XCTAssertEqual(AppsChat.suggestions("/he", commands: catalog.commands).map(\.command), ["hello", "help-me"])
        XCTAssertEqual(AppsChat.suggestions("/a", commands: catalog.commands).map(\.command), ["apps"])
        XCTAssertEqual(AppsChat.suggestions("/", commands: catalog.commands).map(\.command), ["hello", "help-me", "apps"])
        XCTAssertEqual(AppsChat.suggestions("/hello there", commands: catalog.commands), [])
        XCTAssertEqual(AppsChat.suggestions("hello", commands: catalog.commands), [])
        XCTAssertEqual(
            AppsChat.commandsText(catalog.commands), "Your app commands: /hello (Hello App), /help-me (helper)")
        XCTAssertEqual(AppsChat.commandsText([]), "You have no app commands. Install apps from /dashboard/apps.")
    }

    func testRepliesAndErrorsUseTheWebCopy() async {
        let recorder = AppsCallRecorder()
        recorder.replies["POST /api/apps/commands/run"] = [
            "ok": true, "responseType": "ephemeral", "text": "Hi alice",
            "app": ["name": "Hello App", "botName": "Hello App (app)", "slug": "hello-app"],
            "blocks": [["type": "divider"]], "appMessageId": "e1",
        ]
        let reply = await recorder.client.run(text: "/hello", channelId: "c1")
        XCTAssertTrue(reply.isEphemeral)
        XCTAssertEqual(reply.sender, "Hello App (app)")
        XCTAssertEqual(reply.blocks, [.divider])
        XCTAssertEqual(reply.appMessageId, "e1")
        XCTAssertEqual(recorder.calls.last?.2, ["text": "/hello", "channelId": "c1"])

        recorder.failures["POST /api/apps/interact"] = OrgApiException("rate_limited", "rate limited")
        let limited = await recorder.client.interact(appMessageId: "e1", channelId: "", actionId: "wave", value: "w")
        XCTAssertFalse(limited.ok)
        XCTAssertEqual(limited.errorText, "Too many requests to that app; try again in a minute.")
        XCTAssertEqual(
            recorder.calls.last?.2, ["appMessageId": "e1", "channelId": "", "actionId": "wave", "value": "w"])

        XCTAssertEqual(AppReply(json: ["ok": false, "error": "app_unavailable", "app": "Hello App"]).errorText, "Hello App did not answer.")
        XCTAssertEqual(AppsChat.errorText("app_unavailable"), "The app did not answer.")
        XCTAssertEqual(AppsChat.errorText("unknown_command"), "No installed app has that command. Type /apps to see yours.")
        XCTAssertEqual(AppsChat.errorText("message_expired"), "That message is too old to use.")
        XCTAssertEqual(AppsChat.errorText("weird"), "The app could not handle that (weird).")
        XCTAssertEqual(AppReply(json: ["ok": true, "responseType": "in_channel"]).isEphemeral, false)
        XCTAssertEqual(AppReply(json: ["responseType": "ephemeral", "text": "x"]).sender, "App")
    }

    func testRevokeDecideAndMentionsSendTheWireShape() async throws {
        let recorder = AppsCallRecorder()
        recorder.replies["POST /api/apps/orgs/acme%20co/requests/r%2F1"] = ["ok": true, "status": "approved"]
        recorder.replies["POST /api/apps/mentions"] = ["ok": true, "forwarded": 2]
        try await recorder.client.revoke(installId: "i1")
        let status = try await recorder.client.decide(org: "Acme Co", requestId: "r/1", approve: true)
        let forwarded = await recorder.client.forwardMentions(channelId: "c1", messageId: "m1", text: "@ci hi")
        XCTAssertEqual(status, "approved")
        XCTAssertEqual(forwarded, 2)
        XCTAssertEqual(recorder.calls.map { "\($0.0) \($0.1)" }, [
            "DELETE /api/apps/installs/i1",
            "POST /api/apps/orgs/acme%20co/requests/r%2F1",
            "POST /api/apps/mentions",
        ])
        XCTAssertEqual(recorder.calls[1].2, ["action": "approve"])
        XCTAssertEqual(recorder.calls[2].2, ["channelId": "c1", "messageId": "m1", "text": "@ci hi"])
    }

    func testInstalledAppsModelRevokesAndDecides() async {
        let recorder = AppsCallRecorder()
        recorder.replies["GET /api/apps/installed"] = AppsTests.installed
        recorder.replies["POST /api/apps/orgs/acme/requests/r1"] = ["ok": true, "status": "denied"]
        let model = InstalledAppsModel(client: recorder.client)
        await model.reload()
        XCTAssertTrue(model.loaded)
        XCTAssertNil(model.failure)
        await model.revoke(model.apps.organizations[0].installs[0])
        XCTAssertTrue(model.apps.organizations[0].installs.isEmpty)
        XCTAssertEqual(model.notice, "CI was revoked.")
        await model.decide(model.apps.organizations[0].requests[0], org: "acme", approve: false)
        XCTAssertTrue(model.apps.organizations[0].requests.isEmpty)
        XCTAssertEqual(model.notice, "Notes was denied.")
        XCTAssertEqual(recorder.calls.last?.2, ["action": "deny"])

        recorder.failures["DELETE /api/apps/installs/i1"] = OrgApiException("forbidden", "forbidden")
        await model.revoke(model.apps.installs[0])
        XCTAssertEqual(model.apps.installs.count, 1)
        XCTAssertEqual(model.notice, "Only an organization owner or admin can do that.")

        recorder.failures["GET /api/apps/installed"] = URLError(.notConnectedToInternet)
        await model.reload()
        XCTAssertEqual(model.failure, "Could not reach ForkMesh. Try again.")
        XCTAssertEqual(model.apps.installs.count, 1)
    }

    func testApiServiceCallsTheAppsRoutesWithTheSession() async throws {
        let http = FakeHTTPClient { request in
            XCTAssertEqual(request.method, "GET")
            XCTAssertEqual(request.url.path, "/api/apps/commands")
            return HTTPResponse(status: 200, json: [
                "ok": true, "commands": [["command": "hello", "app": ["name": "Hello App"]]], "mentionable": [],
            ])
        }
        let api = makeTestApi(http, token: "tok")
        let catalog = try await api.appsClient.commands()
        XCTAssertEqual(catalog.commands.map(\.command), ["hello"])
        XCTAssertEqual(http.lastRequest?.headers["Authorization"], "Bearer tok")

        let refusing = makeTestApi(FakeHTTPClient { _ in HTTPResponse(status: 404, json: ["ok": false, "error": "unknown_command"]) }, token: "tok")
        let reply = await refusing.appsClient.run(text: "/nope", channelId: "")
        XCTAssertEqual(reply.error, "unknown_command")
    }
}
