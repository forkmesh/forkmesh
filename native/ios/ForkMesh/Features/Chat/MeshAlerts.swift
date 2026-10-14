import FMAccount
import FMApi
import FMBase
import FMChat
import Foundation
import UserNotifications

@MainActor
enum MeshAlerts {
    static var onScreen = false
    static var foreground = true
    private static var shown = false
    private static var asked = false
    private static var listener: ListenerToken?

    static func install(mesh: LocalMeshService, hub: ChatHub) {
        mesh.onIncoming = { (message: ChatMessage, unread: Int) in
            if hub.isFromBlocked(message) { return }
            let watching: Bool = MeshAlerts.foreground && MeshAlerts.onScreen && hub.inMesh
            guard let alert = meshAlert(message, unread: unread, watching: watching, now: Date()) else { return }
            MeshAlerts.shown = true
            Task { await MeshAlerts.post(alert) }
        }
        listener = mesh.addListener {
            MainActor.assumeIsolated {
                if mesh.unread == 0 && MeshAlerts.shown {
                    MeshAlerts.shown = false
                    Task { await MeshAlerts.clear() }
                }
                if !MeshAlerts.asked && MeshAlerts.foreground && mesh.peerCount > 0 {
                    MeshAlerts.asked = true
                    Task { await MeshAlerts.askOnce() }
                }
            }
        }
    }

    private static func isLocal(_ info: [AnyHashable: Any]) -> Bool { info["meshLocal"] != nil }

    private static func isMeshPush(_ notification: UNNotification) -> Bool {
        let info: [AnyHashable: Any] = notification.request.content.userInfo
        return info["topic"] as? String == "chat" && info["conversation"] as? String == meshConversation
    }

    private static func post(_ alert: MeshAlert) async {
        let center: UNUserNotificationCenter = UNUserNotificationCenter.current()
        let settings: UNNotificationSettings = await center.notificationSettings()
        if settings.authorizationStatus != .authorized && settings.authorizationStatus != .provisional { return }
        let content: UNMutableNotificationContent = UNMutableNotificationContent()
        content.title = alert.title
        content.body = alert.body
        content.sound = .default
        content.threadIdentifier = meshAlertIdentifier
        content.userInfo = ["topic": "chat", "conversation": meshConversation, "meshLocal": "1"]
        let request: UNNotificationRequest = UNNotificationRequest(
            identifier: meshAlertIdentifier, content: content, trigger: nil)
        do {
            try await center.add(request)
        } catch {
            Log.warning("mesh", "mesh alert not posted: \(error.localizedDescription)")
            return
        }
        let delivered: [UNNotification] = await center.deliveredNotifications()
        let wakes: [String] = delivered.filter {
            isMeshPush($0) && !isLocal($0.request.content.userInfo) && $0.request.identifier != meshAlertIdentifier
        }
        .map { $0.request.identifier }
        if !wakes.isEmpty { center.removeDeliveredNotifications(withIdentifiers: wakes) }
    }

    private static func clear() async {
        let center: UNUserNotificationCenter = UNUserNotificationCenter.current()
        let delivered: [UNNotification] = await center.deliveredNotifications()
        let ids: [String] = delivered.filter { isMeshPush($0) }.map { $0.request.identifier }
        center.removeDeliveredNotifications(withIdentifiers: ids + [meshAlertIdentifier])
    }

    private static func askOnce() async {
        let center: UNUserNotificationCenter = UNUserNotificationCenter.current()
        let settings: UNNotificationSettings = await center.notificationSettings()
        if settings.authorizationStatus != .notDetermined { return }
        _ = try? await center.requestAuthorization(options: [.alert, .sound, .badge])
    }
}
