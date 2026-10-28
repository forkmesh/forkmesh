import FMPackages
import SwiftUI
import UIKit

enum CrmPalette {
    static let closedWords = ["passed", "rejected", "inactive", "closed", "lost", "churned", "declined"]
    static let wonWords = ["customer", "committed", "hired", "active", "won", "signed", "covered", "renewal"]

    static func stage(_ stage: String, in flow: CrmFlow?) -> Color {
        if closedWords.contains(stage) { return .gray }
        if wonWords.contains(stage) { return .green }
        guard let flow, let index = flow.stages.firstIndex(of: stage) else { return .secondary }
        let hues: [Color] = [.blue, .indigo, .purple, .orange, .teal, .mint, .green]
        return hues[min(index, hues.count - 1)]
    }

    static func person(_ name: String) -> Color { FmColors.senderColor(name.isEmpty ? "?" : name) }

    static func status(_ email: CrmActivity) -> Color {
        if email.failed { return .red }
        if email.engaged { return .green }
        if email.delivered { return .blue }
        return .secondary
    }
}

struct CrmAvatar: View {
    let name: String
    let email: String
    var size: CGFloat = 36

    private var initials: String {
        let words = (name.isEmpty ? email : name).split(whereSeparator: { $0 == " " || $0 == "." || $0 == "@" }).prefix(2)
        let letters = words.compactMap(\.first).map { String($0).uppercased() }.joined()
        return letters.isEmpty ? "?" : letters
    }

    var body: some View {
        Circle()
            .fill(CrmPalette.person(name.isEmpty ? email : name).gradient)
            .overlay {
                Text(initials)
                    .font(.system(size: size * 0.38, weight: .semibold, design: .rounded))
                    .foregroundStyle(.white)
            }
            .frame(width: size, height: size)
            .accessibilityHidden(true)
    }
}

struct CrmChip: View {
    let text: String
    var icon: String?
    var tint: Color = .secondary
    var filled = false

    var body: some View {
        HStack(spacing: 3) {
            if let icon { Image(systemName: icon).imageScale(.small) }
            Text(text).lineLimit(1)
        }
        .font(.caption2.weight(.semibold))
        .padding(.horizontal, 7)
        .padding(.vertical, 3)
        .foregroundStyle(filled ? Color.white : tint)
        .background(filled ? AnyShapeStyle(tint) : AnyShapeStyle(tint.opacity(0.14)), in: Capsule())
    }
}

struct CrmStageBadge: View {
    let stage: String
    let flow: CrmFlow?

    var body: some View {
        CrmChip(text: CrmStageLabel.title(stage), tint: CrmPalette.stage(stage, in: flow))
    }
}

struct CrmStageDots: View {
    let flow: CrmFlow
    let stage: String

    var body: some View {
        let current = flow.stages.firstIndex(of: stage) ?? -1
        HStack(spacing: 3) {
            ForEach(Array(flow.stages.enumerated()), id: \.offset) { index, item in
                Capsule()
                    .fill(index <= current ? CrmPalette.stage(item, in: flow) : Color.secondary.opacity(0.2))
                    .frame(height: 4)
            }
        }
        .accessibilityLabel("Stage \(current + 1) of \(flow.stages.count)")
    }
}

struct CrmStatTile: View {
    let value: String
    let label: String
    var icon: String
    var tint: Color = .accentColor

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Image(systemName: icon).font(.caption).foregroundStyle(tint)
            Text(value).font(.headline.monospacedDigit()).lineLimit(1).minimumScaleFactor(0.7)
            Text(label).font(.caption2).foregroundStyle(.secondary).lineLimit(1)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .background(tint.opacity(0.08), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
        .accessibilityElement(children: .combine)
    }
}

struct CrmPillButton: View {
    let title: String
    let icon: String
    var tint: Color = .accentColor
    let action: () -> Void

    var body: some View {
        Button {
            FmHaptics.tap()
            action()
        } label: {
            VStack(spacing: 4) {
                Image(systemName: icon).font(.body.weight(.semibold))
                Text(title).font(.caption2.weight(.medium)).lineLimit(1)
            }
            .frame(maxWidth: .infinity, minHeight: CrmLayout.dense ? 44 : 54)
            .foregroundStyle(tint)
            .background(tint.opacity(0.1), in: RoundedRectangle(cornerRadius: 12, style: .continuous))
        }
        .buttonStyle(.plain)
    }
}

struct CrmMiniBars: View {
    let values: [Int]
    var tint: Color = .accentColor
    var height: CGFloat = 22

    var body: some View {
        let top = max(1, values.max() ?? 1)
        HStack(alignment: .bottom, spacing: 1) {
            ForEach(Array(values.enumerated()), id: \.offset) { _, value in
                RoundedRectangle(cornerRadius: 1)
                    .fill(value > 0 ? tint : tint.opacity(0.15))
                    .frame(height: max(2, height * CGFloat(value) / CGFloat(top)))
            }
        }
        .frame(height: height, alignment: .bottom)
        .accessibilityHidden(true)
    }
}

enum CrmClipboard {
    static func copy(_ text: String) {
        UIPasteboard.general.string = text
        FmHaptics.commit()
    }
}

struct CrmEmptyRow: View {
    let text: String
    var icon = "tray"

    var body: some View {
        Label(text, systemImage: icon)
            .font(.subheadline)
            .foregroundStyle(.secondary)
            .frame(maxWidth: .infinity, alignment: .center)
            .padding(.vertical, 18)
    }
}

extension View {
    func crmCard() -> some View {
        padding(10)
            .background(Color(uiColor: .secondarySystemGroupedBackground), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
    }
}
