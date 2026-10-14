import FMApi
import SwiftUI
import TipKit

struct TourPage: Identifiable {
    let id: String
    let symbol: String
    let title: String
    let message: String

    static let pages: [TourPage] = [
        TourPage(
            id: "agents", symbol: "sparkles", title: "Agents first",
            message: "The Agents tab shows what needs you, what is running and what finished. Tap a run to steer it."),
        TourPage(
            id: "inbox", symbol: "tray", title: "One inbox",
            message: "Pings, review requests, assignments, mentions and agent approvals land in Inbox."),
        TourPage(
            id: "code", symbol: "chevron.left.forwardslash.chevron.right", title: "Review and merge",
            message: "Code holds your repositories: browse files, review pull requests and merge from your phone."),
        TourPage(
            id: "prompt", symbol: "plus.circle.fill", title: "One tap to start an agent",
            message: "The round glass button above the menu starts an agent on whatever is on screen. Hold it for the command palette."),
        TourPage(
            id: "hub", symbol: "person.crop.circle", title: "Everything else",
            message: "Your avatar opens the hub: notes, tasks, desktops, hosts, the Market, settings and more."),
    ]
}

struct TourView: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.dismiss) private var dismiss
    @State private var index: Int = 0
    let onFinish: (() -> Void)?

    private var isLast: Bool { index == TourPage.pages.count - 1 }

    var body: some View {
        VStack(spacing: FmSpace.x4) {
            TabView(selection: $index) {
                ForEach(Array(TourPage.pages.enumerated()), id: \.element.id) { (entry: (offset: Int, element: TourPage)) in
                    page(entry.element)
                        .tag(entry.offset)
                }
            }
            .tabViewStyle(.page(indexDisplayMode: .always))
            .indexViewStyle(.page(backgroundDisplayMode: .always))
            HStack(spacing: FmSpace.x3) {
                if onFinish != nil && !isLast {
                    Button("Skip") { finish() }
                        .frame(minWidth: 44, minHeight: 44)
                        .accessibilityIdentifier("tour-skip")
                }
                Spacer(minLength: 0)
                FmFilledButton(isLast ? "Get started" : "Next", onPressed: {
                    FmHaptics.selection()
                    if isLast {
                        finish()
                    } else {
                        withAnimation { index += 1 }
                    }
                })
                .accessibilityIdentifier("tour-next")
            }
            .padding(.horizontal, FmSpace.x4)
            .padding(.bottom, FmSpace.x4)
        }
        .fmPreGlassBackground(fm.canvas)
        .navigationTitle("Tour")
        .navigationBarTitleDisplayMode(.inline)
    }

    private func page(_ page: TourPage) -> some View {
        VStack(spacing: FmSpace.x4) {
            Spacer(minLength: 0)
            Image(systemName: page.symbol)
                .font(.system(size: 64, weight: .regular))
                .foregroundStyle(fm.accent)
                .accessibilityHidden(true)
            Text(page.title)
                .fmText(.titleLarge)
                .multilineTextAlignment(.center)
                .accessibilityAddTraits(.isHeader)
            Text(page.message)
                .fmText(.bodyLarge, color: fm.textSecondary)
                .multilineTextAlignment(.center)
            Spacer(minLength: 0)
        }
        .padding(.horizontal, FmSpace.x6)
        .accessibilityElement(children: .combine)
    }

    private func finish() {
        ShellTour.markSeen()
        if let onFinish = onFinish {
            onFinish()
        } else {
            dismiss()
        }
    }
}

enum ShellTour {
    static let seenKey = "fm.tour.seen.v1"

    static var seen: Bool { UserDefaults.standard.bool(forKey: seenKey) }

    static func markSeen() {
        UserDefaults.standard.set(true, forKey: seenKey)
    }

    static func configureTips() {
        try? Tips.configure([.displayFrequency(.daily)])
    }
}

struct HubTip: Tip {
    var title: Text { Text("Your hub") }
    var message: Text? { Text("Notes, tasks, desktops, hosts, settings and everything else live here.") }
    var image: Image? { Image(systemName: "person.crop.circle") }
}

struct PaletteTip: Tip {
    var title: Text { Text("Hold for commands") }
    var message: Text? { Text("Hold the new agent button to open the command palette.") }
    var image: Image? { Image(systemName: "command") }
}
