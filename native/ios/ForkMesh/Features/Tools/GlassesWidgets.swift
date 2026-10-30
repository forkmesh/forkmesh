import FMApi
import FMTools
import SwiftUI

extension FmPalette {
    func glassesCloseness(_ reading: GlassesHomingReading?) -> Color {
        guard let reading = reading, reading.live else { return textTertiary }
        if reading.closeness >= 0.7 { return danger }
        if reading.closeness >= 0.45 { return warning }
        return accent
    }
}

struct GlassesArrowShape: Shape {
    func path(in rect: CGRect) -> Path {
        var path: Path = Path()
        path.move(to: CGPoint(x: rect.midX, y: rect.minY + rect.height * 0.04))
        path.addLine(to: CGPoint(x: rect.minX + rect.width * 0.86, y: rect.minY + rect.height * 0.92))
        path.addLine(to: CGPoint(x: rect.midX, y: rect.minY + rect.height * 0.72))
        path.addLine(to: CGPoint(x: rect.minX + rect.width * 0.14, y: rect.minY + rect.height * 0.92))
        path.closeSubpath()
        return path
    }
}

struct GlassesArrow: View {
    let angle: Double?
    let color: Color
    let size: CGFloat
    @State private var shown: Double = 0
    @State private var pulsing: Bool = false

    var body: some View {
        Group {
            if angle != nil {
                GlassesArrowShape()
                    .fill(color)
                    .rotationEffect(.degrees(shown))
                    .animation(.easeOut(duration: 0.25), value: shown)
            } else {
                ZStack {
                    Circle().stroke(color, lineWidth: size * 0.06).padding(size * 0.09)
                    Circle().fill(color).frame(width: size * 0.18, height: size * 0.18)
                }
                .opacity(pulsing ? 1 : 0.35)
                .animation(.easeInOut(duration: 0.9).repeatForever(autoreverses: true), value: pulsing)
                .onAppear { pulsing = true }
            }
        }
        .frame(width: size, height: size)
        .onAppear { if let angle = angle { shown = angle } }
        .onChange(of: angle) { (_: Double?, next: Double?) in
            if let next = next { shown = GlassesHoming.unwrap(shown, next) }
        }
    }
}

struct GlassesClosenessBar: View {
    let closeness: Double
    let color: Color
    var height: CGFloat = 8

    var body: some View {
        GeometryReader { (proxy: GeometryProxy) in
            ZStack(alignment: .leading) {
                Capsule().fill(color.opacity(0.18))
                Capsule().fill(color).frame(width: proxy.size.width * CGFloat(min(1, max(0, closeness))))
            }
        }
        .frame(height: height)
        .animation(.easeOut(duration: 0.3), value: closeness)
    }
}

struct GlassesHomingCard: View {
    @Environment(\.fmPalette) private var fm
    let reading: GlassesHomingReading
    let watcher: GlassesWatcher
    var identifier: String = "glasses-alert-homing"

    var body: some View {
        let color: Color = fm.glassesCloseness(reading)
        let arrow: Double? = reading.live ? reading.arrow(heading: watcher.heading) : nil
        FmCard {
            HStack(spacing: FmSpace.x4) {
                ZStack {
                    Circle().fill(color.opacity(0.12))
                    GlassesArrow(angle: arrow, color: color, size: 72)
                }
                .frame(width: 104, height: 104)
                .accessibilityElement()
                .accessibilityLabel(arrow.map { "\(reading.label) likely \(GlassesHoming.clock($0))" } ?? "Direction not known yet")
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(reading.label)
                            .fmText(.titleMedium)
                            .lineLimit(2)
                        Text(reading.summary)
                            .fmText(.bodySmall, color: fm.textSecondary)
                    }
                    HStack(spacing: FmSpace.x2) {
                        GlassesClosenessBar(closeness: reading.live ? reading.closeness : 0, color: color, height: 10)
                        Text(reading.live ? "\(Int((reading.closeness * 100).rounded()))%" : "–")
                            .fmText(.labelMedium, color: color)
                    }
                    Text(reading.hint)
                        .fmText(.bodySmall, color: fm.textTertiary)
                }
                Spacer(minLength: 0)
            }
        }
        .accessibilityIdentifier(identifier)
    }
}

struct NearbyDevicesSection: View {
    @Environment(\.fmPalette) private var fm
    let devices: [NearbyDevice]
    let onPick: (NearbyDevice) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            FmSectionHeader(title: "Devices", icon: "bluetooth", count: devices.count)
            if devices.isEmpty {
                Text("Listening…")
                    .fmText(.bodySmall, color: fm.textTertiary)
            }
            LazyVGrid(
                columns: Array(repeating: GridItem(.flexible(), spacing: 6), count: 3),
                alignment: .leading,
                spacing: 6
            ) {
                ForEach(devices) { (device: NearbyDevice) in
                    tile(device)
                }
            }
        }
        .accessibilityIdentifier("glasses-alert-others")
    }

    private func tile(_ device: NearbyDevice) -> some View {
        let signal: Color = device.rssi >= -60 ? fm.danger : device.rssi >= -75 ? fm.warning : fm.accent
        return Button { onPick(device) } label: {
            FmGlassSurface(cornerRadius: FmRadius.lg) {
                VStack(alignment: .leading, spacing: 1) {
                    HStack(spacing: 4) {
                        FmIcon(device.kind.icon, size: 12, color: signal)
                        Text(device.kind.label)
                            .font(.system(size: 9))
                            .foregroundStyle(fm.textPrimary)
                            .lineLimit(1)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    Text(device.label)
                        .font(.system(size: 9, weight: .semibold))
                        .foregroundStyle(fm.textPrimary)
                        .lineLimit(1)
                        .padding(.top, 2)
                    Text(device.id)
                        .font(.system(size: 8))
                        .foregroundStyle(fm.textTertiary)
                        .lineLimit(1)
                    HStack(spacing: 3) {
                        Text(device.distance)
                            .font(.system(size: 8, weight: .bold))
                            .foregroundStyle(signal)
                            .padding(.horizontal, 5)
                            .padding(.vertical, 1)
                            .background(Capsule().fill(signal.opacity(0.16)))
                        Spacer(minLength: 0)
                        Text("\(device.rssi) dBm")
                            .font(.system(size: 8))
                            .foregroundStyle(signal)
                    }
                    .padding(.top, 3)
                }
                .padding(.horizontal, 7)
                .padding(.vertical, 6)
                .frame(maxWidth: .infinity, minHeight: 72, alignment: .leading)
                .background(signal.opacity(fm.dark ? 0.10 : 0.06))
                .overlay(FmRoundedShape(radius: FmRadius.lg).strokeBorder(signal.opacity(0.42), lineWidth: 1))
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(device.kind.label), \(device.label), \(device.id), \(device.distance), \(device.rssi) dBm")
        .accessibilityHint("Shows how far it is and which way")
        .accessibilityAddTraits(.isButton)
        .accessibilityIdentifier("glasses-alert-device")
    }
}

struct NearbyDeviceSheet: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    let device: NearbyDevice
    let watcher: GlassesWatcher

    private var current: NearbyDevice {
        watcher.others.first { $0.id == device.id } ?? device
    }

    var body: some View {
        let shown: NearbyDevice = current
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    HStack(spacing: FmSpace.x3) {
                        FmIcon(shown.kind.icon, size: 28, color: fm.textPrimary)
                            .frame(width: 52, height: 52)
                            .background(RoundedRectangle(cornerRadius: FmRadius.lg).fill(fm.bgRaised))
                            .overlay(RoundedRectangle(cornerRadius: FmRadius.lg).stroke(fm.border, lineWidth: 1))
                        VStack(alignment: .leading, spacing: 2) {
                            Text(shown.label)
                                .fmText(.titleMedium)
                                .lineLimit(2)
                            Text([shown.kind.label, shown.maker].compactMap { $0 }.joined(separator: " · "))
                                .fmText(.bodySmall, color: fm.textSecondary)
                        }
                        Spacer(minLength: 0)
                    }
                    if let reading = watcher.tracked {
                        GlassesHomingCard(reading: reading, watcher: watcher, identifier: "glasses-alert-device-homing")
                    } else {
                        Text(watcher.watching ? "Listening for it…" : "Start watching to follow it.")
                            .fmText(.bodyMedium, color: fm.textSecondary)
                    }
                    Text("Distance is a guess from signal strength. Walls, bodies and the device's own power all change it.")
                        .fmText(.bodySmall, color: fm.textTertiary)
                }
                .padding(FmSpace.x4)
            }
            .background(fm.canvas)
            .navigationTitle("Find device")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
        }
        .presentationDetents([.medium, .large])
        .onAppear { watcher.track(device) }
        .onDisappear { watcher.untrack() }
        .accessibilityIdentifier("glasses-alert-device-sheet")
    }
}

struct GlassesRadar: View {
    let blips: [GlassesRadarBlip]
    let color: Color
    let dot: Color
    let alert: Color
    var size: CGFloat = 34
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var sweeping: Bool = false

    var body: some View {
        ZStack {
            Circle().fill(color.opacity(0.08))
            Circle().stroke(color.opacity(0.45), lineWidth: 1)
            Circle().stroke(color.opacity(0.25), lineWidth: 0.5).padding(size * 0.25)
            Circle()
                .fill(
                    AngularGradient(
                        gradient: Gradient(stops: [
                            .init(color: color.opacity(0), location: 0),
                            .init(color: color.opacity(0), location: 0.7),
                            .init(color: color.opacity(0.55), location: 1),
                        ]), center: .center, angle: .zero)
                )
                .rotationEffect(.degrees(sweeping ? 360 : 0))
                .animation(reduceMotion ? nil : .linear(duration: 2.5).repeatForever(autoreverses: false), value: sweeping)
            Canvas { (context: inout GraphicsContext, canvas: CGSize) in
                let radius: CGFloat = min(canvas.width, canvas.height) / 2
                for blip in blips {
                    let turn: Double = blip.angle * .pi / 180
                    let reach: CGFloat = radius * CGFloat(blip.distance)
                    let width: CGFloat = blip.glasses ? 6 : 3
                    let center: CGPoint = CGPoint(x: radius + reach * CGFloat(sin(turn)), y: radius - reach * CGFloat(cos(turn)))
                    let spot: Path = Path(ellipseIn: CGRect(x: center.x - width / 2, y: center.y - width / 2, width: width, height: width))
                    context.fill(spot, with: .color(blip.glasses ? alert : dot))
                }
            }
            Circle().fill(color).frame(width: 3, height: 3)
        }
        .frame(width: size, height: size)
        .onAppear { sweeping = !reduceMotion }
        .accessibilityHidden(true)
    }
}

struct GlassesRadarChip: View {
    @Environment(\.fmPalette) private var fm
    private let watcher: GlassesWatcher = GlassesWatcher.shared

    var body: some View {
        if watcher.watching, PackageGateSync.isInstalled("glasses-alert") {
            radar
        }
    }

    private var radar: some View {
        let glasses: Int = watcher.nearby.count
        let count: Int = glasses + watcher.others.count
        let alarmed: Bool = watcher.ringing != nil || glasses > 0
        let blips: [GlassesRadarBlip] = GlassesRadarLayout.blips(
            others: watcher.others, glasses: watcher.nearby, homing: watcher.homing, heading: watcher.heading)
        return Button { SurfaceRouter.open(.glassesAlert) } label: {
            GlassesRadar(blips: blips, color: alarmed ? fm.danger : fm.success, dot: fm.textPrimary.opacity(0.75), alert: fm.danger)
                .padding(5)
                .shellGlass(Circle())
                .contentShape(Circle())
                .overlay(alignment: .topTrailing) {
                    Text("\(count)")
                        .font(.system(size: 10, weight: .bold).monospacedDigit())
                        .foregroundStyle(Color.white)
                        .padding(.horizontal, 4)
                        .frame(minWidth: 16, minHeight: 16)
                        .background(Capsule().fill(alarmed ? fm.danger : fm.accent))
                        .offset(x: 6, y: -4)
                }
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(
            glasses > 0
                ? "Glasses Alert: \(glasses == 1 ? "smart glasses or a camera" : "\(glasses) smart glasses or cameras") nearby, \(count) devices heard"
                : "Glasses Alert scanning, \(count == 1 ? "1 device" : "\(count) devices") heard")
        .accessibilityHint("Opens Glasses Alert")
        .accessibilityAddTraits(.isButton)
        .accessibilityIdentifier("glasses-radar")
    }
}
