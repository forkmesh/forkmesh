// New for the native app (WS7): the Hosts area's state. Reads the desktop's host list, keeps it
// fresh from the desktop's bells (hosts.changed, hosts.action), and runs every change as the two
// steps the desktop demands: hosts.prepare (a one-use token and what will happen) then hosts.run.

import FMBase
import FMChat
import FMControl
import Foundation
import Observation

/// What the Hosts area needs from the desktop channel; `DesktopControlHub` provides it.
@MainActor
public protocol HostsChannel: AnyObject {
    func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject
    @discardableResult func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID
    func removeEventTap(_ id: UUID)
    var reach: DesktopReach { get }
}

extension DesktopControlHub: HostsChannel {}

@MainActor
@Observable
public final class HostsStore {
    @ObservationIgnored public let channel: HostsChannel
    @ObservationIgnored private let now: () -> Int

    public private(set) var snapshot: HostsSnapshot?
    public private(set) var loading = false
    public private(set) var error = ""
    /// Actions the desktop has finished for this phone, newest first.
    public private(set) var outcomes: [HostActionOutcome] = []
    /// "action:host" of what this phone started and still waits on.
    public private(set) var running: Set<String> = []

    @ObservationIgnored private var tapId: UUID?
    @ObservationIgnored private var refreshing = false
    @ObservationIgnored private var again = false

    public init(channel: HostsChannel, now: @escaping () -> Int = { Int(Date().timeIntervalSince1970 * 1000) }) {
        self.channel = channel
        self.now = now
        tapId = channel.addEventTap { [weak self] name, data in
            self?.onEvent(name, data)
        }
    }

    public var reach: DesktopReach { channel.reach }

    private func onEvent(_ name: String, _ data: JSONObject) {
        switch name {
        case "hosts.changed":
            Task { await self.refresh() }
        case "hosts.action":
            let outcome = HostActionOutcome(json: data, atMs: now())
            running.remove("\(outcome.action):\(outcome.host.lowercased())")
            outcomes.insert(outcome, at: 0)
            if outcomes.count > 20 { outcomes.removeLast(outcomes.count - 20) }
        default:
            break
        }
    }

    public func refresh() async {
        if refreshing {
            again = true
            return
        }
        refreshing = true
        loading = true
        defer {
            refreshing = false
            loading = false
        }
        repeat {
            again = false
            do {
                snapshot = HostsSnapshot(json: try await channel.request("hosts.list", [:], timeout: 20))
                error = ""
            } catch {
                self.error = ControlErrorText.describe(error)
            }
        } while again
    }

    public func detail(_ host: String) async throws -> HostRow {
        HostRow(json: try await channel.request("hosts.detail", ["host": .string(host)], timeout: 20))
    }

    public func createOptions() async throws -> HostCreateOptions {
        HostCreateOptions(json: try await channel.request("hosts.options", [:], timeout: 30))
    }

    /// Step one of any change: the desktop describes it and hands back a token.
    public func prepare(_ action: HostAction, host: String) async throws -> HostConfirmation {
        let result = try await channel.request(
            "hosts.prepare", ["action": .string(action.rawValue), "host": .string(host)], timeout: 20)
        guard let confirmation = HostConfirmation(json: result) else { throw LocalControlException("bad_answer") }
        return confirmation
    }

    /// Step two: run it with the token (and, for destroy, the typed name). Returns the desktop's
    /// "started" line; the outcome arrives later as a hosts.action event.
    @discardableResult
    public func run(
        _ confirmation: HostConfirmation, typedName: String = "", plan: String = "", region: String = ""
    ) async throws -> String {
        if confirmation.action.opensTerminal { throw LocalControlException("unsupported_action") }
        if !confirmation.accepts(typed: typedName) { throw LocalControlException("name_mismatch") }
        var params: JSONObject = [
            "action": .string(confirmation.action.rawValue),
            "host": .string(confirmation.host),
            "token": .string(confirmation.token),
        ]
        if confirmation.needsTypedName { params["confirmName"] = .string(typedName) }
        if !plan.isEmpty { params["plan"] = .string(plan) }
        if !region.isEmpty { params["region"] = .string(region) }
        let result = try await channel.request("hosts.run", params, timeout: 30)
        let host = result.str("host", confirmation.host)
        running.insert("\(confirmation.action.rawValue):\(host.lowercased())")
        await refresh()
        return result.str("message")
    }

    public func openTerminal(_ confirmation: HostConfirmation, cols: Int = 80, rows: Int = 24) async throws -> String {
        if !confirmation.action.opensTerminal { throw LocalControlException("unsupported_action") }
        let result = try await channel.request(
            "hosts.run",
            [
                "action": .string(confirmation.action.rawValue),
                "host": .string(confirmation.host),
                "token": .string(confirmation.token),
                "cols": .int(Int64(cols)),
                "rows": .int(Int64(rows)),
            ],
            timeout: 30)
        let terminal = result.str("terminal")
        if terminal.isEmpty { throw LocalControlException("terminal_failed") }
        return terminal
    }

    public func openTerminals(host: String) async throws -> [DesktopTerminalInfo] {
        let key = host.lowercased()
        return DesktopTerminalInfo.list(try await channel.request("terminal.list", [:], timeout: 20))
            .filter { $0.provider == "ssh" && $0.host.lowercased() == key }
    }

    public func isRunning(_ action: HostAction, host: String) -> Bool {
        running.contains("\(action.rawValue):\(host.lowercased())")
    }

    public func host(named name: String) -> HostRow? {
        snapshot?.hosts.first { $0.id == name.lowercased() }
    }
}
