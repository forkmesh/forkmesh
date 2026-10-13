import FMApi
import FMBase
import SwiftUI

struct PingCelebrationOverlay: View {
    static let duration: TimeInterval = 6
    static let rain = 140
    static let burst = 60

    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @State private var notifications: NotificationService? = Services.get(NotificationService.self)
    @State private var shown: ForkNotification?
    @State private var start: Date?

    var body: some View {
        ZStack {
            if let start, !reduceMotion {
                confetti(from: start)
            }
            if let shown {
                card(shown)
                    .transition(.scale(scale: 0.4).combined(with: .opacity))
            }
        }
        .allowsHitTesting(false)
        .onChange(of: notifications?.celebrationCount ?? 0) { _, _ in
            fire()
        }
        .task(id: start) {
            guard start != nil else { return }
            do { try await Task.sleep(for: .seconds(Self.duration)) } catch { return }
            withAnimation(.easeOut(duration: 0.4)) { shown = nil }
            start = nil
        }
    }

    private func fire() {
        guard let ping = notifications?.celebration else { return }
        withAnimation(.spring(response: 0.35, dampingFraction: 0.55)) { shown = ping }
        start = Date()
        FmHaptics.success()
    }

    private func card(_ ping: ForkNotification) -> some View {
        VStack(spacing: FmSpace.x1) {
            Text(ping.title)
                .font(.system(size: 26, weight: .heavy, design: .rounded))
                .multilineTextAlignment(.center)
            if !ping.body.isEmpty {
                Text(ping.body)
                    .font(.system(size: 15, weight: .medium))
                    .opacity(0.85)
                    .multilineTextAlignment(.center)
                    .lineLimit(2)
            }
        }
        .foregroundStyle(.white)
        .padding(.horizontal, 26)
        .padding(.vertical, 18)
        .background(RoundedRectangle(cornerRadius: 22, style: .continuous).fill(Color.black.opacity(0.78)))
        .shadow(color: .black.opacity(0.3), radius: 24, y: 12)
        .padding(.horizontal, 24)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .offset(y: -80)
        .accessibilityElement(children: .combine)
    }

    private func confetti(from begin: Date) -> some View {
        let colors = [fm.success, fm.accent, fm.warning, fm.danger, fm.codeType, fm.codeKeyword]
        return TimelineView(.animation) { context in
            Canvas { canvas, size in
                let t = context.date.timeIntervalSince(begin)
                guard t >= 0, t < Self.duration else { return }
                let fade = t < Self.duration - 1 ? 1 : max(0, Self.duration - t)
                for index in 0..<Self.rain {
                    let seed = Double(index)
                    let x0 = abs(sin(seed * 12.9898)) * size.width
                    let fall = 140 + 160 * abs(sin(seed * 78.233))
                    let delay = abs(sin(seed * 3.17)) * 1.6
                    let age = t - delay
                    guard age > 0 else { continue }
                    let x = x0 + CGFloat(sin(age * 2 + seed) * 24)
                    let y = -20 + CGFloat(fall * age)
                    guard y < size.height + 20 else { continue }
                    draw(&canvas, at: CGPoint(x: x, y: y), spin: age * (3 + seed.truncatingRemainder(dividingBy: 4)) + seed,
                         index: index, color: colors[index % colors.count], opacity: fade)
                }
                for index in 0..<Self.burst {
                    let seed = Double(index)
                    let angle = seed / Double(Self.burst) * 2 * .pi + 0.35 * sin(seed * 12.9898)
                    let speed = 220 + 180 * abs(sin(seed * 78.233))
                    let x = size.width / 2 + CGFloat(cos(angle) * speed * t)
                    let y = size.height * 0.4 + CGFloat(sin(angle) * speed * t + 260 * t * t)
                    draw(&canvas, at: CGPoint(x: x, y: y), spin: t * 6 + seed,
                         index: index, color: colors[(index + 2) % colors.count], opacity: fade * max(0, 1 - t / 3))
                }
            }
        }
        .ignoresSafeArea()
        .accessibilityHidden(true)
    }

    private func draw(_ canvas: inout GraphicsContext, at point: CGPoint, spin: Double, index: Int, color: Color, opacity: Double) {
        var piece = canvas
        piece.opacity = opacity
        piece.translateBy(x: point.x, y: point.y)
        piece.rotate(by: .radians(spin))
        let rect = index % 3 == 0
            ? CGRect(x: -3.5, y: -3.5, width: 7, height: 7)
            : CGRect(x: -5, y: -2, width: 10, height: 4)
        let path = index % 3 == 0 ? Circle().path(in: rect) : Path(roundedRect: rect, cornerRadius: 1)
        piece.fill(path, with: .color(color))
    }
}
