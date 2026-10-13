// View model of the home shell: the selected page, the prompt bar and its
// mode, the Agents reload generation, and where a tapped push goes. The view
// owns the navigation stack; this decides.

import FMAccount
import FMBase
import Foundation
import Observation

/// Where a tapped push takes the user (issue #724).
public enum ShellPushRoute: Equatable, Sendable {
    /// The Chat page (the conversation was switched when one had unread).
    case chat
    /// The account inbox, when the banner named nothing this phone can open.
    case notifications
    /// The record the push was about.
    case repo(NotificationDeepLink)
    case ping(String)
    case agent(String)
}

/// What Send on the prompt bar did.
public enum ShellPromptResult: Equatable, Sendable {
    /// Nothing to send (blank), or nothing to send it with.
    case ignored
    /// Open the new-agent form with this prompt.
    case startAgent(String)
    case handled(String)
    case failed(String)
}

@MainActor
@Observable
public final class HomeShellModel {
    /// The page on screen: `HomeShellCatalog.destinations[index]`.
    public private(set) var index = HomeShellCatalog.defaultIndex
    /// What is typed in the prompt bar. It outlives page changes, so a thought
    /// half-written on one page can be sent from another.
    public var prompt = ""
    /// Bumped when the prompt bar starts a run, so the Agents list reloads.
    public private(set) var agentsGeneration = 0
    public private(set) var promptBusy = false

    /// The relay, as the Chat feature exposes it; nil draws an offline relay.
    @ObservationIgnored public var chat: ShellChatBridge?
    @ObservationIgnored private let notifications: NotificationService?
    @ObservationIgnored private var routingPush = false
    @ObservationIgnored public let targets: ShellPromptTargets

    public init(chat: ShellChatBridge?, notifications: NotificationService?, targets: ShellPromptTargets? = nil) {
        self.chat = chat
        self.notifications = notifications
        self.targets = targets ?? ShellPromptTargets()
    }

    public var destination: ShellDestination { HomeShellCatalog.destinations[index] }

    public var tab: AppTab { HomeShellCatalog.tab(at: index) }

    public func select(tab: AppTab) {
        if tab != self.tab { select(HomeShellCatalog.index(of: tab)) }
    }

    public var relayState: ShellRelayState { chat?.relayState ?? .offline }
    public var onlineCount: Int { chat?.onlineCount ?? 0 }
    public var chatUnread: Int { chat?.badgeUnread ?? 0 }
    public var conversation: String { chat?.currentConversation ?? "#\(guestChatChannelName)" }

    public var promptTarget: ShellPromptTarget? { targets.routing }

    public var promptHint: String {
        promptTarget?.placeholder ?? shellPromptHint
    }

    public var promptModeLabel: String {
        if let target = promptTarget { return "\(target.title). Switch to agent" }
        if let target = targets.active { return "Prompt starts an agent. Switch to \(target.label)" }
        return "Prompt starts an agent"
    }

    public func togglePromptMode() {
        guard targets.active != nil else { return }
        targets.bypassed.toggle()
    }

    /// `_select`: the one place the chat badge is cleared, since opening the
    /// Chat tab is the user going to look. (The view pops any page opened over
    /// the tabs.)
    public func select(_ i: Int) {
        guard HomeShellCatalog.destinations.indices.contains(i) else { return }
        let label = HomeShellCatalog.destinations[i].label
        if label == "Chat" { chat?.markBadgeSeen() }
        index = i
    }

    /// `_submitPrompt`.
    public func submitPrompt() async -> ShellPromptResult {
        let text = prompt
        if text.dartTrimmed.isEmpty { return .ignored }
        if let target = promptTarget {
            if promptBusy { return .ignored }
            promptBusy = true
            defer { promptBusy = false }
            do {
                let message = try await target.submit(text.dartTrimmed)
                if prompt == text { prompt = "" }
                return .handled(message)
            } catch {
                return .failed(ShellPromptTargets.message(error))
            }
        }
        return .startAgent(text)
    }

    /// The new-agent form started a run from the prompt.
    public func agentStarted() {
        prompt = ""
        agentsGeneration += 1
        select(HomeShellCatalog.agentsIndex)
    }

    /// `_routePushOpen` for one tap: the conversation for chat and direct
    /// messages, the record for an inbox notification (marking it read), and
    /// the inbox itself when the banner named nothing. Nil while another tap
    /// is still being routed.
    public func routePush(
        isChat: Bool, isDirectMessage: Bool, alertId: String, href: String, agent: String = "",
        ping: ForkNotification? = nil, conversation: String = ""
    ) async -> ShellPushRoute? {
        if routingPush { return nil }
        routingPush = true
        defer { routingPush = false }
        if isChat || isDirectMessage {
            if let chat,
                let target = pushChatConversation(
                    isDirectMessage: isDirectMessage, unreadConversations: chat.unreadConversations,
                    pushed: conversation)
            {
                chat.switchConversation(target)
            }
            select(HomeShellCatalog.chatIndex)
            return .chat
        }
        var row = findRow(alertId)
        if row == nil, let ping, let notifications {
            row = notifications.insertFromPush(ping)
        }
        if row == nil, !alertId.isEmpty, let notifications {
            await notifications.refresh(silent: true)
            row = findRow(alertId)
        }
        if let row, let notifications {
            notifications.markReadSoon(row)
        }
        if !agent.isEmpty { return .agent(agent) }
        if let row, case let .agent(id)? = InboxRoutes.route(kind: row.kind, href: row.href, meta: row.meta, repo: row.repo) {
            return .agent(id)
        }
        if let link = pushDeepLink(pushHref: href, rowHref: row?.href) { return .repo(link) }
        if let row { return .ping(row.id) }
        return .notifications
    }

    private func findRow(_ alertId: String) -> ForkNotification? {
        if alertId.isEmpty { return nil }
        return notifications?.notifications.first { $0.id == alertId }
    }
}

/// The guest chat channel, which the Kit also defines for the views.
public let guestChatChannelName = "general"
