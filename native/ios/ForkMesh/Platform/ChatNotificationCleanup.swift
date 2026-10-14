import Foundation
import UserNotifications

enum ChatNotificationCleanup {
    static func remove(messageId: String) {
        guard !messageId.isEmpty else { return }
        Task { await removeDelivered(messageId: messageId) }
    }

    static func removeDelivered(messageId: String) async {
        let center: UNUserNotificationCenter = UNUserNotificationCenter.current()
        let delivered: [UNNotification] = await center.deliveredNotifications()
        let ids: [String] = delivered.filter { ($0.request.content.userInfo["messageId"] as? String) == messageId }
            .map { $0.request.identifier }
        center.removeDeliveredNotifications(withIdentifiers: ids + [messageId])
    }
}
