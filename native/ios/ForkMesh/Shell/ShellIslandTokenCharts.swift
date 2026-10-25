import FMBase
import FMDesktopRemote
import SwiftUI

struct ShellIslandTokenCharts: View {
    static let chartWidth: CGFloat = 40
    static let gap: CGFloat = 8
    static let inputFloor: Double = 1_000
    static let outputFloor: Double = 50

    @Environment(\.fmPalette) private var fm
    @State private var vitals: DesktopVitals?
    let height: CGFloat

    init(height: CGFloat) {
        self.height = height
        _vitals = State(initialValue: DesktopRemoteFeature.vitals)
    }

    var body: some View {
        if let vitals, vitals.watching, vitals.tokensKnown {
            HStack(spacing: ShellIslandTokenCharts.gap) {
                chart(vitals.tokensIn, floor: ShellIslandTokenCharts.inputFloor, tone: fm.accent)
                chart(vitals.tokensOut, floor: ShellIslandTokenCharts.outputFloor, tone: fm.success)
            }
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(Text(ShellIslandTokenCharts.summary(vitals)))
            .accessibilityIdentifier("top-island-token-charts")
        }
    }

    private func chart(_ values: [Double], floor: Double, tone: Color) -> some View {
        TelemetryChart(
            values: values, slots: DesktopVitals.seconds, floor: floor,
            line: tone, fill: tone.opacity(TelemetrySparks.fillOpacity), strokeWidth: 1)
            .frame(width: ShellIslandTokenCharts.chartWidth, height: height)
    }

    static func summary(_ vitals: DesktopVitals) -> String {
        func latest(_ values: [Double]) -> String {
            guard let value = values.last(where: { !$0.isNaN }) else { return "no reading" }
            return "\(Int(value.rounded())) a second"
        }
        return "Agent tokens in \(latest(vitals.tokensIn)), out \(latest(vitals.tokensOut))"
    }
}
