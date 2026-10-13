import FMApi
import FMBase
import FMNet
import Foundation
import XCTest

@testable import FMCode

final class ProviderMirrorTests: XCTestCase {
    private let repo = RepoRef(owner: "alice", name: "tool")

    @MainActor private func api(_ http: FakeHTTPClient) -> ApiService {
        let api = ApiService(settings: StaticApiSettings(serverUrl: "wss://mirror.test/ws"), http: http, sleep: { _ in })
        api.sessionTokenProvider = { "session-token" }
        return api
    }

    private func body(_ request: HTTPRequest?) -> JSONObject {
        guard let data = request?.body, let parsed = try? JSON.parse(data) else { return [:] }
        return parsed.objectValue ?? [:]
    }

    private static let catalog: JSONValue = [
        "providers": [
            ["key": "github", "label": "GitHub", "web": "https://github.com", "tokenUrl": "https://github.com/t", "tokenHelp": "help"],
            ["key": "gitlab", "label": "GitLab", "web": "https://gitlab.com", "tokenUrl": "https://gitlab.com/t", "tokenHelp": "help"],
        ],
        "resources": [
            ["key": "code", "label": "Code", "labels": [:], "icon": "git-branch", "hint": "h", "directions": ["off", "push"],
             "providers": ["github", "gitlab"], "default": "push"],
            ["key": "pulls", "label": "Pull requests", "labels": ["gitlab": "Merge requests"], "icon": "git-pull-request",
             "hint": "h", "directions": ["off", "push", "pull", "both"], "providers": ["github", "gitlab"], "default": "off"],
            ["key": "discussions", "label": "Discussions", "labels": [:], "icon": "messages-square", "hint": "h",
             "directions": ["off", "push", "pull", "both"], "providers": ["github"], "default": "off"],
        ],
    ]

    private static let mirror: JSONValue = [
        "provider": "github", "providerLabel": "GitHub", "remote": "octo/tool", "remoteUrl": "https://github.com/octo/tool",
        "tokenHint": "…1234", "active": true, "prune": false,
        "resources": ["code": ["direction": "push", "mode": "auto", "comments": true],
                      "pulls": ["direction": "pull", "mode": "manual", "comments": false]],
        "status": ["login": "octo", "canPush": true, "codePush": ["at": 1_000, "ok": true, "refs": 3],
                   "lastSync": ["at": 2_000, "ok": true, "imported": 2, "comments": 1]],
        "history": [["at": 2_000, "kind": "sync", "ok": true, "message": "Imported 2 item(s)"]],
    ]

    func testRemoteParsingAndDraftValidation() {
        XCTAssertEqual(ProviderMirrorDraft.cleanRemote(" https://github.com/octo/tool.git "), "octo/tool")
        XCTAssertEqual(ProviderMirrorDraft.cleanRemote("gitlab.com/octo/tool"), "octo/tool")
        XCTAssertNil(ProviderMirrorDraft.cleanRemote("octo/sub/tool"))
        XCTAssertNil(ProviderMirrorDraft.cleanRemote("octo/../x"))
        XCTAssertNil(ProviderMirrorDraft.cleanRemote("octo/to ol"))
        var draft = ProviderMirrorDraft()
        draft.remote = "octo/tool"
        XCTAssertEqual(draft.validationError(hasStoredToken: false), "Paste an access token.")
        XCTAssertNil(draft.validationError(hasStoredToken: true))
        draft.token = "has space"
        XCTAssertEqual(draft.validationError(hasStoredToken: true), "That token does not look right.")
        XCTAssertEqual(ProviderMirrorText.direction("pull", provider: "GitLab"), "GitLab → ForkMesh")
        XCTAssertEqual(RepoSettingsSection(route: "mirror"), .mirror)
        XCTAssertEqual(RepoSettingsSection.mirror.title, "Mirror to GitHub or GitLab")
    }

    @MainActor func testLoadSaveAndManualImport() async {
        let http = FakeHTTPClient()
        var connected = false
        http.on("POST", path: "/api/repo/alice/tool/provider-mirror") { request in
            let sent = (try? JSON.parse(request.body ?? Data()))?.objectValue ?? [:]
            switch sent.str("action") {
            case "get":
                return HTTPResponse(status: 200, json: ["ok": true, "catalog": Self.catalog, "codeHosted": true,
                                                         "mirror": connected ? Self.mirror : .null])
            case "save":
                connected = true
                return HTTPResponse(status: 200, json: ["ok": true, "catalog": Self.catalog, "codeHosted": true,
                                                         "mirror": Self.mirror, "created": true])
            case "candidates":
                return HTTPResponse(status: 200, json: ["ok": true, "kind": "pulls", "more": false, "items": [
                    ["key": "12", "number": 12, "title": "Speed up", "author": "erin", "state": "open", "url": "u"],
                    ["key": "11", "number": 11, "title": "Typo", "author": "dan", "state": "closed", "merged": true,
                     "imported": true, "url": "u"],
                ]])
            case "import":
                return HTTPResponse(status: 200, json: ["ok": true, "catalog": Self.catalog, "mirror": Self.mirror,
                                                         "imported": 1])
            default:
                return HTTPResponse(status: 400, json: ["error": "verification_failed", "message": "The token was refused."])
            }
        }
        let model = ProviderMirrorModel(client: api(http), repo: repo)
        await model.load()
        XCTAssertTrue(model.loaded)
        XCTAssertNil(model.mirror)
        XCTAssertEqual(model.resources.map(\.key), ["code", "pulls", "discussions"])
        XCTAssertEqual(model.resources[1].title(for: "gitlab"), "Merge requests")
        XCTAssertFalse(model.resources[2].supported(by: "gitlab"))

        var draft = ProviderMirrorDraft()
        draft.remote = "https://github.com/octo/tool"
        draft.token = "github_pat_1234"
        draft.resources["pulls"] = ProviderMirrorSetting(direction: "pull", mode: "manual", comments: false)
        let saved = await model.save(draft, description: "Tool")
        XCTAssertTrue(saved)
        XCTAssertEqual(model.notice, "Created octo/tool and connected.")
        let save = body(http.requests.first { self.body($0)["action"] == "save" })
        XCTAssertEqual(save.str("remote"), "octo/tool")
        XCTAssertEqual(save.str("token"), "github_pat_1234")
        XCTAssertEqual(save.obj("resources").obj("code").str("direction"), "push")
        XCTAssertEqual(save.obj("resources").obj("pulls").str("mode"), "manual")
        XCTAssertEqual(save.obj("resources").obj("pulls")["comments"], .bool(false))
        XCTAssertEqual(http.requests.last?.headers["Authorization"], "Bearer session-token")

        XCTAssertEqual(model.mirror?.login, "octo")
        XCTAssertEqual(model.mirror?.codePushRefs, 3)
        XCTAssertEqual(model.importKinds.map(\.key), ["pulls"])
        XCTAssertTrue(model.mirror?.setting("pulls").manual ?? false)

        await model.openCandidates("pulls")
        XCTAssertEqual(model.candidates.map(\.key), ["12", "11"])
        XCTAssertEqual(model.candidates[1].stateLabel, "merged")
        model.toggle(model.candidates[1])
        XCTAssertTrue(model.selected.isEmpty)
        model.toggle(model.candidates[0])
        let count = await model.importSelected()
        XCTAssertEqual(count, 1)
        let sent = body(http.requests.last(where: { self.body($0)["action"] == "import" }))
        XCTAssertEqual(sent.strings("keys"), ["12"])
        XCTAssertEqual(sent.str("kind"), "pulls")

        await model.verify()
        XCTAssertEqual(model.error, "The token was refused.")
    }
}
