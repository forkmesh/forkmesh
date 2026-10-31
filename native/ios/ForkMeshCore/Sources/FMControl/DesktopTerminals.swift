import FMBase
import FMChat
import Foundation

public struct DesktopTerminalInfo: Equatable, Sendable, Identifiable {
    public var terminal: String
    public var provider: String
    public var label: String
    public var cli: String
    public var repositoryOwner: String
    public var repositoryName: String
    public var cwd: String
    public var startedAtMs: Int
    public var device: String
    public var running: Bool
    public var cols: Int
    public var rows: Int
    public var host: String

    public init(
        terminal: String, provider: String = "", label: String = "", cli: String = "", repositoryOwner: String = "",
        repositoryName: String = "", cwd: String = "", startedAtMs: Int = 0, device: String = "",
        running: Bool = true, cols: Int = 0, rows: Int = 0, host: String = ""
    ) {
        self.terminal = terminal
        self.provider = provider
        self.label = label
        self.cli = cli
        self.repositoryOwner = repositoryOwner
        self.repositoryName = repositoryName
        self.cwd = cwd
        self.startedAtMs = startedAtMs
        self.device = device
        self.running = running
        self.cols = cols
        self.rows = rows
        self.host = host
    }

    public init(json: JSONObject) {
        self.init(
            terminal: json.str("terminal"), provider: json.str("provider"), label: json.str("label"),
            cli: json.str("cli"), repositoryOwner: json.str("repositoryOwner"),
            repositoryName: json.str("repositoryName"), cwd: json.str("cwd"), startedAtMs: json.int("startedAtMs"),
            device: json.str("device"), running: json["running"] == nil ? true : json.bool("running"),
            cols: json.int("cols"), rows: json.int("rows"), host: json.str("host"))
    }

    public var id: String { terminal }

    public var repoLabel: String {
        repositoryOwner.isEmpty ? repositoryName : "\(repositoryOwner)/\(repositoryName)"
    }

    public static func list(_ answer: JSONObject) -> [DesktopTerminalInfo] {
        answer.list("terminals").compactMap { $0.objectValue }.map { DesktopTerminalInfo(json: $0) }
            .filter { !$0.terminal.isEmpty }
    }
}

@MainActor
public protocol DesktopTerminalChannel: AnyObject {
    var linkIdentity: ObjectIdentifier? { get }
    func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject
    @discardableResult
    func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID
    func removeEventTap(_ id: UUID)
    @discardableResult
    func addLinkListener(_ listener: @escaping @MainActor () -> Void) -> UUID
    func removeLinkListener(_ id: UUID)
}

extension DesktopControlHub: DesktopTerminalChannel {
    public var linkIdentity: ObjectIdentifier? { currentLink.map { ObjectIdentifier($0) } }
}

extension DesktopScopedChannel: DesktopTerminalChannel {
    public var linkIdentity: ObjectIdentifier? { hub.openLink(for: desktopKey).map { ObjectIdentifier($0) } }

    @discardableResult
    public func addLinkListener(_ listener: @escaping @MainActor () -> Void) -> UUID { hub.addLinkListener(listener) }

    public func removeLinkListener(_ id: UUID) { hub.removeLinkListener(id) }
}

extension DesktopControlHub {
    public func terminalChannel(for desktopKey: String) -> any DesktopTerminalChannel {
        desktopKey.isEmpty ? self : DesktopScopedChannel(hub: self, desktopKey: desktopKey)
    }
}

@MainActor
public enum DesktopTerminalSupport {
    public nonisolated static let capability = "terminal.agents"
    public nonisolated static let resumeCapability = "terminal.resume"
    public nonisolated static let accountsCapability = "terminal.accounts"

    private static var known: [ObjectIdentifier: [String]] = [:]

    public nonisolated static func supported(status: JSONObject, _ name: String = capability) -> Bool {
        status.list("capabilities").contains { $0.stringValue == name }
    }

    public static func check(_ hub: DesktopControlHub, _ name: String = capability, desktopKey: String = "") async -> Bool? {
        if let link = hub.openLink(for: desktopKey), let cached = known[ObjectIdentifier(link)] { return cached.contains(name) }
        guard let link = try? await hub.link(to: desktopKey),
            let status = try? await link.request("status", [:], timeout: 10)
        else { return nil }
        let names = status.list("capabilities").compactMap { $0.stringValue }
        known[ObjectIdentifier(link)] = names
        return names.contains(name)
    }
}
