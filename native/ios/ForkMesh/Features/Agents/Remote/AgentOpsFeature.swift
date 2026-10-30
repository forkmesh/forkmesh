import FMApi
import FMAgents
import FMBase
import FMControl
import Foundation
import LocalAuthentication
import SwiftUI
import UserNotifications

@MainActor
enum AgentOpsFeature {
    static func register() {
        if Services.get(AgentOpsClient.self) != nil { return }
        guard let hub = HostsFeature.hub else { return }
        let client = AgentOpsClient(channel: hub)
        Services.put(client)
        let queue = ApprovalsQueue(client: client)
        Services.put(queue)
        queue.start()
    }

    static var client: AgentOpsClient? {
        register()
        return Services.get(AgentOpsClient.self)
    }

    private static var desktopClients: [String: AgentOpsClient] = [:]

    static func client(desktopKey: String) -> AgentOpsClient? {
        guard let shared = client, let hub = HostsFeature.hub else { return nil }
        if desktopKey.isEmpty || desktopKey == hub.activeKey { return shared }
        if let made = desktopClients[desktopKey] { return made }
        let made = AgentOpsClient(channel: hub.channel(for: desktopKey))
        desktopClients[desktopKey] = made
        return made
    }

    static func client(for row: UnifiedAgentRow) -> AgentOpsClient? {
        client(desktopKey: row.desktopKey)
    }

    static var approvals: ApprovalsQueue? {
        register()
        return Services.get(ApprovalsQueue.self)
    }

    static func authenticate(reason: String) async -> Bool {
        let context = LAContext()
        var error: NSError?
        guard context.canEvaluatePolicy(.deviceOwnerAuthentication, error: &error) else { return true }
        do {
            return try await context.evaluatePolicy(.deviceOwnerAuthentication, localizedReason: reason)
        } catch {
            return false
        }
    }

    static func scheduleUsageReminders(_ reminders: [UsageReminder], nowMs: Int64) async {
        let center = UNUserNotificationCenter.current()
        let settings = await center.notificationSettings()
        guard settings.authorizationStatus == .authorized || settings.authorizationStatus == .provisional else { return }
        for reminder in reminders where reminder.fireAtMs > nowMs {
            let content = UNMutableNotificationContent()
            content.title = reminder.title
            content.body = reminder.body
            content.threadIdentifier = "agent-usage"
            let seconds = max(60, TimeInterval(reminder.fireAtMs - nowMs) / 1_000)
            let trigger = UNTimeIntervalNotificationTrigger(timeInterval: seconds, repeats: false)
            try? await center.add(UNNotificationRequest(identifier: reminder.id, content: content, trigger: trigger))
        }
    }
}

struct AgentOpsUnavailableView: View {
    var body: some View {
        ContentUnavailableView(
            "Desktop not connected", systemImage: "desktopcomputer",
            description: Text("Pair this phone with your desktop to manage its agents."))
    }
}

@MainActor
final class AgentOpsApprovalDecider: AgentApprovalDeciding {
    struct Failed: Error {}

    func decide(sessionId: String, approvalId: String, approve: Bool) async throws {
        guard let queue = AgentOpsFeature.approvals else { throw Failed() }
        let done = approve ? await queue.approve(approvalId) : await queue.deny(approvalId)
        if !done { throw Failed() }
    }
}
