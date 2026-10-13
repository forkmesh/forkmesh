import SwiftUI

@MainActor @Observable final class ShellDock {
    static let gap: CGFloat = FmSpace.x3
    static let barSpace: String = "shell-bar"

    var bottom: CGFloat = 0
    var barBottom: CGFloat = 0
    var clearance: CGFloat = 0
    var top: CGFloat = 0
    private var heights: [UUID: CGFloat] = [:]

    var height: CGFloat {
        heights.values.max() ?? 0
    }

    func set(_ key: UUID, _ height: CGFloat) {
        if heights[key] != height {
            heights[key] = height
        }
    }

    func remove(_ key: UUID) {
        heights[key] = nil
    }
}

private struct ShellDockKey: EnvironmentKey {
    static let defaultValue: ShellDock? = nil
}

extension EnvironmentValues {
    var shellDock: ShellDock? {
        get { self[ShellDockKey.self] }
        set { self[ShellDockKey.self] = newValue }
    }
}

private struct ShellDocked<Dock: View>: ViewModifier {
    @Environment(\.shellDock) private var shell
    @State private var key: UUID = UUID()
    @State private var height: CGFloat = 0
    @State private var pageBottom: CGFloat = 0
    let dock: Dock

    private func padding(_ shell: ShellDock) -> CGFloat {
        guard shell.bottom > 0 else { return 0 }
        return shell.bottom + max(0, pageBottom - shell.barBottom)
    }

    func body(content: Content) -> some View {
        if let shell = shell {
            VStack(spacing: 0) {
                content
                dock
                    .onGeometryChange(for: CGFloat.self, of: { $0.size.height }) { (next: CGFloat) in
                        height = next
                        shell.set(key, next)
                    }
            }
            .padding(.bottom, padding(shell))
            .onGeometryChange(for: CGFloat.self, of: { $0.frame(in: .global).maxY }) { (bottom: CGFloat) in
                pageBottom = bottom
            }
            .ignoresSafeArea(.container, edges: .bottom)
            .onAppear { shell.set(key, height) }
            .onDisappear { shell.remove(key) }
        } else {
            VStack(spacing: 0) {
                content
                dock
            }
        }
    }
}

extension View {
    func shellDock<Dock: View>(@ViewBuilder _ dock: () -> Dock) -> some View {
        modifier(ShellDocked(dock: dock()))
    }
}

private struct ShellPageClearance: ViewModifier {
    @Environment(\.shellDock) private var shell

    func body(content: Content) -> some View {
        content
            .safeAreaPadding(.top, shell?.top ?? 0)
            .safeAreaPadding(.bottom, shell?.clearance ?? 0)
    }
}

extension View {
    func shellPageClearance() -> some View {
        modifier(ShellPageClearance())
    }
}
