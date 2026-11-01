import FMApi
import SwiftUI
import UIKit

@MainActor
enum FmMessageHandoff {
    static func send(_ text: String) {
        guard MessageHandoff.canHandOff(text) else { return }
        compose(MessageHandoff.composeTarget(text))
    }

    static func compose(_ target: ComposeTarget) {
        let request: () -> Void = {
            if let navigator = AppNavigator.shared {
                navigator.router.requestCompose(target)
            } else {
                UIPasteboard.general.string = target.text
            }
        }
        if let root = rootController(), root.presentedViewController != nil {
            root.dismiss(animated: true, completion: request)
        } else {
            request()
        }
    }

    private static func rootController() -> UIViewController? {
        let scenes: [UIWindowScene] = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        let scene: UIWindowScene? = scenes.first { $0.activationState == .foregroundActive } ?? scenes.first
        return scene?.windows.first(where: { $0.isKeyWindow })?.rootViewController
    }
}

struct FmMessageToolsModifier: ViewModifier {
    let text: String

    @ViewBuilder
    func body(content: Content) -> some View {
        if MessageHandoff.canHandOff(text) {
            content
                .contentShape(Rectangle())
                .onTapGesture { open() }
                .accessibilityAction(named: Text("Open in composer")) { open() }
        } else {
            content
        }
    }

    @MainActor
    private func open() {
        FmHaptics.tap()
        FmMessageHandoff.send(text)
    }
}

extension View {
    func fmMessageTools(_ text: String?) -> some View {
        return modifier(FmMessageToolsModifier(text: text ?? ""))
    }
}
