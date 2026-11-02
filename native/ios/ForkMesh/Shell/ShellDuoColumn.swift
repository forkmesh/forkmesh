import FMApi
import SwiftUI

@MainActor
enum ShellDuo {
    static let device: Bool = ShellPanes.isDuo(model: DeviceInfoProvider.machineIdentifier())

    static let nativeTabs: Bool = {
        guard #available(iOS 26.0, *) else { return device }
        return device || UIDevice.current.userInterfaceIdiom == .phone
    }()
}

struct ShellDuoTab: Identifiable {
    let id: String
    let title: String
    let symbol: String
    let badge: Int
    let members: [ShellBarItem]
}

struct ShellDuoFolder: Identifiable {
    let id: String
    let title: String
    let members: [ShellBarItem]
}

struct ShellDuoBars: ViewModifier {
    @Environment(\.fmPalette) private var fm

    let on: Bool
    let onNewAgent: (() -> Void)?
    let onPalette: () -> Void
    let onVoice: (() -> Void)?
    var showsMore: Bool = true

    func body(content: Content) -> some View {
        content.toolbar {
            if on {
                if let onNewAgent {
                    #if compiler(>=6.4)
                    if #available(iOS 27.0, *) {
                        ToolbarItem(placement: .topBarTrailing) { newAgent(onNewAgent) }
                            .visibilityPriority(.high)
                    } else {
                        ToolbarItem(placement: .topBarTrailing) { newAgent(onNewAgent) }
                    }
                    #else
                    ToolbarItem(placement: .topBarTrailing) { newAgent(onNewAgent) }
                    #endif
                }
                if showsMore {
                    if #available(iOS 26.0, *) {
                        ToolbarSpacer(.fixed, placement: .topBarTrailing)
                    }
                    ToolbarItem(placement: .topBarTrailing) {
                        Menu("More", systemImage: "ellipsis") {
                            Button("Commands", systemImage: "command", action: onPalette)
                            if let onVoice {
                                Button("Speak a new agent", systemImage: "mic", action: onVoice)
                            }
                        }
                        .tint(fm.textPrimary)
                        .accessibilityIdentifier("shell-duo-more")
                    }
                }
            }
        }
    }

    private func newAgent(_ action: @escaping () -> Void) -> some View {
        Button("New agent", systemImage: "plus", action: action)
            .tint(fm.textPrimary)
            .accessibilityIdentifier("shell-duo-new-agent")
    }
}

struct ShellDuoListBars: ViewModifier {
    func body(content: Content) -> some View {
        #if compiler(>=6.4)
        if #available(iOS 27.1, *) {
            content.toolbarVerticalBehavior(.disabled)
        } else {
            content
        }
        #else
        content
        #endif
    }
}

struct ShellFoldReader: ViewModifier {
    @Binding var fold: CGRect?

    func body(content: Content) -> some View {
        #if compiler(>=6.4)
        if #available(iOS 27.1, *) {
            content.onGeometryChange(for: CGRect?.self) { (proxy: GeometryProxy) -> CGRect? in
                let frame: CGRect? = proxy.reservedRegions(kind: .division).first(where: \.isActive)?.frame
                return frame.flatMap { $0.height > $0.width ? $0 : nil }
            } action: { (next: CGRect?) in
                fold = next
            }
        } else {
            content
        }
        #else
        content
        #endif
    }
}
