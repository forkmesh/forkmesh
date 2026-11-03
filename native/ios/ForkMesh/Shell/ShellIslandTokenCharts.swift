import CoreMotion
import FMAccount
import FMAgents
import FMBase
import FMDesktopRemote
import SwiftUI

struct ShellIslandTokenCharts: View {
    static let gap: CGFloat = 6
    static let inputFloor: Double = 1_000
    static let outputFloor: Double = 50
    static let every: Duration = .seconds(1)
    static let trackOpacity = 0.1
    static let corner: CGFloat = 2
    static let shadeOpacities: [Double] = [0.55, 0.7, 0.85, 1]

    @Environment(\.fmPalette) private var fm
    @State private var vitals: DesktopVitals?
    @State private var series = AgentTokenSeries()
    @State private var tanks: ShellTokenTanks?
    @State private var resting = true
    let width: CGFloat
    let height: CGFloat

    init(width: CGFloat, height: CGFloat) {
        self.width = width
        self.height = height
        _vitals = State(initialValue: DesktopRemoteFeature.vitals)
    }

    private var chartWidth: CGFloat { max(0, (width - ShellIslandTokenCharts.gap) / 2) }

    var body: some View {
        TimelineView(.animation(minimumInterval: AgentTokenTanks.frameSeconds, paused: resting)) {
            (timeline: TimelineViewDefaultContext) in
            Canvas { (context: inout GraphicsContext, _: CGSize) in
                guard let tanks else { return }
                PerformanceMonitorService.section("shell.tokenTanks") {
                    tanks.pair.advance(to: timeline.date)
                    draw(tanks.pair.input, tone: fm.accent, left: 0, in: &context)
                    draw(tanks.pair.output, tone: fm.success, left: chartWidth + ShellIslandTokenCharts.gap, in: &context)
                }
            }
        }
        .frame(width: width, height: height)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(ShellIslandTokenCharts.summary(series)))
        .accessibilityIdentifier("top-island-token-charts")
        .onDisappear { tanks?.stopTilt() }
        .task {
            let tanks = self.tanks ?? ShellTokenTanks(width: Double(chartWidth), height: Double(height))
            self.tanks = tanks
            tick(tanks)
            while !Task.isCancelled {
                try? await Task.sleep(for: ShellIslandTokenCharts.every)
                if Task.isCancelled { return }
                tick(tanks)
            }
        }
    }

    private func tick(_ tanks: ShellTokenTanks) {
        let desktop = vitals?.watching == true ? vitals?.tokenTotals ?? [] : []
        series.tick(AgentTokenSeries.totals(desktop: desktop, rows: HostsFeature.agents?.rows ?? []))
        tanks.pair.input.pour(
            tokens: series.input.last ?? 0,
            scale: AgentTokenTank.scale(series.input, floor: ShellIslandTokenCharts.inputFloor))
        tanks.pair.output.pour(
            tokens: series.output.last ?? 0,
            scale: AgentTokenTank.scale(series.output, floor: ShellIslandTokenCharts.outputFloor))
        tanks.syncTilt()
        let idle = tanks.pair.idle
        if resting != idle { resting = idle }
    }

    private func draw(_ tank: AgentTokenTank, tone: Color, left: CGFloat, in context: inout GraphicsContext) {
        let track = CGRect(x: left, y: 0, width: chartWidth, height: height)
        context.fill(
            Path(roundedRect: track, cornerRadius: ShellIslandTokenCharts.corner),
            with: .color(tone.opacity(ShellIslandTokenCharts.trackOpacity)))
        guard tank.width >= 1, tank.height >= 1 else { return }
        let unit = chartWidth / CGFloat(tank.width)
        let columns = Int(tank.width) - 1
        let rows = Int(tank.height) - 1
        let opacities = ShellIslandTokenCharts.shadeOpacities
        let xs = tank.xs
        let ys = tank.ys
        let shades = tank.shades
        var paths = [Path](repeating: Path(), count: opacities.count)
        for i in 0..<tank.grains {
            let x = min(max(Int(xs[i]), 0), columns)
            let y = min(max(Int(ys[i]), 0), rows)
            let shade = min(max(shades[i] - 1, 0), opacities.count - 1)
            paths[shade].addRect(CGRect(x: left + CGFloat(x) * unit, y: CGFloat(y) * unit, width: unit, height: unit))
        }
        for (shade, path) in paths.enumerated() {
            context.fill(path, with: .color(tone.opacity(opacities[shade])))
        }
    }

    static func summary(_ series: AgentTokenSeries) -> String {
        func latest(_ values: [Double]) -> String {
            guard let value = values.last else { return "no reading" }
            return "\(Int(value.rounded())) a second"
        }
        return "Agent tokens in \(latest(series.input)), out \(latest(series.output))"
    }
}

@MainActor
final class ShellTokenTanks {
    static let tiltInterval = 1.0 / 30.0

    let pair: AgentTokenTanks
    private let motion = CMMotionManager()

    init(width: Double, height: Double) {
        pair = AgentTokenTanks(width: width, height: height)
    }

    func syncTilt() {
        if pair.wantsMotion { startTilt() } else { stopTilt() }
    }

    func startTilt() {
        guard motion.isAccelerometerAvailable, !motion.isAccelerometerActive else { return }
        motion.accelerometerUpdateInterval = ShellTokenTanks.tiltInterval
        motion.startAccelerometerUpdates(to: .main) { [weak self] (data: CMAccelerometerData?, _: Error?) in
            guard let data else { return }
            let pull = data.acceleration
            MainActor.assumeIsolated { self?.pair.tilt(x: pull.x, y: -pull.y) }
        }
    }

    func stopTilt() {
        if motion.isAccelerometerActive { motion.stopAccelerometerUpdates() }
    }
}
