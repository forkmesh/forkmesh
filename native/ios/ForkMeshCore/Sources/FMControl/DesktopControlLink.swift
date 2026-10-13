// New for the native app (WS7): one open, authenticated channel to the owner's desktop, whichever
// way it got there. On the same network it is the local control channel over the desktop's mesh
// port (FMChat's LocalControlService); away from home it is the same protocol carried through the
// relay (RelayControlConnection). Features above this line (Hosts, the unified Agents list, the
// Watch through the phone) speak requests and events and never care which.

import FMBase
import FMChat
import FMNet
import Foundation

/// How the phone reached the desktop.
public enum ControlTransport: String, Equatable, Sendable {
    /// The desktop's mesh port on this network.
    case lan
    /// The account's relay socket, sealed end to end between the two devices.
    case relay

    public var label: String {
        switch self {
        case .lan: return "On this network"
        case .relay: return "Through the relay"
        }
    }
}

/// An open channel to the desktop.
@MainActor
public protocol DesktopControlLink: AnyObject {
    var transport: ControlTransport { get }
    var isOpen: Bool { get }
    /// The desktop's machine name.
    var desktopName: String { get }
    /// One request; throws `LocalControlException` with the desktop's error code.
    func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject
    /// Every `{event, data}` frame from the desktop, until removed.
    @discardableResult
    func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID
    func removeEventTap(_ id: UUID)
}

extension DesktopControlLink {
    public func request(_ method: String, _ params: JSONObject = [:]) async throws -> JSONObject {
        try await request(method, params, timeout: 20)
    }
}

/// The LAN channel, as a link: LocalControlService keeps the connection (pairing, reconnects,
/// files, agent bells) and this only borrows it.
@MainActor
public final class LanControlLink: DesktopControlLink {
    public let service: LocalControlService

    public init(service: LocalControlService) {
        self.service = service
    }

    public var transport: ControlTransport { .lan }
    public var isOpen: Bool { service.connected }
    public var desktopName: String { service.desktopName }
    /// The paired desktop's Ed25519 key, proven by the handshake.
    public var desktopKey: String { service.desktopKey }

    public func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject {
        try await service.call(method, params, timeout: timeout)
    }

    @discardableResult
    public func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID {
        service.addEventTap(tap)
    }

    public func removeEventTap(_ id: UUID) { service.removeEventTap(id) }
}

/// A sentence for a control error code, for the screens.
public enum ControlErrorText {
    public static func describe(_ error: Error) -> String {
        if let control = error as? LocalControlException { return withReason(describe(code: control.code), control) }
        return String(describing: error)
    }

    public static func isCancellation(_ error: Error) -> Bool {
        error is CancellationError || (error as? HTTPError) == .cancelled
    }

    public static func withReason(_ text: String, _ control: LocalControlException) -> String {
        let reason = control.message.trimmingCharacters(in: .whitespacesAndNewlines)
        return reason.isEmpty ? text : "\(text) \(reason)"
    }

    public static func describe(code: String) -> String {
        switch code {
        case "not_connected", "closed": return "The desktop is not connected."
        case "timeout": return "The desktop did not answer in time."
        case "not_paired": return "Approve this phone on the desktop first."
        case "pair_on_lan":
            return "Sign this phone and the desktop in to the same ForkMesh account, or approve this phone on the desktop while both are on the same network; after that it works from anywhere."
        case "not_owner": return "Only the account that owns this desktop can manage its hosts."
        case "confirmation_required", "confirmation_mismatch":
            return "That change was not confirmed. Try again."
        case "confirmation_expired": return "The confirmation expired. Try again."
        case "name_mismatch": return "The typed name does not match the host."
        case "busy": return "The desktop is already installing a host. Wait for it to finish."
        case "already_running": return "That action is already running."
        case "unknown_host": return "The desktop no longer has that host."
        case "not_a_mirror_node": return "Only mirror nodes run the ForkMesh service the desktop can control."
        case "not_a_vultr_host": return "Only a Vultr server the desktop created can be destroyed from here."
        case "vultr_unreachable": return "The desktop could not reach Vultr."
        case "hosts_page_unavailable": return "Open the desktop's Hosts page once, then try again."
        case "lan_only": return "Files move only on the same network."
        case "relay_unavailable":
            return "The relay does not carry desktop control yet, or the desktop is offline."
        case "desktop_unreachable": return "The desktop stopped answering through the relay."
        case "channel_gone", "relay_reconnected", "idle":
            return "The connection to the desktop was reset. Try again."
        case "account_check_failed":
            return "The desktop could not confirm this phone with your ForkMesh account. Try again in a moment."
        case "revoked": return "This phone is no longer on your ForkMesh account, so it cannot control the desktop."
        case "unsupported": return "This desktop's ForkMesh is too old for that. Update it."
        case "no_checkout": return "The desktop has no local checkout of that repository."
        case "bad_task": return "Write what the agent should do (8000 characters at most)."
        case "not_started": return "The desktop could not start the agent."
        case "images_unsupported": return "Update ForkMesh on the desktop to send it photos."
        case "too_many_images", "bad_images": return "A prompt can carry 5 photos at most."
        case "bad_image": return "The desktop takes JPEG, PNG, GIF or WebP photos of 5 MB at most."
        case "bad_chunk", "unknown_image", "image_incomplete":
            return "A photo did not reach the desktop whole. Send it again."
        case "cannot_save_image": return "The desktop could not save the photo."
        case "not_steerable":
            return "That Claude Code session no longer takes messages. Steer it in the terminal that started it."
        case "outside_cli_unsupported":
            return "Only Claude Code started outside ForkMesh can be steered from here. Steer this agent in the terminal that started it."
        case "outside_stop_unsupported":
            return "An agent started outside ForkMesh cannot be stopped from here. Ask it to stop in a message, or stop it in the terminal that started it."
        case "outside_other_user":
            return "That agent runs in another login on the desktop. Open ForkMesh in that login to follow and steer it."
        case "outside_images_unsupported": return "Photos cannot be sent to an agent started outside ForkMesh."
        case "outside_refuses_messages":
            return "That Claude Code session refuses messages from other sessions (crossSessionInbound is \"refuse\"). Steer it in the terminal that started it."
        case "outside_held_no_terminal":
            return "That Claude Code session would hold the message for approval, but it has no terminal to approve it in. Turn on Accept messages from ForkMesh (\"crossSessionInbound\": \"accept\" in its settings.json) to steer it."
        case "typing_rate_limited":
            return "Too many prompts from this phone in the last minute. Wait a moment and try again."
        case "terminal_user_typing": return "Someone is typing in that terminal right now. Try again in a moment."
        case "terminal_remote_off":
            return "Phone typing is turned off in that terminal. Press Ctrl-^ there (or restart it without --no-remote) to turn it back on."
        case "bad_limit": return "Choose between 1 and 99 agents at once."
        case "not_queued": return "That agent is no longer waiting in the desktop's queue."
        case "not_typeable": return "That terminal can no longer be typed into. It may have closed."
        case "unknown_terminal": return "That terminal has ended on the desktop."
        case "unknown_repository": return "The desktop does not have that repository open."
        case "unsupported_provider": return "The desktop can only start Claude Code or Codex in a terminal."
        case "cli_missing": return "That agent's CLI is not installed on the desktop. Install it there first."
        case "guardrails_on":
            return "This repository's agent guardrails on the desktop do not allow a free-form terminal."
        case "terminal_vm":
            return "The desktop runs agents in its virtual machine, so it cannot open an agent terminal."
        case "terminal_limit": return "Six terminals (agents and SSH sessions) already run on the desktop. End one first."
        case "ssh_missing": return "The desktop has no ssh (or sshpass, for a password host) installed to open the session."
        case "ssh_unavailable": return "The desktop could not use this host's saved SSH key or address. Check the host on the desktop."
        case "terminal_failed": return "The desktop could not open a terminal."
        case "unknown_session": return "That agent is no longer on the desktop."
        case "no_worktree": return "That agent has no folder left on the desktop to open a terminal in."
        case "cloud_session": return "That agent runs on another node, so its conversation is not on the desktop."
        case "terminal_open":
            return "That agent is open in a terminal on the desktop. Continue it there, or end the terminal first."
        case "bad_input": return "The desktop refused that input."
        case "held_for_approval":
            return "Sent. Claude Code is holding it for approval: approve it in the terminal that started it, or turn on Accept messages from ForkMesh (\"crossSessionInbound\": \"accept\" in that Claude Code's settings.json) to steer without approving."
        default: return code.replacingOccurrences(of: "_", with: " ")
        }
    }
}
