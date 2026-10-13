import FMApi
import FMBase
import FMControl
import FMLocalAI
import FMNet
import FMPackages
import XCTest
@testable import FMAssistant

@MainActor
final class FakeCrmTransport: PackagesTransport {
    var contacts: [JSONObject] = []
    var installed = true
    var requests: [(method: String, path: String, body: JSONObject?)] = []
    var nextId = 0

    func send(_ method: String, _ path: String, query: [(String, String)], body: JSONObject?) async throws -> PackagesReply {
        requests.append((method, path, body))
        let parts = path.split(separator: "/").map(String.init)
        switch (method, parts.count) {
        case ("GET", 2):
            return PackagesReply(status: 200, json: ["organizations": .array([.object(["name": "acme", "role": "owner", "installed": .bool(installed)])])])
        case ("GET", 3):
            return PackagesReply(status: 200, json: [
                "organization": "acme",
                "contacts": .array(contacts.map(JSONValue.object)),
                "emails": .array([.object(["id": "e1", "contactId": .string(String(repeating: "a", count: 32)), "subject": "Intro", "status": "sent", "sentAt": 1_790_000_000_000])]),
                "settings": .object(["flows": .array([.object(["id": "customers", "name": "Customers", "stages": .array(["lead", "qualified", "customer"])])])]),
                "members": .array(["ada", "bob"]),
                "summary": .object(["contacts": .int(Int64(contacts.count)), "customers": 0, "sent": 1]),
            ])
        case ("POST", 4):
            nextId += 1
            var contact = body ?? [:]
            contact["id"] = .string(String(repeating: "c", count: 31) + "\(nextId)")
            contacts.append(contact)
            return PackagesReply(status: 200, json: ["contact": .object(contact)])
        case ("PATCH", 5):
            guard let index = contacts.firstIndex(where: { $0.str("id") == parts[4] }) else { return PackagesReply(status: 404, json: ["error": "not_found"]) }
            for (key, value) in body ?? [:] { contacts[index][key] = value }
            return PackagesReply(status: 200, json: ["contact": .object(contacts[index])])
        case ("DELETE", 5):
            contacts.removeAll { $0.str("id") == parts[4] }
            return PackagesReply(status: 200, json: ["ok": true])
        default:
            return PackagesReply(status: 404, json: ["error": "not_found"])
        }
    }
}

@MainActor
final class FakeAgentControl: AgentControlling {
    var rows: [UnifiedAgentRow] = []
    var promptNotice = ""
    var refreshes = 0
    var prompts: [(String, String)] = []
    var stopped: [String] = []

    func refresh() async { refreshes += 1 }
    func stop(_ row: UnifiedAgentRow) async throws { stopped.append(row.id) }
    func prompt(_ row: UnifiedAgentRow, _ text: String, images: [PromptAttachment]) async throws { prompts.append((row.id, text)) }
    func continueRun(_ row: UnifiedAgentRow) async throws {}
    func transcript(_ row: UnifiedAgentRow, since: Int) async throws -> AgentTranscriptDelta {
        AgentTranscriptDelta(text: String(repeating: "x", count: 5000) + "All tests pass.", next: 1, reset: true, active: true, status: "running")
    }
    func startOnDesktop(owner: String, name: String, task: String, provider: String, images: [PromptAttachment]) async throws -> Int { 0 }
}

@MainActor
final class ForkMeshToolboxManageTests: XCTestCase {
    private let utc = TimeZone(identifier: "UTC")!
    private let nowDate = Date(timeIntervalSince1970: 1_790_000_000)
    private let adaId = String(repeating: "a", count: 32)
    private let bobId = String(repeating: "b", count: 32)

    private func crm() -> FakeCrmTransport {
        let transport = FakeCrmTransport()
        transport.contacts = [
            ["id": .string(adaId), "name": "Ada Lovelace", "email": "ada@example.com", "company": "Engines", "flow": "customers", "stage": "lead", "notes": "Met at a conference.", "tags": .array(["vip"])],
            ["id": .string(bobId), "name": "Bob Builder", "email": "bob@example.com", "flow": "customers", "stage": "qualified"],
        ]
        return transport
    }

    private func toolbox(crm transport: FakeCrmTransport? = nil, agents: FakeAgentControl? = nil, web: HTTPClient? = nil) -> ForkMeshToolbox {
        let client = transport.map { PackagesClient(transport: $0) }
        let sources = AssistantSources(
            account: { AssistantAccount(username: "ada") },
            crm: { client },
            agentControl: { agents },
            web: { web }
        )
        return ForkMeshToolbox(sources: sources, timeZone: utc, now: { self.nowDate })
    }

    private func run(_ box: ForkMeshToolbox, _ name: String, _ args: JSONObject = [:]) async -> OnDeviceToolResult {
        await box.invoke(LocalToolCall(id: "c", name: name, arguments: args))
    }

    private func approval(_ box: ForkMeshToolbox, _ name: String, _ args: JSONObject) async -> String? {
        await box.approval(for: LocalToolCall(id: "c", name: name, arguments: args))
    }

    func testCrmListsThePipelineAndOneContact() async {
        let box = toolbox(crm: crm())
        let list = await run(box, "crm_contacts").text
        XCTAssertTrue(list.hasPrefix("CRM of acme: 2 contacts"), list)
        XCTAssertTrue(list.contains("Pipeline customers (Customers): lead 1 → qualified 1 → customer 0"), list)
        XCTAssertTrue(list.contains("[\(adaId)] Ada Lovelace · ada@example.com · Engines · customers/lead · tags: vip"), list)

        let staged = await run(box, "crm_contacts", ["stage": "qualified"]).text
        XCTAssertTrue(staged.contains("Bob Builder") && !staged.contains("Ada Lovelace ·"), staged)

        let one = await run(box, "crm_contacts", ["contact": "ada"]).text
        XCTAssertTrue(one.contains("Met at a conference.") && one.contains("sent \"Intro\""), one)
    }

    func testCrmCreateUpdateAppendsSignedNotes() async {
        let transport = crm()
        let box = toolbox(crm: transport)
        let created = await run(box, "create_crm_contact", ["name": "Grace Hopper", "email": "grace@example.com", "tags": "navy, cobol", "notes": "Wants a demo"]).text
        XCTAssertTrue(created.hasPrefix("Added to the acme CRM: "), created)
        let posted = transport.requests.last { $0.method == "POST" }?.body
        XCTAssertEqual(posted?.strings("tags"), ["navy", "cobol"])
        XCTAssertEqual(posted?.str("notes"), "[2026-09-21 ada] Wants a demo")

        let updated = await run(box, "update_crm_contact", ["contact": "Ada Lovelace", "stage": "qualified", "value": 5000, "append_note": "Asked for pricing", "mark_contacted": true]).text
        XCTAssertTrue(updated.hasPrefix("Updated Ada Lovelace"), updated)
        let patch = transport.requests.last { $0.method == "PATCH" }
        XCTAssertEqual(patch?.path, "/api/crm/acme/contacts/\(adaId)")
        XCTAssertEqual(patch?.body?.str("stage"), "qualified")
        XCTAssertEqual(patch?.body?.int64("value"), 5000)
        XCTAssertEqual(patch?.body?.str("notes"), "Met at a conference.\n\n[2026-09-21 ada] Asked for pricing")
        XCTAssertEqual(patch?.body?.int64("lastContactedAt"), 1_790_000_000_000)
        XCTAssertNil(patch?.body?["name"])

        let ambiguous = await run(box, "update_crm_contact", ["contact": "example.com", "stage": "lead"]).text
        XCTAssertTrue(ambiguous.contains("matches 3 contacts"), ambiguous)
    }

    func testCrmDeleteNeedsApprovalAndNamesTheContact() async {
        let transport = crm()
        let box = toolbox(crm: transport)
        let summary = await approval(box, "delete_crm_contact", ["id": .string(bobId)])
        XCTAssertEqual(summary, "Delete Bob Builder <bob@example.com> from the acme CRM")
        let noAsk = await approval(box, "update_crm_contact", ["id": .string(bobId)])
        XCTAssertNil(noAsk)
        let done = await run(box, "delete_crm_contact", ["id": .string(bobId)]).text
        XCTAssertEqual(done, "Deleted Bob Builder from the acme CRM.")
        XCTAssertEqual(transport.contacts.count, 1)
    }

    func testCrmNotInstalledSaysSo() async {
        let transport = crm()
        transport.installed = false
        let text = await run(toolbox(crm: transport), "crm_contacts").text
        XCTAssertTrue(text.contains("none of the user's organizations has the CRM package installed"), text)
    }

    func testDraftEmailAddressesAContactAndSendsNothing() async {
        let transport = crm()
        let box = toolbox(crm: transport)
        let result = await run(box, "draft_email", ["contact": "Ada", "subject": "Pricing", "body": "Hi Ada,\nHere it is."])
        XCTAssertEqual(result.email, OnDeviceEmailDraft(to: "ada@example.com", recipientName: "Ada Lovelace", subject: "Pricing", body: "Hi Ada,\nHere it is."))
        XCTAssertTrue(result.text.contains("It has not been sent."), result.text)
        XCTAssertFalse(transport.requests.contains { $0.method != "GET" }, "drafting writes nothing")

        let missing = await run(box, "draft_email", ["subject": "x", "body": "y"]).text
        XCTAssertTrue(missing.contains("no email address"), missing)
    }

    func testAgentsListReadMessageAndStop() async {
        let control = FakeAgentControl()
        control.rows = [
            UnifiedAgentRow(id: "desktop:7", badge: .desktopLan, title: "Fix login bug", repoOwner: "ada", repoName: "site", node: "workstation", status: "running", statusLabel: "Working", isActive: true, provider: "claude", updatedAtMs: 2, canPrompt: true, localId: 7),
            UnifiedAgentRow(id: "cloud:s1", badge: .node, title: "Write docs", repoOwner: "forkmesh", repoName: "core", node: "n1", status: "done", statusLabel: "Done", isActive: false, updatedAtMs: 1, cloudSessionId: "s1"),
        ]
        let box = toolbox(agents: control)
        let list = await run(box, "list_agents").text
        XCTAssertTrue(list.contains("[desktop:7] Fix login bug · Working · ada/site · desktop workstation · claude · can take messages"), list)
        XCTAssertTrue(list.contains("[cloud:s1] Write docs · Done · forkmesh/core · cloud on n1"), list)
        XCTAssertEqual(control.refreshes, 1)

        let running = await run(box, "list_agents", ["running_only": true]).text
        XCTAssertFalse(running.contains("Write docs"), running)

        let transcript = await run(box, "list_agents", ["id": "7"]).text
        XCTAssertTrue(transcript.contains("(working)") && transcript.hasSuffix("All tests pass."), transcript)
        XCTAssertLessThan(transcript.count, ForkMeshToolbox.transcriptTailChars + 400)

        let ask = await approval(box, "message_agent", ["id": "desktop:7", "text": "Also add a test"])
        XCTAssertEqual(ask, "Send to agent \"Fix login bug\" in ada/site: \"Also add a test\"")
        let stopAsk = await approval(box, "stop_agent", ["id": "docs"])
        XCTAssertEqual(stopAsk, "Stop agent \"Write docs\" in forkmesh/core")

        _ = await run(box, "message_agent", ["id": "desktop:7", "text": "Also add a test"])
        XCTAssertEqual(control.prompts.first?.0, "desktop:7")
        XCTAssertEqual(control.prompts.first?.1, "Also add a test")
        let stopped = await run(box, "stop_agent", ["id": "cloud:s1"]).text
        XCTAssertEqual(stopped, "Stopped \"Write docs\".")
        XCTAssertEqual(control.stopped, ["cloud:s1"])

        let unknown = await run(box, "stop_agent", ["id": "nope"]).text
        XCTAssertTrue(unknown.contains("no agent matches \"nope\""), unknown)
    }

    func testAgentsFallBackToCloudRunsWithoutControl() async {
        let sources = AssistantSources(agents: { [AgentLaunchSession(sessionId: "s9", owner: "ada", repo: "site", status: "running", title: "Cloud only")] })
        let box = ForkMeshToolbox(sources: sources, timeZone: utc, now: { self.nowDate })
        let text = await run(box, "list_agents").text
        XCTAssertTrue(text.hasPrefix("Agent runs: 1") && text.contains("Cloud only"), text)
    }

    func testSearchWebParsesDuckDuckGo() async throws {
        let html = """
        <div class="result results_links"><a rel="nofollow" class="result__a" href="//duckduckgo.com/l/?uddg=https%3A%2F%2Fengines.example%2Fada&amp;rut=abc">Ada <b>Lovelace</b> &amp; Engines</a>
        <a class="result__snippet" href="x">CEO of <b>Engines</b>, based in London.</a></div>
        <div class="result result--ad"><a class="result__a" href="https://duckduckgo.com/y.js?ad_domain=x">Buy now</a><a class="result__snippet">ad text</a></div>
        <div class="result"><a class="result__a" href="https://example.org/two">Second</a></div>
        """
        let http = FakeHTTPClient()
        http.on("GET", path: "/html") { _ in HTTPResponse(status: 200, headers: ["Content-Type": "text/html"], text: html) }
        let text = await run(toolbox(web: http), "search_web", ["query": "Ada Lovelace Engines"]).text
        XCTAssertTrue(text.contains("Ada Lovelace & Engines\n  https://engines.example/ada\n  CEO of Engines, based in London."), text)
        XCTAssertTrue(text.contains("Second\n  https://example.org/two"), text)
        XCTAssertFalse(text.contains("Buy now") || text.contains("ad text"), text)
        let sent = try XCTUnwrap(http.lastRequest)
        XCTAssertEqual(sent.url.host, "html.duckduckgo.com")
        XCTAssertTrue(sent.url.absoluteString.contains("q=Ada%20Lovelace%20Engines"), sent.url.absoluteString)
    }

    func testSearchWebReportsARobotCheck() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/html") { _ in HTTPResponse(status: 202, text: "<div class=\"anomaly-modal\"></div>") }
        let text = await run(toolbox(web: http), "search_web", ["query": "x"]).text
        XCTAssertTrue(text.contains("robot check"), text)
    }

    func testReadWebPageTurnsHTMLIntoText() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/about") { _ in
            HTTPResponse(status: 200, headers: ["Content-Type": "text/html; charset=utf-8"], text: """
            <html><head><title>About &amp; Team</title><style>p{color:red}</style></head>
            <body><script>alert(1)</script><h1>Ada&#39;s team</h1><p>We build <b>engines</b>.</p><ul><li>One</li><li>Two</li></ul></body></html>
            """)
        }
        let text = await run(toolbox(web: http), "read_web_page", ["url": "engines.example/about"]).text
        XCTAssertTrue(text.hasPrefix("Title: About & Team\nURL: https://engines.example/about\n"), text)
        XCTAssertTrue(text.hasSuffix("Ada's team\nWe build engines.\n- One\n- Two"), text)
        XCTAssertFalse(text.contains("alert") || text.contains("color:red"), text)
    }

    func testReadWebPageRefusesPrivateAddressesAndRedirectsToThem() async {
        let http = FakeHTTPClient()
        http.on("GET", path: "/go") { _ in HTTPResponse(status: 302, headers: ["Location": "http://192.168.1.1/admin"], text: "") }
        let box = toolbox(web: http)
        let redirected = await run(box, "read_web_page", ["url": "https://short.example/go"]).text
        XCTAssertTrue(redirected.contains("http://192.168.1.1/admin: private and local IP addresses are not read"), redirected)
        XCTAssertEqual(http.requests.count, 1, "the private hop is never fetched")
        XCTAssertFalse(http.requests[0].followRedirects)

        for url in ["http://localhost:8080", "http://127.0.0.1", "http://[::1]/", "http://printer.local", "http://nas", "file:///etc/hosts", "http://10.0.0.5", "http://172.20.1.1", "http://169.254.169.254/latest", "http://user:pw@example.com"] {
            let text = await run(box, "read_web_page", ["url": .string(url)]).text
            XCTAssertTrue(text.hasPrefix("read_web_page failed"), "\(url): \(text)")
        }
        XCTAssertEqual(http.requests.count, 1)
    }

    func testWebAddressAllowsPublicHosts() {
        XCTAssertNil(WebAddress.refusal(URL(string: "https://www.linkedin.com/in/ada")!))
        XCTAssertNil(WebAddress.refusal(URL(string: "http://93.184.216.34/")!))
        XCTAssertNil(WebAddress.refusal(URL(string: "https://[2606:2800:220:1::]/")!))
        XCTAssertNotNil(WebAddress.refusal(URL(string: "https://[fd00::1]/")!))
        XCTAssertNotNil(WebAddress.refusal(URL(string: "https://[::ffff:10.0.0.1]/")!))
    }

    func testWebTextDecodesEntities() {
        XCTAssertEqual(WebText.decodeEntities("a &amp; b &lt;c&gt; &#x41;&#66; &unknown; &"), "a & b <c> AB &unknown; &")
    }
}
