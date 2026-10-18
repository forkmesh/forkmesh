import FMCodeInsight
import SwiftUI

extension EmberColor {
    var color: Color { Color(.sRGB, red: r, green: g, blue: b, opacity: a) }

    func opacity(_ alpha: Double) -> Color { Color(.sRGB, red: r, green: g, blue: b, opacity: alpha) }
}

enum EmberStyle {
    static func background(dark: Bool) -> EmberColor {
        dark ? EmberColor(hex: 0x0B0B0C) : EmberColor(hex: 0xF7F7F7)
    }

    static let trendLines = Color(.sRGB, red: 0x8b / 255.0, green: 0x94 / 255.0, blue: 0x9e / 255.0, opacity: 1)
    static let trendLinesLight = Color(.sRGB, red: 0x57 / 255.0, green: 0x60 / 255.0, blue: 0x6a / 255.0, opacity: 1)
    static let trendComplexity = Ember.hotspotMid.color
}

struct EmberChip: View {
    @Environment(\.fmPalette) private var fm
    let label: String
    let active: Bool
    var muted = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Text(label)
                .fmText(.labelMedium, color: active ? fm.accent : (muted ? fm.textTertiary : fm.textSecondary))
                .lineLimit(1)
                .padding(.horizontal, FmSpace.x3)
                .frame(height: 32)
                .background(active ? fm.accentSubtle : fm.bgRaised)
                .clipShape(Capsule())
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(active ? [.isSelected, .isButton] : .isButton)
    }
}

struct EmberBar: View {
    @Environment(\.fmPalette) private var fm
    let fraction: Double
    let color: Color

    var body: some View {
        GeometryReader { proxy in
            ZStack(alignment: .leading) {
                Capsule().fill(fm.bgElevated)
                Capsule().fill(color)
                    .frame(width: max(3, proxy.size.width * CGFloat(max(0, min(1, fraction)))))
            }
        }
        .frame(height: 4)
        .accessibilityHidden(true)
    }
}

struct EmberRowView: View {
    @Environment(\.fmPalette) private var fm
    let row: EmberRow
    var selected = false
    let onTap: () -> Void

    var body: some View {
        Button(action: onTap) {
            HStack(alignment: .top, spacing: FmSpace.x3) {
                if row.rank > 0 {
                    Text("\(row.rank)")
                        .fmText(.labelSmall, color: fm.onPrimary, tabular: true)
                        .frame(width: 22, height: 22)
                        .background(fm.textPrimary)
                        .clipShape(Circle())
                } else {
                    Circle().fill(row.color.color).frame(width: 10, height: 10).padding(.top, 5)
                }
                VStack(alignment: .leading, spacing: FmSpace.x1) {
                    HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                        Text(row.title).fmText(.bodyMedium, weight: FmType.medium).lineLimit(1).truncationMode(.middle)
                        Spacer(minLength: FmSpace.x2)
                        Text(row.value).fmText(.bodySmall, color: fm.textSecondary, tabular: true).lineLimit(1)
                    }
                    if !row.detail.isEmpty {
                        Text(row.detail).fmText(.bodySmall, color: fm.textTertiary).lineLimit(1).truncationMode(.head)
                    }
                    EmberBar(fraction: row.fraction, color: row.color.color)
                    ForEach(Array(row.notes.enumerated()), id: \.offset) { (item: (offset: Int, element: String)) in
                        Text("· " + item.element).fmText(.bodySmall, color: fm.textSecondary).lineLimit(2)
                    }
                }
            }
            .padding(.vertical, FmSpace.x2)
            .padding(.horizontal, FmSpace.x3)
            .background(selected ? fm.accentSubtle : Color.clear)
            .clipShape(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(([row.rank > 0 ? "Rank \(row.rank)" : "", row.title, row.detail, row.value] + row.notes)
            .filter { !$0.isEmpty }.joined(separator: ", "))
        .accessibilityAddTraits(selected ? [.isButton, .isSelected] : .isButton)
    }
}

struct EmberHealthRing: View {
    @Environment(\.fmPalette) private var fm
    let health: Double

    var body: some View {
        ZStack {
            Circle().stroke(fm.textPrimary.opacity(0.1), lineWidth: 8)
            Circle()
                .trim(from: 0, to: CGFloat(Ember.healthShare(health)))
                .stroke(Ember.healthColor(health).color, style: StrokeStyle(lineWidth: 8, lineCap: .round))
                .rotationEffect(.degrees(-90))
            VStack(spacing: 0) {
                Text(Ember.healthText(health)).fmText(.titleLarge, weight: FmType.emphasis, tabular: true)
                Text("of 10").fmText(.labelSmall, color: fm.textTertiary)
            }
        }
        .frame(width: 84, height: 84)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("Code health \(Ember.decimal(health)) of 10")
    }
}

struct EmberRangeSlider: View {
    @Environment(\.fmPalette) private var fm
    @Binding var low: Double
    @Binding var high: Double
    let bounds: ClosedRange<Double>
    let step: Double
    let onChange: () -> Void

    private let thumb: CGFloat = 28

    var body: some View {
        GeometryReader { proxy in
            let usable = max(1, proxy.size.width - thumb)
            ZStack(alignment: .leading) {
                track(usable)
                    .offset(x: thumb / 2)
                knob(value: low, usable: usable, label: "Lowest code health") { value in
                    low = min(value, high)
                }
                knob(value: high, usable: usable, label: "Highest code health") { value in
                    high = max(value, low)
                }
            }
            .frame(height: thumb)
            .coordinateSpace(.named("emberRange"))
        }
        .frame(height: thumb)
    }

    private func position(_ value: Double, _ usable: CGFloat) -> CGFloat {
        CGFloat((value - bounds.lowerBound) / (bounds.upperBound - bounds.lowerBound)) * usable
    }

    private func value(at x: CGFloat, _ usable: CGFloat) -> Double {
        let raw = bounds.lowerBound + Double(max(0, min(usable, x)) / usable) * (bounds.upperBound - bounds.lowerBound)
        return max(bounds.lowerBound, min(bounds.upperBound, (raw / step).rounded() * step))
    }

    private func track(_ usable: CGFloat) -> some View {
        let span = bounds.upperBound - bounds.lowerBound
        let unhealthy = CGFloat((4 - bounds.lowerBound) / span) * usable
        let problematic = CGFloat((8 - bounds.lowerBound) / span) * usable
        return ZStack(alignment: .leading) {
            HStack(spacing: 0) {
                Rectangle().fill(Ember.bandColor(.unhealthy).color).frame(width: max(0, unhealthy))
                Rectangle().fill(Ember.bandColor(.problematic).color).frame(width: max(0, problematic - unhealthy))
                Rectangle().fill(Ember.bandColor(.healthy).color)
            }
            .frame(width: usable, height: 6)
            .opacity(0.35)
            .clipShape(Capsule())
            HStack(spacing: 0) {
                Color.clear.frame(width: position(low, usable))
                Capsule().fill(fm.accent).frame(width: max(0, position(high, usable) - position(low, usable)), height: 6)
            }
        }
        .frame(width: usable, height: thumb)
    }

    private func knob(value current: Double, usable: CGFloat, label: String, set: @escaping (Double) -> Void) -> some View {
        Circle()
            .fill(fm.bgRaised)
            .overlay(Circle().stroke(fm.accent, lineWidth: 2))
            .shadow(color: Color.black.opacity(0.15), radius: 2, y: 1)
            .frame(width: thumb, height: thumb)
            .offset(x: position(current, usable))
            .gesture(DragGesture(minimumDistance: 0, coordinateSpace: .named("emberRange")).onChanged { drag in
                let next = value(at: drag.location.x - thumb / 2, usable)
                if next != current {
                    set(next)
                    onChange()
                }
            })
            .accessibilityElement()
            .accessibilityLabel(label)
            .accessibilityValue(Ember.decimal(current))
            .accessibilityAdjustableAction { direction in
                switch direction {
                case .increment: set(min(bounds.upperBound, current + step))
                case .decrement: set(max(bounds.lowerBound, current - step))
                @unknown default: break
                }
                onChange()
            }
    }
}
