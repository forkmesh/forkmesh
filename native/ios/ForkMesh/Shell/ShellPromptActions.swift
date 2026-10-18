import FMAccount
import FMAgents
import FMApi
import FMBase
import FMCode
import FMCrypto
import FMTools
import Foundation

@MainActor
final class ShellPromptActions: QuickPromptActions {
    private let api: () -> ApiService?
    private let auth: () -> AuthService?

    init(
        api: @escaping () -> ApiService? = { Services.get(ApiService.self) },
        auth: @escaping () -> AuthService? = { Services.get(AuthService.self) }
    ) {
        self.api = api
        self.auth = auth
    }

    private func requireApi() throws -> ApiService {
        guard let api = api() else { throw QuickActionError("ForkMesh is not connected in this build.") }
        return api
    }

    func reportBug(_ text: String, photos: [PromptAttachment]) async throws {
        let api = try requireApi()
        let build: AppBuild = Services.get(AppConfig.self)?.build ?? AppBuild(infoDictionary: Bundle.main.infoDictionary)
        let result = await api.submitBugReport(
            message: text, screen: "composer", platform: build.platform, appVersion: build.version,
            osVersion: build.osVersion,
            attachments: photos.map { BugReportAttachment(name: $0.name, contentType: $0.mediaType, data: $0.bytes) })
        if !result.ok { throw QuickActionError(result.message) }
    }

    func taskPeople() async throws -> QuickTaskPeople {
        let data = try await requireApi().tasksApi("GET", "", nil)
        let actor = (data["actor"]?.stringValue ?? "").lowercased()
        let members = data["canManage"]?.boolValue == true
            ? (data["members"]?.arrayValue ?? []).compactMap(\.stringValue) : []
        return QuickTaskPeople(actor: actor, members: members)
    }

    func createTask(title: String, details: String, assignee: String, repository: String) async throws {
        var body: JSONObject = [
            "title": .string(title),
            "details": .string(details),
            "department": "general",
            "destination": "department",
            "assigneeKind": assignee.isEmpty ? "unassigned" : "user",
            "priority": 50,
        ]
        if !assignee.isEmpty { body["assignee"] = .string(assignee) }
        if !repository.isEmpty { body["repository"] = .string(repository) }
        do {
            _ = try await requireApi().tasksApi("POST", "", body)
        } catch let error as QuickActionError {
            throw error
        } catch {
            throw QuickActionError(TaskText.errorText(error, "The task was not created."))
        }
    }

    func createIssue(owner: String, name: String, title: String, body: String) async throws {
        guard let inbox = Services.get(InboxService.self) else {
            throw QuickActionError("Sign in on this device to file issues.")
        }
        do {
            try await inbox.submitNewIssue(owner, name, title: title, body: body)
        } catch {
            throw QuickActionError("The issue was not filed: \(error)")
        }
    }

    func postToActivityPub(owner: String, name: String, title: String, body: String) async throws {
        let account: String = (auth()?.session?.nodeName ?? "").lowercased()
        guard let identity = Services.get(DeviceIdentity.self), !account.isEmpty else {
            throw QuickActionError("Sign in on this device to post to ActivityPub.")
        }
        do {
            try await requireApi().announcePost(
                RepoRef(owner: owner, name: name), title: title, body: body, account: account, identity: identity,
                nowMillis: SystemClock().nowMillis)
        } catch let error as QuickActionError {
            throw error
        } catch {
            throw QuickActionError("Could not post to ActivityPub: \(error)")
        }
    }

    func takeNote(title: String, markdown: String) async throws {
        do {
            _ = try await requireApi().notesApi("POST", "", [
                "title": .string(title), "markdown": .string(markdown), "visibility": "private",
            ])
        } catch let error as QuickActionError {
            throw error
        } catch let error as OrgApiException {
            throw QuickActionError(error.message.isEmpty ? "The note was not saved." : "The note was not saved: \(error.message)")
        } catch {
            throw QuickActionError("The note was not saved.")
        }
    }
}
