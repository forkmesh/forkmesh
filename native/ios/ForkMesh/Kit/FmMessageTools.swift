import FMApi
import SwiftUI
import UIKit

struct FmMessageTools: View {
    @Environment(\.fmPalette) private var fm
    @State private var copied: Bool = false

    private let text: String
    private let color: Color?

    init(_ text: String, color: Color? = nil) {
        self.text = text
        self.color = color
    }

    var body: some View {
        if MessageHandoff.canHandOff(text) {
            HStack(spacing: 0) {
                tool(copied ? "check" : "content_copy", label: copied ? "Copied" : "Copy message", action: copy)
                tool("send_outlined", label: "Send to agent", action: send)
            }
            .fixedSize()
            .accessibilityElement(children: .contain)
            .accessibilityIdentifier("message-tools")
        }
    }

    private func tool(_ icon: String, label: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            FmIcon(icon, size: 13, color: color ?? fm.textTertiary)
                .frame(width: 24, height: 24)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text(label))
    }

    private func copy() {
        FmHaptics.tap()
        UIPasteboard.general.string = MessageHandoff.cleaned(text)
        copied = true
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: 1_500_000_000)
            copied = false
        }
    }

    private func send() {
        FmHaptics.tap()
        FmMessageHandoff.send(text)
    }
}

@MainActor
enum FmMessageHandoff {
    static func send(_ text: String) {
        guard MessageHandoff.canHandOff(text) else { return }
        let target: ComposeTarget = MessageHandoff.composeTarget(text)
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
    let color: Color?
    let alignment: VerticalAlignment

    func body(content: Content) -> some View {
        HStack(alignment: alignment, spacing: FmSpace.x1) {
            content
            FmMessageTools(text, color: color)
        }
    }
}

extension View {
    func fmMessageTools(_ text: String?, color: Color? = nil, alignment: VerticalAlignment = .firstTextBaseline) -> some View {
        return modifier(FmMessageToolsModifier(text: text ?? "", color: color, alignment: alignment))
    }
}
