import FMApi
import FMPackages
import SwiftUI

struct TourObjectCard: ViewModifier {
    @Environment(\.fmPalette) private var fm
    var radius: CGFloat = 22

    func body(content: Content) -> some View {
        content
            .background(fm.bgRaised, in: RoundedRectangle(cornerRadius: radius, style: .continuous))
            .overlay {
                RoundedRectangle(cornerRadius: radius, style: .continuous)
                    .strokeBorder(Color.white.opacity(fm.dark ? 0.06 : 0), lineWidth: 0.5)
            }
            .shadow(color: Color.black.opacity(fm.dark ? 0.45 : 0.1), radius: 22, y: 12)
    }
}

extension View {
    func tourCard(radius: CGFloat = 22) -> some View {
        modifier(TourObjectCard(radius: radius))
    }
}

struct TourAgentLine: View {
    @Environment(\.fmPalette) private var fm

    let initial: String
    let color: Color
    let title: String
    let detail: String

    var body: some View {
        HStack(spacing: 12) {
            Text(initial)
                .font(.system(size: 14, weight: .bold))
                .foregroundStyle(Color.white)
                .frame(width: 34, height: 34)
                .background(color, in: Circle())
            VStack(alignment: .leading, spacing: 2) {
                Text(title)
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(fm.textPrimary)
                    .lineLimit(1)
                Text(detail)
                    .font(.system(size: 13))
                    .foregroundStyle(fm.textSecondary)
                    .lineLimit(1)
                    .contentTransition(.opacity)
            }
            Spacer(minLength: 0)
        }
    }
}

struct TourAgentsScene: View {
    @Environment(\.fmPalette) private var fm

    let phase: Int
    let act: (Int) -> Void

    private var step: Int { phase % 3 }

    var body: some View {
        ZStack {
            TourAgentLine(initial: "D", color: Color(fmARGB: 0xFF6B5A46), title: "docs-refresh", detail: "Finished · 2 commits")
                .padding(16)
                .tourCard()
                .scaleEffect(0.84)
                .offset(y: -86)
                .opacity(0.45)
            TourAgentLine(initial: "R", color: Color(fmARGB: 0xFF2F5D50), title: "fix-inbox-badges", detail: "Running on your desktop · 4:12")
                .padding(16)
                .tourCard()
                .scaleEffect(0.92)
                .offset(y: -44)
                .opacity(0.75)
            VStack(alignment: .leading, spacing: 14) {
                TourAgentLine(
                    initial: "A", color: fm.accent, title: step == 2 ? "Pushed to main" : "Agent wants to push to main",
                    detail: step == 0 ? "forkmesh/forkmesh · 3 commits" : (step == 1 ? "Running checks · 0:12" : "+214 −96 · ready to merge"))
                HStack(spacing: 10) {
                    Image(systemName: "arrow.triangle.pull")
                        .font(.system(size: 14, weight: .medium))
                        .foregroundStyle(fm.textSecondary)
                    Text("Phone header avatar: fill the status ring")
                        .font(.system(size: 13))
                        .foregroundStyle(fm.textSecondary)
                        .lineLimit(1)
                    Spacer(minLength: 0)
                }
                HStack {
                    Spacer(minLength: 0)
                    Button(action: { act(1) }) {
                        Text(step == 0 ? "Allow" : (step == 1 ? "Running" : "Done"))
                            .font(.system(size: 15, weight: .semibold))
                            .foregroundStyle(step == 0 ? fm.onPrimary : Color.white)
                            .padding(.horizontal, 18)
                            .frame(height: 36)
                            .background(step == 0 ? fm.textPrimary : fm.success, in: Capsule())
                            .contentTransition(.opacity)
                    }
                    .buttonStyle(.plain)
                    .tourTarget(step == 0)
                    .sensoryFeedback(.success, trigger: step == 1)
                }
            }
            .padding(18)
            .tourCard(radius: 26)
            .offset(y: 30)
        }
        .frame(width: 320)
        .animation(.smooth(duration: 0.45), value: step)
    }
}

struct TourNewAgentScene: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    let phase: Int
    let act: (Int) -> Void

    private static let prompt: [String] = ["Fix", "the", "failing", "inbox", "tests"]

    private var step: Int { phase % 4 }

    private var typed: String {
        switch step {
        case 1: return TourNewAgentScene.prompt.prefix(3).joined(separator: " ")
        case 2, 3: return TourNewAgentScene.prompt.joined(separator: " ")
        default: return ""
        }
    }

    var body: some View {
        VStack(spacing: 34) {
            Button(action: { act(1) }) {
                TourSparkleButton(stars: step >= 3 ? 6 : 3, size: 156, lit: step == 0)
                    .phaseAnimator([false, true]) { view, on in
                        view.scaleEffect(on && !reduceMotion ? 1.03 : 1)
                    } animation: { _ in
                        .easeInOut(duration: 1.6)
                    }
            }
            .buttonStyle(.plain)
            .accessibilityLabel(Text("New agent"))
            HStack(spacing: 10) {
                Image(systemName: step >= 3 ? "checkmark.circle.fill" : "sparkles")
                    .font(.system(size: 16, weight: .semibold))
                    .foregroundStyle(step >= 3 ? fm.success : fm.accent)
                    .contentTransition(.symbolEffect(.replace))
                Text(typed.isEmpty ? "Describe a change" : typed)
                    .font(.system(size: 16))
                    .foregroundStyle(typed.isEmpty ? fm.textTertiary : fm.textPrimary)
                    .contentTransition(.opacity)
                Spacer(minLength: 0)
                if step >= 3 {
                    Text("Started")
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundStyle(fm.success)
                        .transition(.opacity.combined(with: .scale(scale: 0.8)))
                }
            }
            .padding(.horizontal, 18)
            .frame(width: 300, height: 52)
            .tourCard(radius: 26)
        }
        .animation(.smooth(duration: 0.4), value: step)
    }
}

struct TourInboxScene: View {
    @Environment(\.fmPalette) private var fm

    let phase: Int
    let act: (Int) -> Void

    private static let filters: [(title: String, symbol: String, count: Int)] = [
        ("Approvals", "checkmark.shield", 1), ("Reviews", "arrow.triangle.pull", 2), ("Mentions", "at", 2), ("Pings", "bell", 3),
    ]

    private var step: Int { phase % 4 }
    private var open: Bool { step == 1 || step == 2 }
    private var picked: Int? { step >= 2 ? 1 : nil }

    var body: some View {
        ZStack(alignment: .topLeading) {
            VStack(alignment: .leading, spacing: 16) {
                Button(action: { act(1) }) {
                    VStack(alignment: .leading, spacing: 2) {
                        HStack(alignment: .firstTextBaseline, spacing: 6) {
                            Text("Inbox")
                                .font(.system(size: 28, weight: .semibold))
                                .foregroundStyle(fm.textPrimary)
                            Image(systemName: "chevron.down")
                                .font(.system(size: 14, weight: .semibold))
                                .foregroundStyle(fm.textSecondary)
                        }
                        Text(picked == nil ? "All activity · 14 unread" : "Reviews · 2 unread")
                            .font(.system(size: 14))
                            .foregroundStyle(fm.textSecondary)
                            .contentTransition(.opacity)
                    }
                }
                .buttonStyle(.plain)
                .tourTarget(step == 0)
                VStack(spacing: 0) {
                    row("RT", Color(fmARGB: 0xFF3D8BFF), "Rahel asked for your review", "#1482 · 12m")
                    if picked == nil {
                        row("IB", Color(fmARGB: 0xFF6B5A46), "Ivo mentioned you in #mesh", "1h")
                        row("MR", Color(fmARGB: 0xFF2F5D50), "Mirror fmnext-ams-2 is behind", "Tue")
                    } else {
                        row("KA", Color(fmARGB: 0xFF7B4DFF), "Kaif asked for your review", "#1479 · 3h")
                    }
                }
                .tourCard()
            }
            .frame(width: 310)
            if open {
                VStack(spacing: 0) {
                    menuRow("All activity", symbol: picked == nil ? "checkmark" : "tray.full", count: 14)
                    Divider().padding(.vertical, 2)
                    ForEach(Array(TourInboxScene.filters.enumerated()), id: \.offset) {
                        (entry: (offset: Int, element: (title: String, symbol: String, count: Int))) in
                        menuRow(
                            entry.element.title, symbol: picked == entry.offset ? "checkmark" : entry.element.symbol,
                            count: entry.element.count)
                    }
                }
                .padding(.vertical, 6)
                .frame(width: 230)
                .tourGlass(RoundedRectangle(cornerRadius: 22, style: .continuous), fallback: fm.bgRaised)
                .shadow(color: Color.black.opacity(fm.dark ? 0.4 : 0.12), radius: 20, y: 10)
                .offset(y: 62)
                .transition(.scale(scale: 0.3, anchor: .topLeading).combined(with: .opacity))
            }
        }
        .animation(.spring(duration: 0.45, bounce: 0.15), value: step)
    }

    private func row(_ initials: String, _ color: Color, _ title: String, _ detail: String) -> some View {
        HStack(spacing: 12) {
            Text(initials)
                .font(.system(size: 12, weight: .bold))
                .foregroundStyle(Color.white)
                .frame(width: 32, height: 32)
                .background(color, in: Circle())
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.system(size: 14, weight: .semibold)).foregroundStyle(fm.textPrimary).lineLimit(1)
                Text(detail).font(.system(size: 12)).foregroundStyle(fm.textSecondary)
            }
            Spacer(minLength: 0)
            Circle().fill(fm.accent).frame(width: 7, height: 7)
        }
        .padding(.horizontal, 14)
        .frame(height: 58)
        .transition(.opacity)
    }

    private func menuRow(_ title: String, symbol: String, count: Int) -> some View {
        HStack(spacing: 12) {
            Image(systemName: symbol)
                .font(.system(size: 14, weight: .medium))
                .foregroundStyle(fm.textPrimary)
                .frame(width: 20)
            Text(title).font(.system(size: 15)).foregroundStyle(fm.textPrimary)
            Spacer(minLength: 0)
            Text("\(count)").font(.system(size: 14)).foregroundStyle(fm.textTertiary).monospacedDigit()
        }
        .padding(.horizontal, 16)
        .frame(height: 38)
    }
}

struct TourCodeScene: View {
    @Environment(\.fmPalette) private var fm

    let phase: Int
    let act: (Int) -> Void

    private var step: Int { phase % 3 }
    private static let merged: Color = Color(fmARGB: 0xFF7B4DFF)

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("Fill the status ring")
                    .font(.system(size: 16, weight: .semibold))
                    .foregroundStyle(fm.textPrimary)
                Spacer(minLength: 0)
                Text("#1482")
                    .font(.system(size: 13))
                    .foregroundStyle(fm.textSecondary)
            }
            VStack(alignment: .leading, spacing: 3) {
                line("- ring.trim(from: 0, to: 0.8)", kind: -1)
                line("+ ring.trim(from: 0, to: 1)", kind: 1)
                line("  .stroke(status.color)", kind: 0)
                line("+ .animation(.smooth)", kind: 1)
            }
            Button(action: { act(2) }) {
                Label(step == 2 ? "Merged" : (step == 1 ? "Checks passed" : "Merge"), systemImage: step == 2 ? "arrow.triangle.merge" : "checkmark")
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(step == 0 ? fm.onPrimary : Color.white)
                    .frame(maxWidth: .infinity, minHeight: 44)
                    .background(step == 0 ? fm.textPrimary : (step == 1 ? fm.success : TourCodeScene.merged), in: Capsule())
                    .contentTransition(.opacity)
            }
            .buttonStyle(.plain)
            .tourTarget(step == 0)
            .sensoryFeedback(.success, trigger: step == 2)
        }
        .padding(18)
        .frame(width: 320)
        .tourCard(radius: 26)
        .animation(.smooth(duration: 0.4), value: step)
    }

    private func line(_ text: String, kind: Int) -> some View {
        let add: Color = fm.dark ? Color(fmARGB: 0xFF4FD38A) : Color(fmARGB: 0xFF1D8A4D)
        let del: Color = fm.dark ? Color(fmARGB: 0xFFFF7A7A) : Color(fmARGB: 0xFFC43C3C)
        let ink: Color = kind > 0 ? add : (kind < 0 ? del : fm.textSecondary)
        return Text(text)
            .font(.system(size: 13, design: .monospaced))
            .foregroundStyle(ink)
            .lineLimit(1)
            .padding(.horizontal, 8)
            .padding(.vertical, 2)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(ink.opacity(kind == 0 ? 0 : 0.12), in: RoundedRectangle(cornerRadius: 6, style: .continuous))
    }
}

struct TourMarketScene: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    let phase: Int
    let act: (Int) -> Void

    @State private var added: Set<Int> = []

    private static let plugins: [(title: String, detail: String, symbol: String, color: UInt32)] = [
        ("NEXUS", "Network inspection", "network", 0xFF3D8BFF),
        ("Notes", "Workspace", "note.text", 0xFF2F5D50),
        ("Vault", "Personal", "lock.shield", 0xFF6B5A46),
    ]

    private var center: Int { phase % TourMarketScene.plugins.count }

    var body: some View {
        ZStack {
            ForEach(0..<TourMarketScene.plugins.count, id: \.self) { (at: Int) in
                card(at)
            }
        }
        .frame(maxWidth: .infinity)
        .animation(.spring(duration: 0.55, bounce: 0.18), value: center)
    }

    private func slot(_ at: Int) -> Int {
        let count: Int = TourMarketScene.plugins.count
        let raw: Int = (at - center + count) % count
        return raw == 2 ? -1 : raw
    }

    private func card(_ at: Int) -> some View {
        let plugin: (title: String, detail: String, symbol: String, color: UInt32) = TourMarketScene.plugins[at]
        let place: Int = slot(at)
        let isCenter: Bool = place == 0
        return VStack(spacing: 0) {
            Image(systemName: plugin.symbol)
                .font(.system(size: 40, weight: .regular))
                .foregroundStyle(Color.white)
                .frame(width: 96, height: 96)
                .background(Color(fmARGB: plugin.color), in: RoundedRectangle(cornerRadius: 28, style: .continuous))
                .padding(.top, 24)
            Spacer(minLength: 0)
            Text(plugin.title)
                .font(.system(size: 17, weight: .semibold))
                .foregroundStyle(fm.textPrimary)
            Text(plugin.detail)
                .font(.system(size: 13))
                .foregroundStyle(fm.textSecondary)
                .padding(.top, 2)
            Button(action: {
                FmHaptics.tap()
                added.insert(at)
                act(phase)
            }) {
                Text(added.contains(at) ? "Open" : "Get")
                    .font(.system(size: 14, weight: .semibold))
                    .foregroundStyle(fm.onPrimary)
                    .frame(width: 120, height: 34)
                    .background(fm.textPrimary, in: Capsule())
                    .contentTransition(.opacity)
            }
            .buttonStyle(.plain)
            .padding(.top, 12)
            .padding(.bottom, 20)
            .opacity(isCenter ? 1 : 0)
        }
        .frame(width: 210, height: 290)
        .tourCard(radius: 32)
        .scaleEffect(isCenter ? 1 : 0.82)
        .rotationEffect(.degrees(Double(place) * 6))
        .offset(x: CGFloat(place) * 168, y: isCenter ? 0 : 14)
        .opacity(isCenter ? 1 : 0.5)
        .zIndex(isCenter ? 2 : 1)
        .allowsHitTesting(isCenter)
        .accessibilityHidden(!isCenter)
    }
}
