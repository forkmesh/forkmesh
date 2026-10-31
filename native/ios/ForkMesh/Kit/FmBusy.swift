import SwiftUI

enum FmBusy {
    static let turn: Double = 1.6
    static let arc: CGFloat = 0.7
    static let grace: TimeInterval = 1.0
    static let cap: TimeInterval = 60
    static let lineWidth: CGFloat = 1.75
}

struct FmBusyRing: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.shellTabHidden) private var tabHidden

    var corner: CGFloat? = nil
    var color: Color? = nil
    var lineWidth: CGFloat = FmBusy.lineWidth

    private var tint: Color {
        return color ?? fm.accent
    }

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 30.0, paused: reduceMotion || tabHidden)) {
            (context: TimelineViewDefaultContext) in
            let t: Double = context.date.timeIntervalSinceReferenceDate
            let spin: Double = reduceMotion ? 0 : t.truncatingRemainder(dividingBy: FmBusy.turn) / FmBusy.turn
            if let corner = corner {
                outline(corner: corner, start: 360 * spin)
            } else {
                circle(spin: spin)
            }
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    private func circle(spin: Double) -> some View {
        ZStack {
            Circle()
                .stroke(tint.opacity(0.14), lineWidth: lineWidth)
            Circle()
                .trim(from: 0, to: reduceMotion ? 1 : FmBusy.arc)
                .stroke(
                    AngularGradient(
                        colors: [tint.opacity(reduceMotion ? 1 : 0), tint], center: .center,
                        startAngle: .degrees(0), endAngle: .degrees(360 * Double(FmBusy.arc))),
                    style: StrokeStyle(lineWidth: lineWidth, lineCap: .round)
                )
                .rotationEffect(.degrees(360 * spin))
        }
    }

    private func outline(corner: CGFloat, start: Double) -> some View {
        let sweep: Double = 360 * Double(FmBusy.arc)
        return FmRoundedShape(radius: corner)
            .strokeBorder(
                reduceMotion
                    ? AnyShapeStyle(tint)
                    : AnyShapeStyle(
                        AngularGradient(
                            colors: [tint.opacity(0), tint], center: .center,
                            startAngle: .degrees(start), endAngle: .degrees(start + sweep))),
                lineWidth: lineWidth
            )
            .background(
                FmRoundedShape(radius: corner)
                    .strokeBorder(tint.opacity(0.14), lineWidth: lineWidth))
    }
}

struct FmBusyTap: Equatable {
    var tappedAt: Date? = nil
    var busy: Bool = false

    mutating func mark() {
        tappedAt = Date()
    }

    mutating func enabled(_ enabled: Bool) {
        if enabled {
            busy = false
        } else if let tappedAt = tappedAt, Date().timeIntervalSince(tappedAt) <= FmBusy.grace {
            busy = true
        }
        tappedAt = nil
    }
}

private struct FmBusyRingOverlay: ViewModifier {
    let busy: Bool
    let size: CGFloat?
    let corner: CGFloat?
    let color: Color?
    let outset: CGFloat

    func body(content: Content) -> some View {
        content.overlay {
            if busy {
                if let size = size {
                    FmBusyRing(corner: corner, color: color)
                        .frame(width: size, height: size)
                } else {
                    FmBusyRing(corner: corner.map { $0 + outset }, color: color)
                        .padding(-outset)
                }
            }
        }
    }
}

private struct FmBusyTracking: ViewModifier {
    @Binding var tap: FmBusyTap
    let enabled: Bool

    func body(content: Content) -> some View {
        content
            .onChange(of: enabled) { (_: Bool, now: Bool) in
                tap.enabled(now)
            }
            .task(id: tap.busy) {
                guard tap.busy else {
                    return
                }
                try? await Task.sleep(nanoseconds: UInt64(FmBusy.cap * 1_000_000_000))
                if !Task.isCancelled {
                    tap.busy = false
                }
            }
    }
}

extension View {
    func fmBusyRing(
        _ busy: Bool, size: CGFloat? = nil, corner: CGFloat? = nil, color: Color? = nil, outset: CGFloat = 0
    ) -> some View {
        modifier(FmBusyRingOverlay(busy: busy, size: size, corner: corner, color: color, outset: outset))
    }

    func fmBusyTracking(_ tap: Binding<FmBusyTap>, enabled: Bool) -> some View {
        modifier(FmBusyTracking(tap: tap, enabled: enabled))
    }
}
