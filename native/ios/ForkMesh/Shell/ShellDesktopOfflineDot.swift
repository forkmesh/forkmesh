import FMApi
import FMControl
import SwiftUI

struct ShellDesktopOfflineDot: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.accessibilityReduceMotion) var reduceMotion

    @State private var dim: Bool = false

    static let size: CGFloat = 8
    static let period: Double = 1.2

    var body: some View {
        Circle()
            .fill(fm.warning)
            .frame(width: ShellDesktopOfflineDot.size, height: ShellDesktopOfflineDot.size)
            .padding(1.5)
            .background(Circle().fill(fm.bgBase))
            .fixedSize()
            .opacity(dim ? 0.2 : 1)
            .onAppear {
                guard !reduceMotion else { return }
                withAnimation(.easeInOut(duration: ShellDesktopOfflineDot.period).repeatForever(autoreverses: true)) {
                    dim = true
                }
            }
            .accessibilityHidden(true)
    }

    @MainActor static func shows(_ id: String) -> Bool {
        guard PackageGate.reliesOnDesktop(item: id), let hub = HostsFeature.hub else { return false }
        if case .offline = hub.reach { return true }
        return false
    }
}
