import FMBase
import Foundation

public struct AppScope: Equatable, Hashable, Sendable, Identifiable {
    public let scope: String
    public let label: String
    public let hint: String

    public var id: String { scope }

    public init(scope: String, label: String = "", hint: String = "") {
        self.scope = scope
        self.label = label
        self.hint = hint
    }

    public init(json: JSONObject) {
        self.init(scope: json.str("scope"), label: json.str("label"), hint: json.str("hint"))
    }

    public var title: String { label.isEmpty ? scope : label }

    static func list(_ values: [JSONObject]) -> [AppScope] {
        values.map(AppScope.init(json:)).filter { !$0.scope.isEmpty }
    }
}

public struct AppSummary: Equatable, Hashable, Sendable, Identifiable {
    public let id: String
    public let slug: String
    public let name: String
    public let botName: String
    public let description: String
    public let developer: String
    public let organization: String
    public let listing: String

    public init(
        id: String, slug: String, name: String, botName: String = "", description: String = "", developer: String = "",
        organization: String = "", listing: String = ""
    ) {
        self.id = id
        self.slug = slug
        self.name = name
        self.botName = botName
        self.description = description
        self.developer = developer
        self.organization = organization
        self.listing = listing
    }

    public init(json: JSONObject) {
        self.init(
            id: json.str("id"), slug: json.str("slug"), name: json.str("name"), botName: json.str("botName"),
            description: json.str("description"), developer: json.str("developer"),
            organization: json.str("organization"), listing: json.str("listing"))
    }

    public var title: String {
        if !name.isEmpty { return name }
        return slug.isEmpty ? "App" : slug
    }
}

public struct AppInstall: Equatable, Hashable, Sendable, Identifiable {
    public let installId: String
    public let app: AppSummary
    public let targetKind: String
    public let targetName: String
    public let scopes: [AppScope]
    public let installedBy: String
    public let createdAt: Int64
    public let lastUsedAt: Int64
    public let canRevoke: Bool

    public var id: String { installId }

    public init(json: JSONObject) {
        installId = json.str("installId")
        app = AppSummary(json: json.obj("app"))
        let target = json.obj("target")
        targetKind = target.str("kind")
        targetName = target.str("name")
        scopes = AppScope.list(json.maps("scopes"))
        installedBy = json.str("installedBy")
        createdAt = json.int64("createdAt")
        lastUsedAt = json.int64("lastUsedAt")
        canRevoke = json.bool("canRevoke")
    }

    public var targetLabel: String {
        targetKind == "org" ? (targetName.isEmpty ? "An organization" : targetName) : "Just me"
    }

    public func lastUsedText(nowMillis: Int64 = SystemClock().nowMillis) -> String {
        lastUsedAt <= 0 ? "Never used" : "Last used \(Format.relativeTimeFrom(lastUsedAt, nowMillis: nowMillis))"
    }
}

public struct AppInstallRequest: Equatable, Hashable, Sendable, Identifiable {
    public let id: String
    public let app: AppSummary
    public let scopes: [AppScope]
    public let requester: String
    public let createdAt: Int64

    public init(json: JSONObject) {
        id = json.str("id")
        app = AppSummary(json: json.obj("app"))
        scopes = AppScope.list(json.maps("scopes"))
        requester = json.str("requester")
        createdAt = json.int64("createdAt")
    }
}

public struct AppOrgInstalls: Equatable, Sendable, Identifiable {
    public let name: String
    public let role: String
    public let canManage: Bool
    public var installs: [AppInstall]
    public var requests: [AppInstallRequest]

    public var id: String { name }

    public init(json: JSONObject) {
        name = json.str("name")
        role = json.str("role")
        canManage = json.bool("canManage")
        installs = json.maps("installs").map(AppInstall.init(json:)).filter { !$0.installId.isEmpty }
        requests = json.maps("requests").map(AppInstallRequest.init(json:)).filter { !$0.id.isEmpty }
    }
}

public struct InstalledApps: Equatable, Sendable {
    public var account: String
    public var installs: [AppInstall]
    public var organizations: [AppOrgInstalls]

    public init(account: String = "", installs: [AppInstall] = [], organizations: [AppOrgInstalls] = []) {
        self.account = account
        self.installs = installs
        self.organizations = organizations
    }

    public init(json: JSONObject) {
        account = json.str("account")
        installs = json.maps("installs").map(AppInstall.init(json:)).filter { !$0.installId.isEmpty }
        organizations = json.maps("organizations").map(AppOrgInstalls.init(json:)).filter { !$0.name.isEmpty }
    }

    public var isEmpty: Bool {
        installs.isEmpty && organizations.allSatisfy { $0.installs.isEmpty && $0.requests.isEmpty }
    }

    public var pendingRequests: Int { organizations.reduce(0) { $0 + $1.requests.count } }
}

public struct AppCommand: Equatable, Hashable, Sendable, Identifiable {
    public let command: String
    public let description: String
    public let usageHint: String
    public let appName: String
    public let appSlug: String
    public let installId: String

    public var id: String { command }

    public init(
        command: String, description: String = "", usageHint: String = "", appName: String = "", appSlug: String = "",
        installId: String = ""
    ) {
        self.command = command
        self.description = description
        self.usageHint = usageHint
        self.appName = appName
        self.appSlug = appSlug
        self.installId = installId
    }

    public init(json: JSONObject) {
        let app = json.obj("app")
        self.init(
            command: json.str("command").lowercased(), description: json.str("description"),
            usageHint: json.str("usageHint"), appName: app.str("name"), appSlug: app.str("slug"),
            installId: json.str("installId"))
    }

    public var usage: String { usageHint.isEmpty ? "/\(command)" : "/\(command) \(usageHint)" }

    public var summary: String {
        let app = appName.isEmpty ? appSlug : appName
        if description.isEmpty { return app }
        return app.isEmpty ? description : "\(description) · \(app)"
    }
}

public struct AppCommandCatalog: Equatable, Sendable {
    public var commands: [AppCommand]
    public var mentionable: [String]

    public init(commands: [AppCommand] = [], mentionable: [String] = []) {
        self.commands = commands
        self.mentionable = mentionable
    }

    public init(json: JSONObject) {
        var seen = Set<String>()
        commands = json.maps("commands").map(AppCommand.init(json:)).filter {
            AppsChat.validCommand($0.command) && seen.insert($0.command).inserted
        }
        mentionable = json.strings("mentionable").map { $0.lowercased() }.filter { !$0.isEmpty }
    }

    public func command(_ name: String) -> AppCommand? { commands.first { $0.command == name.lowercased() } }
}

public struct AppSelectOption: Equatable, Hashable, Sendable, Identifiable {
    public let text: String
    public let value: String

    public var id: String { value }

    public init(text: String, value: String) {
        self.text = text
        self.value = value
    }
}

public enum AppButtonStyle: String, Equatable, Hashable, Sendable {
    case plain = ""
    case primary
    case danger
}

public enum AppBlockElement: Equatable, Hashable, Sendable, Identifiable {
    case button(actionId: String, text: String, value: String, style: AppButtonStyle)
    case select(actionId: String, placeholder: String, options: [AppSelectOption])

    public var actionId: String {
        switch self {
        case .button(let actionId, _, _, _), .select(let actionId, _, _): return actionId
        }
    }

    public var id: String { actionId }
}

public enum AppBlock: Equatable, Hashable, Sendable {
    case section(text: String, fields: [String])
    case context(text: String)
    case divider
    case actions([AppBlockElement])
}

public struct AppMessage: Equatable, Hashable, Sendable {
    public let blocks: [AppBlock]
    public let appMessageId: String
    public let channelId: String
    public let appName: String
    public let ephemeral: Bool

    public init(blocks: [AppBlock], appMessageId: String, channelId: String = "", appName: String = "", ephemeral: Bool = false) {
        self.blocks = blocks
        self.appMessageId = appMessageId
        self.channelId = channelId
        self.appName = appName
        self.ephemeral = ephemeral
    }

    public var interactive: Bool {
        !appMessageId.isEmpty && blocks.contains { if case .actions = $0 { return true } else { return false } }
    }
}

public struct AppReply: Equatable, Sendable {
    public let ok: Bool
    public let responseType: String
    public let text: String
    public let blocks: [AppBlock]
    public let appMessageId: String
    public let appName: String
    public let botName: String
    public let error: String

    public init(
        ok: Bool, responseType: String = "", text: String = "", blocks: [AppBlock] = [], appMessageId: String = "",
        appName: String = "", botName: String = "", error: String = ""
    ) {
        self.ok = ok
        self.responseType = responseType
        self.text = text
        self.blocks = blocks
        self.appMessageId = appMessageId
        self.appName = appName
        self.botName = botName
        self.error = error
    }

    public init(json: JSONObject) {
        let app = json["app"]?.objectValue ?? JSONObject()
        let name = app.str("name").isEmpty ? (json["app"]?.stringValue ?? "") : app.str("name")
        self.init(
            ok: json["ok"] != .bool(false) && json.str("error").isEmpty, responseType: json.str("responseType"),
            text: json.str("text"), blocks: AppsChat.blocks(json.list("blocks")),
            appMessageId: json.str("appMessageId"), appName: name, botName: app.str("botName"), error: json.str("error"))
    }

    public var isEphemeral: Bool { ok && responseType == "ephemeral" }

    public var sender: String {
        if !botName.isEmpty { return botName }
        return appName.isEmpty ? "App" : "\(appName) (app)"
    }

    public var errorText: String { AppsChat.errorText(error, app: appName) }
}

public enum AppsChat {
    public static let maxBlocks = 20
    public static let maxFields = 10
    public static let maxElements = 10
    public static let maxOptions = 100
    public static let ephemeralNote = "Only you can see this."
    public static let listCommand = "apps"

    private static let slashPattern = try! NSRegularExpression(
        pattern: "^/([a-z][a-z0-9-]{1,31})(?:\\s+([\\s\\S]*))?$", options: [.caseInsensitive])
    private static let mentionPattern = try! NSRegularExpression(
        pattern: "(^|[^\\w@/-])@([a-z][a-z0-9-]{1,30}[a-z0-9])(?![\\w-])", options: [.caseInsensitive])
    private static let commandPattern = try! NSRegularExpression(pattern: "^[a-z][a-z0-9-]{1,31}$")

    public static func validCommand(_ name: String) -> Bool {
        commandPattern.firstMatch(in: name, range: NSRange(name.startIndex..., in: name)) != nil
    }

    public static func parseSlash(_ text: String) -> (command: String, arguments: String)? {
        let source = text.trimmingCharacters(in: .whitespacesAndNewlines)
        let range = NSRange(source.startIndex..., in: source)
        guard let match = slashPattern.firstMatch(in: source, range: range),
            let command = Range(match.range(at: 1), in: source)
        else { return nil }
        let arguments = Range(match.range(at: 2), in: source).map { String(source[$0]) } ?? ""
        return (String(source[command]).lowercased(), arguments)
    }

    public static func mentionedSlugs(_ text: String) -> [String] {
        let range = NSRange(text.startIndex..., in: text)
        var out: [String] = []
        for match in mentionPattern.matches(in: text, range: range) {
            guard let slug = Range(match.range(at: 2), in: text) else { continue }
            let lowered = String(text[slug]).lowercased()
            if !out.contains(lowered) { out.append(lowered) }
        }
        return out
    }

    public static func shouldForwardMentions(_ text: String, mentionable: [String]) -> Bool {
        let known = Set(mentionable.map { $0.lowercased() })
        return mentionedSlugs(text).contains { known.contains($0) }
    }

    public static func commandsText(_ commands: [AppCommand]) -> String {
        if commands.isEmpty { return "You have no app commands. Install apps from /dashboard/apps." }
        return "Your app commands: " + commands.map { "/\($0.command) (\($0.appName.isEmpty ? $0.appSlug : $0.appName))" }
            .joined(separator: ", ")
    }

    public static func errorText(_ code: String, app: String = "") -> String {
        switch code {
        case "unknown_command": return "No installed app has that command. Type /apps to see yours."
        case "app_unavailable": return app.isEmpty ? "The app did not answer." : "\(app) did not answer."
        case "rate_limited": return "Too many requests to that app; try again in a minute."
        case "message_expired": return "That message is too old to use."
        case "network_error": return "Could not reach ForkMesh. Try again."
        default: return "The app could not handle that (\(code.isEmpty ? "error" : code))."
        }
    }

    public static func suggestions(_ text: String, commands: [AppCommand]) -> [AppCommand] {
        guard text.hasPrefix("/"), !text.contains(where: { $0.isWhitespace }) else { return [] }
        let prefix = text.dropFirst().lowercased()
        var out = commands.filter { $0.command.hasPrefix(prefix) }
        if listCommand.hasPrefix(prefix) && !commands.contains(where: { $0.command == listCommand }) {
            out.append(AppCommand(command: listCommand, description: "List your app commands"))
        }
        return out
    }

    public static func blocks(_ values: [JSONValue]) -> [AppBlock] {
        var out: [AppBlock] = []
        var actionIds = Set<String>()
        for value in values.prefix(maxBlocks) {
            guard let block = value.objectValue else { continue }
            switch block.str("type") {
            case "divider":
                out.append(.divider)
            case "section":
                let text = blockText(block["text"])
                let fields = block.list("fields").prefix(maxFields).map { blockText($0) }.filter { !$0.isEmpty }
                if !text.isEmpty || !fields.isEmpty { out.append(.section(text: text, fields: Array(fields))) }
            case "context":
                let text = blockText(block["text"])
                if !text.isEmpty { out.append(.context(text: text)) }
            case "actions":
                var elements: [AppBlockElement] = []
                for element in block.maps("elements").prefix(maxElements) {
                    let actionId = element.str("action_id")
                    if actionId.isEmpty || actionIds.contains(actionId) { continue }
                    switch element.str("type") {
                    case "button":
                        let text = blockText(element["text"])
                        if text.isEmpty { continue }
                        actionIds.insert(actionId)
                        elements.append(
                            .button(
                                actionId: actionId, text: text, value: element.str("value"),
                                style: AppButtonStyle(rawValue: element.str("style")) ?? .plain))
                    case "static_select":
                        let options = element.maps("options").prefix(maxOptions).compactMap { option -> AppSelectOption? in
                            let text = blockText(option["text"])
                            let value = option.str("value")
                            return text.isEmpty || value.isEmpty ? nil : AppSelectOption(text: text, value: value)
                        }
                        if options.isEmpty { continue }
                        actionIds.insert(actionId)
                        let placeholder = blockText(element["placeholder"])
                        elements.append(
                            .select(actionId: actionId, placeholder: placeholder.isEmpty ? "Choose" : placeholder, options: options))
                    default:
                        continue
                    }
                }
                if !elements.isEmpty { out.append(.actions(elements)) }
            default:
                continue
            }
        }
        return out
    }

    public static func message(_ frame: JSONObject, channelId: String) -> AppMessage? {
        guard frame.str("senderId").hasPrefix("app:") else { return nil }
        let blocks = blocks(frame.list("blocks"))
        if blocks.isEmpty { return nil }
        let app = frame.obj("app")
        return AppMessage(
            blocks: blocks, appMessageId: frame.str("appMessageId"), channelId: channelId, appName: app.str("name"))
    }

    static func blockText(_ value: JSONValue?) -> String {
        guard let value else { return "" }
        if let object = value.objectValue { return object.str("text") }
        return value.stringValue ?? ""
    }
}

public struct AppsClient: Sendable {
    public typealias Call = @MainActor @Sendable (String, String, JSONObject?) async throws -> JSONObject

    let call: Call

    public init(call: @escaping Call) {
        self.call = call
    }

    static func segment(_ value: String) -> String {
        value.addingPercentEncoding(withAllowedCharacters: .alphanumerics.union(CharacterSet(charactersIn: "-._~"))) ?? value
    }

    @MainActor public func installed() async throws -> InstalledApps {
        InstalledApps(json: try await call("GET", "/api/apps/installed", nil))
    }

    @MainActor public func revoke(installId: String) async throws {
        _ = try await call("DELETE", "/api/apps/installs/\(AppsClient.segment(installId))", nil)
    }

    @MainActor public func decide(org: String, requestId: String, approve: Bool) async throws -> String {
        let path = "/api/apps/orgs/\(AppsClient.segment(org.lowercased()))/requests/\(AppsClient.segment(requestId))"
        let answer = try await call("POST", path, ["action": .string(approve ? "approve" : "deny")])
        return answer.str("status")
    }

    @MainActor public func commands() async throws -> AppCommandCatalog {
        AppCommandCatalog(json: try await call("GET", "/api/apps/commands", nil))
    }

    @MainActor public func run(text: String, channelId: String) async -> AppReply {
        await reply("/api/apps/commands/run", ["text": .string(text), "channelId": .string(channelId)])
    }

    @MainActor public func interact(appMessageId: String, channelId: String, actionId: String, value: String) async -> AppReply {
        await reply(
            "/api/apps/interact",
            [
                "appMessageId": .string(appMessageId), "channelId": .string(channelId), "actionId": .string(actionId),
                "value": .string(value),
            ])
    }

    @MainActor public func forwardMentions(channelId: String, messageId: String, text: String) async -> Int {
        let body: JSONObject = ["channelId": .string(channelId), "messageId": .string(messageId), "text": .string(text)]
        return (try? await call("POST", "/api/apps/mentions", body))?.int("forwarded") ?? 0
    }

    @MainActor private func reply(_ path: String, _ body: JSONObject) async -> AppReply {
        do {
            return AppReply(json: try await call("POST", path, body))
        } catch let error as OrgApiException {
            return AppReply(ok: false, error: error.code)
        } catch {
            return AppReply(ok: false, error: "network_error")
        }
    }
}
