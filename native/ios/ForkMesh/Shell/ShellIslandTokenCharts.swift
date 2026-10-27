import FMAgents
import FMBase
import FMDesktopRemote
import SwiftUI

struct ShellIslandTokenCharts: View {
    static let gap: CGFloat = 6
    static let inputFloor: Double = 1_000
    static let outputFloor: Double = 50
    static let every: Duration = .seconds(1)

    @Environment(\.fmPalette) private var fm
    @State private var vitals: DesktopVitals?
    @State private var series = AgentTokenSeries()
    let width: CGFloat
    let height: CGFloat

    init(width: CGFloat, height: CGFloat) {
        self.width = width
        self.height = height
        _vitals = State(initialValue: DesktopRemoteFeature.vitals)
    }

    private var chartWidth: CGFloat { max(0, (width - ShellIslandTokenCharts.gap) / 2) }

    var body: some View {
        HStack(spacing: ShellIslandTokenCharts.gap) {
            chart(series.input, floor: ShellIslandTokenCharts.inputFloor, tone: fm.accent)
            chart(series.output, floor: ShellIslandTokenCharts.outputFloor, tone: fm.success)
        }
        .frame(width: width, height: height)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(ShellIslandTokenCharts.summary(series)))
        .accessibilityIdentifier("top-island-token-charts")
        .task {
            tick()
            while !Task.isCancelled {
                try? await Task.sleep(for: ShellIslandTokenCharts.every)
                if Task.isCancelled { return }
                tick()
            }
        }
    }

    private func tick() {
        let desktop = vitals?.watching == true ? vitals?.tokenTotals ?? [] : []
        series.tick(AgentTokenSeries.totals(desktop: desktop, rows: HostsFeature.agents?.rows ?? []))
    }

    private func chart(_ values: [Double], floor: Double, tone: Color) -> some View {
        TelemetryChart(
            values: values, slots: AgentTokenSeries.seconds, floor: floor,
            line: tone, fill: tone.opacity(TelemetrySparks.fillOpacity), strokeWidth: 1)
            .frame(width: chartWidth, height: height)
    }

    static func summary(_ series: AgentTokenSeries) -> String {
        func latest(_ values: [Double]) -> String {
            guard let value = values.last else { return "no reading" }
            return "\(Int(value.rounded())) a second"
        }
        return "Agent tokens in \(latest(series.input)), out \(latest(series.output))"
    }
}
