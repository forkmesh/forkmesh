import FMApi
import FMChat
import Foundation
import SwiftUI
#if canImport(UserNotifications)
import UserNotifications
#endif

struct ChatMessageLaterMenu: View {
    let actions: ChatRowActions

    var body: some View {
        if let onSave = actions.onSave {
            Button(action: onSave) {
                Label(actions.saved ? "Remove from saved" : "Save for later", systemImage: actions.saved ? "bookmark.slash" : "bookmark")
            }
        }
        if let onRemind = actions.onRemind {
            Menu {
                ForEach(ChatReminderOption.allCases) { (option: ChatReminderOption) in
                    Button(option.label) { onRemind(option) }
                }
            } label: {
                Label("Remind me about this", systemImage: "alarm")
            }
        }
        if let onShare = actions.onShare {
            Button(action: onShare) { Label("Share message", systemImage: "arrowshape.turn.up.right") }
        }
    }
}

struct ChatShareRequest: Identifiable {
    let id: UUID = UUID()
    let message: ChatMessage
    let text: String
}

@MainActor
enum ChatReminders {
    static func schedule(
        _ option: ChatReminderOption, message: ChatMessage, text: String, place: String, now: Date = Date()
    ) async -> String {
        #if canImport(UserNotifications)
        let center: UNUserNotificationCenter = UNUserNotificationCenter.current()
        let allowed: Bool = (try? await center.requestAuthorization(options: [.alert, .sound])) ?? false
        if !allowed { return "Allow notifications for ForkMesh to get reminders." }
        let fire: Date = option.fireDate(from: now)
        let content: UNMutableNotificationContent = UNMutableNotificationContent()
        content.title = "Reminder: \(message.senderName) in \(place)"
        let body: String = text.isEmpty ? message.fileName : text
        content.body = String(body.prefix(240))
        content.sound = .default
        content.userInfo = ["chatConversation": message.conversation, "chatMessageId": message.id]
        let trigger: UNTimeIntervalNotificationTrigger = UNTimeIntervalNotificationTrigger(
            timeInterval: max(1, fire.timeIntervalSince(now)), repeats: false)
        let request: UNNotificationRequest = UNNotificationRequest(
            identifier: "chat-reminder-\(message.id)-\(Int(fire.timeIntervalSince1970))", content: content, trigger: trigger)
        do {
            try await center.add(request)
        } catch {
            return "The reminder could not be set."
        }
        let formatter: DateFormatter = DateFormatter()
        formatter.doesRelativeDateFormatting = true
        formatter.dateStyle = .medium
        formatter.timeStyle = .short
        return "I'll remind you \(formatter.string(from: fire))."
        #else
        return "Reminders are not available here."
        #endif
    }
}
