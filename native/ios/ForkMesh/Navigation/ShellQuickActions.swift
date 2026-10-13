import FMApi
import UIKit

@MainActor
enum ShellQuickActions {
    static let payloadKey = "payload"

    static func refresh(recents: [Route]) {
        var items: [UIApplicationShortcutItem] = ShellQuickAction.availableActions.map { (action: ShellQuickAction) in
            UIApplicationShortcutItem(
                type: action.rawValue, localizedTitle: action.title, localizedSubtitle: nil,
                icon: UIApplicationShortcutIcon(systemImageName: action.symbol), userInfo: nil)
        }
        for item in ShellQuickAction.dynamicItems(recents: recents) {
            items.append(UIApplicationShortcutItem(
                type: item.action.rawValue, localizedTitle: item.title, localizedSubtitle: item.action.title,
                icon: UIApplicationShortcutIcon(systemImageName: item.action.symbol),
                userInfo: [payloadKey: item.payload as NSString]))
        }
        UIApplication.shared.shortcutItems = items
    }

    @discardableResult
    static func perform(_ item: UIApplicationShortcutItem) -> Bool {
        guard let action = ShellQuickAction(rawValue: item.type) else { return false }
        let payload: String = (item.userInfo?[payloadKey] as? String) ?? ""
        guard let route = action.route(payload: payload) else { return false }
        if let navigator = AppNavigator.shared {
            navigator.router.show(route)
        } else {
            AppNavigator.early.append(route)
        }
        return true
    }
}

@MainActor
final class ShellSceneDelegate: NSObject, UIWindowSceneDelegate {
    func scene(
        _ scene: UIScene, willConnectTo session: UISceneSession, options connectionOptions: UIScene.ConnectionOptions
    ) {
        guard let item = connectionOptions.shortcutItem else { return }
        Task { @MainActor in
            ShellQuickActions.perform(item)
        }
    }

    func windowScene(
        _ windowScene: UIWindowScene, performActionFor shortcutItem: UIApplicationShortcutItem,
        completionHandler: @escaping @MainActor @Sendable (Bool) -> Void
    ) {
        completionHandler(ShellQuickActions.perform(shortcutItem))
    }
}
