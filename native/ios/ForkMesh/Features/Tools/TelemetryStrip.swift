import FMAccount
import FMBase
import FMDesktopRemote
import FMTools
import SwiftUI

/// One series drawn as a line over a faint area, right-aligned into `slots`; NaN points break the
/// line; `scrubIndex` draws a crosshair.
struct TelemetryChart: View {
    let values: [Double]
    let slots: Int
    let floor: Double
    let line: Color
    let fill: Color
    var strokeWidth: CGFloat = 2
    var grid: Color? = nil
    var scrubIndex: Int? = nil
    var surface: Color? = nil

    var body: some View {
        Canvas { (context: inout GraphicsContext, size: CGSize) in
            draw(&context, size)
        }
    }

    private func draw(_ context: inout GraphicsContext, _ size: CGSize) {
        if let grid = grid {
            var rules: Path = Path()
            rules.move(to: CGPoint(x: 0, y: size.height - 0.5))
            rules.addLine(to: CGPoint(x: size.width, y: size.height - 0.5))
            rules.move(to: CGPoint(x: 0, y: 0.5))
            rules.addLine(to: CGPoint(x: size.width, y: 0.5))
            context.stroke(rules, with: .color(grid), lineWidth: 1)
        }
        if values.isEmpty {
            return
        }
        let geometry: TelemetryChartGeometry = TelemetryChartGeometry(
            values: values, slots: slots, floor: floor,
            width: Double(size.width), height: Double(size.height), strokeWidth: Double(strokeWidth))
        for segment in geometry.segments {
            drawSegment(&context, geometry, segment, size)
        }
        if let scrub = scrubIndex, scrub >= 0, scrub < values.count {
            let x: CGFloat = CGFloat(geometry.x(scrub))
            var cross: Path = Path()
            cross.move(to: CGPoint(x: x, y: 0))
            cross.addLine(to: CGPoint(x: x, y: size.height))
            context.stroke(cross, with: .color(grid ?? line), lineWidth: 1)
            let value: Double = values[scrub]
            if !value.isNaN {
                let center: CGPoint = CGPoint(x: x, y: CGFloat(geometry.y(value)))
                if let surface = surface {
                    context.fill(Path(ellipseIn: CGRect(x: center.x - 6, y: center.y - 6, width: 12, height: 12)), with: .color(surface))
                }
                context.fill(Path(ellipseIn: CGRect(x: center.x - 4, y: center.y - 4, width: 8, height: 8)), with: .color(line))
            }
        }
    }

    private func drawSegment(_ context: inout GraphicsContext, _ geometry: TelemetryChartGeometry, _ segment: ClosedRange<Int>, _ size: CGSize) {
        func point(_ i: Int) -> CGPoint {
            return CGPoint(x: CGFloat(geometry.x(i)), y: CGFloat(geometry.y(values[i])))
        }
        let start: Int = segment.lowerBound
        let end: Int = segment.upperBound
        if start == end {
            let p: CGPoint = point(start)
            let r: CGFloat = strokeWidth
            context.fill(Path(ellipseIn: CGRect(x: p.x - r, y: p.y - r, width: r * 2, height: r * 2)), with: .color(line))
            return
        }
        var path: Path = Path()
        path.move(to: point(start))
        for i in (start + 1)...end {
            path.addLine(to: point(i))
        }
        var area: Path = path
        area.addLine(to: CGPoint(x: CGFloat(geometry.x(end)), y: size.height))
        area.addLine(to: CGPoint(x: CGFloat(geometry.x(start)), y: size.height))
        area.closeSubpath()
        context.fill(area, with: .color(fill))
        context.stroke(path, with: .color(line), style: StrokeStyle(lineWidth: strokeWidth, lineCap: .round, lineJoin: .round))
    }
}

/// What the strip opens: the Diagnostics screen on a metric (nil = no focus) or on a log family.
struct TelemetryTarget: Identifiable {
    let id: UUID = UUID()
    let metric: TelemetryMetric?
    let category: LogCategory?
}

struct TelemetryStrip: View {
    static let height: CGFloat = 10
    static let chartWidth: CGFloat = 12
    static let sparkSeconds: Int = 30

    @Environment(\.fmPalette) private var fm
    @State private var telemetry: AppTelemetry?
    @State private var target: TelemetryTarget?
    @State private var settings: SettingsService?
    @State private var vitals: DesktopVitals?

    init() {
        _telemetry = State(initialValue: Services.get(AppTelemetry.self))
        _settings = State(initialValue: Services.get(SettingsService.self))
        _vitals = State(initialValue: DesktopRemoteFeature.vitals)
    }

    var body: some View {
        if let telemetry = telemetry, settings?.showTelemetryStrip ?? true {
            strip(telemetry)
                .sheet(item: $target) { (open: TelemetryTarget) in
                    NavigationStack {
                        TelemetryScreen(focusMetric: open.metric, logCategory: open.category)
                            .toolbar {
                                ToolbarItem(placement: .cancellationAction) {
                                    Button("Done") { target = nil }
                                }
                            }
                    }
                }
        }
    }

    private func strip(_ telemetry: AppTelemetry) -> some View {
        Button {
            target = TelemetryTarget(metric: nil, category: nil)
        } label: {
            TelemetrySparks(telemetry: telemetry, vitals: vitals, chartWidth: TelemetryStrip.chartWidth, height: TelemetryStrip.height)
                .fixedSize()
                .padding(.vertical, 3)
                .frame(maxWidth: .infinity)
                .clipped()
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(TelemetrySparks.summary(telemetry, vitals)))
        .accessibilityHint(Text("Opens Diagnostics"))
        .accessibilityAddTraits(.isButton)
        .accessibilityIdentifier("telemetry-strip")
    }
}

struct TelemetrySparks: View {
    static let spacing: CGFloat = 2
    static let groupGap: CGFloat = 4
    static let fillOpacity: Double = 0.35

    @Environment(\.fmPalette) private var fm
    let telemetry: AppTelemetry
    let vitals: DesktopVitals?
    let chartWidth: CGFloat
    let height: CGFloat
    var spacing: CGFloat = TelemetrySparks.spacing
    var groupGap: CGFloat = TelemetrySparks.groupGap

    static func fitted(width: CGFloat, count: Int, spacing: CGFloat, groupGap: CGFloat, widest: CGFloat) -> CGFloat {
        guard count > 0 else { return widest }
        let room: CGFloat = width - spacing * CGFloat(count - 1) - groupGap
        return max(2, min(widest, room / CGFloat(count)))
    }

    static func chartCount(_ vitals: DesktopVitals?) -> Int {
        TelemetryMetric.allCases.count + (vitals?.watching == true ? DesktopVital.allCases.count : 0)
    }

    var body: some View {
        HStack(spacing: spacing) {
            if let vitals, vitals.watching {
                ForEach(DesktopVital.allCases, id: \.self) { (vital: DesktopVital) in
                    let values: [Double] = vitals.values(vital)
                    let tone: Color = color(vital.tone(values))
                    TelemetryChart(
                        values: values,
                        slots: DesktopVitals.seconds, floor: 100,
                        line: tone, fill: tone.opacity(TelemetrySparks.fillOpacity), strokeWidth: 1)
                        .frame(width: chartWidth, height: height)
                }
                Spacer().frame(width: groupGap)
            }
            ForEach(TelemetryMetric.allCases, id: \.self) { (metric: TelemetryMetric) in
                let values: [Double] = telemetry.history(metric).lastSeconds(TelemetryStrip.sparkSeconds)
                let tone: Color = color(metric.health(values))
                TelemetryChart(
                    values: values,
                    slots: TelemetryStrip.sparkSeconds, floor: metric.floor,
                    line: tone, fill: tone.opacity(TelemetrySparks.fillOpacity), strokeWidth: 1)
                    .frame(width: chartWidth, height: height)
            }
        }
    }

    private func color(_ health: TelemetryHealth?) -> Color {
        switch health {
        case .good: return fm.success
        case .warning: return fm.warning
        case .bad: return fm.danger
        case nil: return fm.textTertiary
        }
    }

    private func color(_ tone: DesktopVitalTone?) -> Color {
        switch tone {
        case .good: return fm.success
        case .warning: return fm.warning
        case .bad: return fm.danger
        case nil: return fm.textTertiary
        }
    }

    static func summary(_ telemetry: AppTelemetry, _ vitals: DesktopVitals?) -> String {
        var desktop: [String] = []
        if let vitals, vitals.watching {
            desktop = DesktopVital.allCases.map { (vital: DesktopVital) -> String in
                "\(vital.title) \(vital.format(vitals.latest(vital)))"
            }
        }
        let metrics: [String] = TelemetryMetric.allCases.map { (metric: TelemetryMetric) -> String in
            "\(metric.title) \(metric.format(telemetry.latest(metric)))"
        }
        return (["Diagnostics"] + desktop + metrics).joined(separator: ", ")
    }
}

struct TelemetryEarSparks: View {
    static let spacing: CGFloat = 1
    static let groupGap: CGFloat = 3

    @State private var telemetry: AppTelemetry?
    let width: CGFloat
    let height: CGFloat

    init(width: CGFloat, height: CGFloat) {
        self.width = width
        self.height = height
        _telemetry = State(initialValue: Services.get(AppTelemetry.self))
    }

    var body: some View {
        if let telemetry = telemetry {
            TelemetrySparks(
                telemetry: telemetry, vitals: nil,
                chartWidth: TelemetrySparks.fitted(
                    width: width, count: TelemetrySparks.chartCount(nil),
                    spacing: TelemetryEarSparks.spacing, groupGap: TelemetryEarSparks.groupGap,
                    widest: TelemetryStrip.chartWidth),
                height: height, spacing: TelemetryEarSparks.spacing, groupGap: TelemetryEarSparks.groupGap)
                .fixedSize()
                .accessibilityElement(children: .ignore)
                .accessibilityLabel(Text(TelemetrySparks.summary(telemetry, nil)))
                .accessibilityIdentifier("top-ear-telemetry")
        }
    }
}
