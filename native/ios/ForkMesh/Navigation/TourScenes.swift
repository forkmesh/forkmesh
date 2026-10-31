import FMApi
import FMPackages
import SwiftUI

struct TourHomeScene: View {
    let phase: Int
    let act: (Int) -> Void

    private static let icons: [(id: String, title: String, symbol: String, count: Int?, active: Bool)] = [
        ("agents", "Agents", "sparkles", 3, true), ("inbox", "Inbox", "tray", 12, false),
        ("repos", "Repos", "books.vertical", 24, false), ("code", "Code", "chevron.left.forwardslash.chevron.right", nil, false),
        ("chat", "Chat", "bubble.left.and.bubble.right", 4, false), ("notes", "Notes", "note.text", 9, false),
        ("tasks", "Tasks", "checklist", 5, false), ("market", "Market", "bag", nil, false),
    ]

    private static let columns: [GridItem] = Array(repeating: GridItem(.flexible(), spacing: 4), count: 4)

    private var focus: Int { phase % TourHomeScene.icons.count }

    var body: some View {
        VStack(spacing: 0) {
            TourTopBar()
            TourSectionLabel(text: "Go to")
            LazyVGrid(columns: TourHomeScene.columns, spacing: 10) {
                ForEach(0..<TourHomeScene.icons.count, id: \.self) { (at: Int) in
                    icon(at)
                }
            }
            .padding(.horizontal, 10)
            .padding(.top, 6)
            Spacer(minLength: 0)
            TourBottomBar()
        }
    }

    private func item(_ at: Int) -> ShellBarItem {
        let icon: (id: String, title: String, symbol: String, count: Int?, active: Bool) = TourHomeScene.icons[at]
        let lit: Bool = at == focus
        return ShellBarItem(
            id: icon.id, title: icon.title, symbol: icon.symbol, badge: 0, selected: lit, kind: .action,
            pulse: lit ? phase : 0, active: icon.active, count: icon.count, action: {})
    }

    private func icon(_ at: Int) -> some View {
        let heat: Double = at == focus ? 0.8 : 0
        return Button(action: { act(at) }) {
            ShellHomeIcon(item: item(at), style: .outline, iconSize: 20, labelSize: 11, heat: heat)
        }
        .buttonStyle(.plain)
        .accessibilityLabel(Text(TourHomeScene.icons[at].title))
    }
}

struct TourAgentsScene: View {
    @Environment(\.fmPalette) private var fm

    let phase: Int
    let act: (Int) -> Void

    private var step: Int { phase % 3 }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            TourTopBar(stars: step == 0 ? 1 : 2)
            TourTitle(text: "Agents", trailing: step == 0 ? "1 needs you" : nil)
            if step == 0 {
                TourSectionLabel(text: "Needs you")
                TourRow(symbol: "hand.raised.fill", tint: fm.warning, title: "Agent 754 · Fix login test", detail: "Wants to run npm test", unread: true) {
                    Button(action: { act(1) }) {
                        TourPill(text: "Allow", lit: true)
                    }
                    .buttonStyle(.plain)
                }
                .transition(.move(edge: .top).combined(with: .opacity))
            }
            TourSectionLabel(text: "Running")
            if step >= 1 {
                TourRow(symbol: "sparkles", title: "Agent 754 · Fix login test", detail: step == 1 ? "Running npm test · 0:12" : "Writing the fix · 1:04") {
                    ProgressView()
                        .controlSize(.small)
                }
                .transition(.move(edge: .top).combined(with: .opacity))
            }
            TourRow(symbol: "sparkles", title: "Agent 751 · Dark mode", detail: "Editing 4 files · 6:30") {
                ProgressView()
                    .controlSize(.small)
            }
            TourSectionLabel(text: "Finished")
            TourRow(symbol: "checkmark.circle.fill", tint: fm.success, title: "Agent 748 · Update docs", detail: "+42 −7 · ready to merge") {
                Image(systemName: "chevron.right")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(fm.textTertiary)
            }
            Spacer(minLength: 0)
        }
    }
}

struct TourNewAgentScene: View {
    @Environment(\.fmPalette) private var fm

    let phase: Int
    let act: (Int) -> Void

    private var step: Int { phase % 4 }

    var body: some View {
        ZStack(alignment: .bottom) {
            VStack(alignment: .leading, spacing: 6) {
                TourTopBar(stars: step >= 2 ? 3 : 2, buttonLit: step == 0, onButton: { act(1) })
                TourTitle(text: "forkmesh/app")
                TourRow(symbol: "exclamationmark.circle", tint: fm.success, title: "#91 Login fails offline", detail: "Opened 2 hours ago") {
                    EmptyView()
                }
                TourRow(symbol: "arrow.triangle.pull", title: "#128 Fix flaky login test", detail: "2 checks running") {
                    EmptyView()
                }
                Spacer(minLength: 0)
            }
            .blur(radius: step == 1 ? 1.5 : 0)
            if step == 1 {
                composer
                    .transition(.move(edge: .bottom).combined(with: .opacity))
            }
            if step == 2 {
                Label("Agent started", systemImage: "sparkles")
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundStyle(fm.accent)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 8)
                    .tourGlass(Capsule(), fallback: fm.bgRaised)
                    .padding(.bottom, 20)
                    .transition(.scale(scale: 0.3, anchor: .topTrailing).combined(with: .opacity))
            }
        }
    }

    private var composer: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 6) {
                TourPill(text: "forkmesh/app", filled: false)
                TourPill(text: "Edit", filled: false)
                TourPill(text: "Opus", filled: false)
                Spacer(minLength: 0)
            }
            Text("Fix the flaky login test and open a pull request")
                .font(.system(size: 15))
                .foregroundStyle(fm.textPrimary)
                .frame(maxWidth: .infinity, alignment: .leading)
            HStack {
                Image(systemName: "paperclip")
                Image(systemName: "mic")
                Spacer(minLength: 0)
                Button(action: { act(2) }) {
                    Image(systemName: "arrow.up")
                        .font(.system(size: 15, weight: .bold))
                        .foregroundStyle(Color.white)
                        .frame(width: 32, height: 32)
                        .background(fm.accent, in: Circle())
                        .tourTarget(true, round: true)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(Text("Send"))
            }
            .foregroundStyle(fm.textSecondary)
        }
        .padding(14)
        .tourGlass(RoundedRectangle(cornerRadius: 22, style: .continuous), fallback: fm.bgElevated)
        .shadow(color: Color.black.opacity(0.15), radius: 12, y: 4)
        .padding(10)
    }
}

struct TourInboxScene: View {
    @Environment(\.fmPalette) private var fm

    let phase: Int
    let act: (Int) -> Void

    private static let items: [(symbol: String, title: String, detail: String, filter: Int)] = [
        ("hand.raised.fill", "Agent 754 needs approval", "Wants to run npm test", 5),
        ("arrow.triangle.pull", "Review requested: #128", "Fix flaky login test", 2),
        ("at", "You were mentioned in #mesh", "\"can you take a look?\"", 4),
        ("exclamationmark.circle", "Assigned: #91", "Login fails offline", 3),
    ]
    private static let filters: [String] = ["All", "Pings", "Reviews", "Assigned", "Mentions", "Approvals"]

    private var step: Int { phase % 6 }
    private var shown: Int { min(TourInboxScene.items.count, step + 1) }
    private var read: Int { step >= 5 ? 1 : 0 }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            TourTopBar()
            TourTitle(text: "Inbox", trailing: "\(max(0, shown - read)) unread")
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 6) {
                    ForEach(0..<TourInboxScene.filters.count, id: \.self) { (at: Int) in
                        TourPill(text: TourInboxScene.filters[at], filled: at == 0)
                    }
                }
                .padding(.horizontal, 12)
            }
            .allowsHitTesting(false)
            ForEach(0..<shown, id: \.self) { (at: Int) in
                row(at)
            }
            Spacer(minLength: 0)
        }
    }

    private func row(_ at: Int) -> some View {
        let item: (symbol: String, title: String, detail: String, filter: Int) = TourInboxScene.items[at]
        let fresh: Bool = at >= read
        let dot: Color = fresh ? fm.accent : Color.clear
        return Button(action: { act(5) }) {
            TourRow(symbol: item.symbol, title: item.title, detail: item.detail, unread: fresh) {
                Circle()
                    .fill(dot)
                    .frame(width: 8, height: 8)
            }
        }
        .buttonStyle(.plain)
        .transition(.move(edge: .top).combined(with: .opacity))
    }
}

struct TourHubScene: View {
    @Environment(\.fmPalette) private var fm

    let phase: Int
    let act: (Int) -> Void

    private var open: Bool { phase % 2 == 1 }

    var body: some View {
        ZStack(alignment: .topTrailing) {
            VStack(spacing: 0) {
                TourTopBar(avatarLit: !open, onAvatar: { act(open ? 0 : 1) })
                TourTitle(text: "Agents")
                    .padding(.top, 6)
                Spacer(minLength: 0)
                TourBottomBar()
            }
            if open {
                Color.black.opacity(0.18)
                    .onTapGesture { act(0) }
                    .transition(.opacity)
                card
                    .padding(.top, 54)
                    .padding(.trailing, 10)
                    .transition(.scale(scale: 0.2, anchor: .topTrailing).combined(with: .opacity))
            }
        }
    }

    private var card: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                AvatarWithDot(state: .connected, label: "You", statusEmoji: "🌴", size: 34, rounded: true)
                VStack(alignment: .leading, spacing: 0) {
                    Text("You")
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundStyle(fm.textPrimary)
                    Text("Away until Monday")
                        .font(.system(size: 11))
                        .foregroundStyle(fm.textSecondary)
                }
                Spacer(minLength: 0)
                TourPill(text: "Set a status", filled: false)
            }
            toggleRow("Pocket mode", symbol: "iphone.gen3", on: false)
            toggleRow("Suggestion mode", symbol: "lightbulb", on: true)
            HStack(spacing: 6) {
                smallButton("Settings", symbol: "gearshape")
                smallButton("Sign out", symbol: "rectangle.portrait.and.arrow.right")
            }
            Text("Version 1.15 · What's new")
                .font(.system(size: 11))
                .foregroundStyle(fm.textTertiary)
        }
        .padding(12)
        .frame(width: 230)
        .background(fm.bgElevated, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
        .shadow(color: Color.black.opacity(0.2), radius: 14, y: 6)
    }

    private func toggleRow(_ title: String, symbol: String, on: Bool) -> some View {
        HStack(spacing: 8) {
            Image(systemName: symbol)
                .frame(width: 18)
                .foregroundStyle(fm.textSecondary)
            Text(title)
                .font(.system(size: 13))
                .foregroundStyle(fm.textPrimary)
            Spacer(minLength: 0)
            Capsule()
                .fill(on ? fm.accent : fm.border)
                .frame(width: 34, height: 20)
                .overlay(alignment: on ? .trailing : .leading) {
                    Circle().fill(Color.white).padding(2)
                }
        }
    }

    private func smallButton(_ title: String, symbol: String) -> some View {
        Label(title, systemImage: symbol)
            .font(.system(size: 12, weight: .medium))
            .foregroundStyle(fm.textPrimary)
            .frame(maxWidth: .infinity, minHeight: 30)
            .background(fm.bgRaised, in: RoundedRectangle(cornerRadius: 9, style: .continuous))
    }
}

struct TourVoiceScene: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    let phase: Int
    let act: (Int) -> Void

    @State private var holding: Bool = false
    @State private var heard: Int = 0

    private static let words: [String] = ["Add", "a", "dark", "mode", "to", "settings"]

    private var step: Int { phase % 5 }
    private var listening: Bool { holding || step == 1 || step == 2 }
    private var sent: Bool { !holding && step == 3 }
    private var said: Int {
        if holding { return heard }
        switch step {
        case 1: return 3
        case 2, 3: return TourVoiceScene.words.count
        default: return 0
        }
    }

    var body: some View {
        ZStack {
            VStack(spacing: 0) {
                TourTopBar(stars: sent ? 3 : 2)
                Spacer(minLength: 0)
            }
            .blur(radius: listening || sent ? 6 : 0)
            if listening || sent {
                Color.black.opacity(0.25)
                    .transition(.opacity)
            }
            VStack(spacing: 14) {
                Spacer(minLength: 0)
                orb
                Text(TourVoiceScene.words.prefix(said).joined(separator: " "))
                    .font(.system(size: 17, weight: .medium))
                    .foregroundStyle(listening || sent ? Color.white : fm.textPrimary)
                    .frame(height: 24)
                    .contentTransition(.opacity)
                if sent {
                    Label("Agent started", systemImage: "sparkles")
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundStyle(Color.white)
                        .transition(.scale.combined(with: .opacity))
                } else if listening {
                    HStack(spacing: 8) {
                        TourPill(text: "Cancel", filled: false, tint: Color.white)
                        TourPill(text: "Type", filled: false, tint: Color.white)
                        TourPill(text: "Commands", filled: false, tint: Color.white)
                    }
                    .transition(.opacity)
                } else {
                    Text("Touch and hold")
                        .font(.system(size: 13))
                        .foregroundStyle(fm.textSecondary)
                }
                Spacer(minLength: 0)
            }
        }
        .task(id: holding) { await listen() }
    }

    private var orb: some View {
        let size: CGFloat = listening ? 96 : 56
        return ZStack {
            if listening && !reduceMotion {
                ForEach(0..<3, id: \.self) { (ring: Int) in
                    Circle()
                        .stroke(fm.accent.opacity(0.5), lineWidth: 2)
                        .frame(width: size, height: size)
                        .phaseAnimator([false, true]) { view, on in
                            view.scaleEffect(on ? 1.7 : 1).opacity(on ? 0 : 0.8)
                        } animation: { _ in
                            .easeOut(duration: 1.4).delay(Double(ring) * 0.45)
                        }
                }
            }
            TourSparkleButton(stars: sent ? 9 : 4, size: size, lit: step == 0 && !holding)
        }
        .frame(width: 150, height: 150)
        .contentShape(Circle())
        .onLongPressGesture(minimumDuration: 0.35, maximumDistance: 40, perform: {}, onPressingChanged: { (down: Bool) in
            pressed(down)
        })
        .accessibilityElement()
        .accessibilityAddTraits(.isButton)
        .accessibilityLabel(Text("New agent, hold to talk"))
        .accessibilityAction { act(2) }
    }

    private func pressed(_ down: Bool) {
        if down {
            FmHaptics.palette()
            heard = 0
            act(1)
            holding = true
        } else if holding {
            holding = false
            act(3)
            FmHaptics.success()
        }
    }

    private func listen() async {
        guard holding else { return }
        while !Task.isCancelled && heard < TourVoiceScene.words.count {
            try? await Task.sleep(for: .milliseconds(380))
            guard !Task.isCancelled else { return }
            withAnimation(reduceMotion ? nil : .easeOut(duration: 0.2)) {
                heard += 1
            }
        }
    }
}

struct TourCodeScene: View {
    @Environment(\.fmPalette) private var fm

    let phase: Int
    let act: (Int) -> Void

    private var step: Int { phase % 3 }
    private var merged: Bool { step == 2 }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            TourTopBar()
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 6) {
                    TourPill(text: merged ? "Merged" : "Open", tint: merged ? Color.purple : fm.success)
                    Text("#128")
                        .font(.system(size: 13, weight: .medium))
                        .foregroundStyle(fm.textSecondary)
                }
                Text("Fix flaky login test")
                    .font(.system(size: 19, weight: .bold))
                    .foregroundStyle(fm.textPrimary)
                HStack(spacing: 10) {
                    Label("12 checks passed", systemImage: "checkmark.circle.fill")
                        .foregroundStyle(fm.success)
                    if step >= 1 {
                        Label("Approved", systemImage: "person.crop.circle.badge.checkmark")
                            .foregroundStyle(fm.accent)
                            .transition(.scale.combined(with: .opacity))
                    }
                }
                .font(.system(size: 12, weight: .medium))
            }
            .padding(.horizontal, 16)
            VStack(alignment: .leading, spacing: 0) {
                Text("LoginTests.swift")
                    .font(.system(size: 11, weight: .semibold, design: .monospaced))
                    .foregroundStyle(fm.textSecondary)
                    .padding(.horizontal, 10)
                    .padding(.vertical, 6)
                diffLine(" ", "func testSignIn() async {", nil)
                diffLine("-", "  try await Task.sleep(for: .seconds(1))", fm.danger)
                diffLine("+", "  await session.waitUntilReady()", fm.success)
                diffLine(" ", "  XCTAssertTrue(session.signedIn)", nil)
            }
            .background(fm.bgRaised, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            .padding(.horizontal, 12)
            Spacer(minLength: 0)
            Button(action: { act(merged ? 0 : 2) }) {
                Label(merged ? "Merged" : "Merge pull request", systemImage: merged ? "checkmark" : "arrow.triangle.merge")
                    .font(.system(size: 15, weight: .semibold))
                    .foregroundStyle(Color.white)
                    .frame(maxWidth: .infinity, minHeight: 40)
                    .background(merged ? Color.purple : fm.success, in: Capsule())
                    .tourTarget(!merged)
            }
            .buttonStyle(.plain)
            .padding(.horizontal, 16)
            .padding(.bottom, 14)
        }
    }

    private func diffLine(_ mark: String, _ code: String, _ tint: Color?) -> some View {
        HStack(spacing: 6) {
            Text(mark)
                .foregroundStyle(tint ?? fm.textTertiary)
            Text(code)
                .foregroundStyle(fm.textPrimary)
                .lineLimit(1)
            Spacer(minLength: 0)
        }
        .font(.system(size: 11, design: .monospaced))
        .padding(.horizontal, 10)
        .padding(.vertical, 3)
        .background((tint ?? Color.clear).opacity(0.14))
    }
}

struct TourPaletteScene: View {
    @Environment(\.fmPalette) private var fm

    let phase: Int
    let act: (Int) -> Void

    private static let query: String = "new is"
    private static let commands: [(title: String, symbol: String)] = [
        ("New issue", "exclamationmark.circle"), ("New agent", "plus.circle"), ("Switch project", "square.stack.3d.up"),
        ("Open Inbox", "tray"), ("Search everywhere", "magnifyingglass"),
    ]

    private var typed: String {
        let count: Int = min(TourPaletteScene.query.count, (phase % 9) * 2)
        return String(TourPaletteScene.query.prefix(count))
    }

    private var results: [(title: String, symbol: String)] {
        let words: [Substring] = typed.lowercased().split(separator: " ")
        guard !words.isEmpty else { return TourPaletteScene.commands }
        return TourPaletteScene.commands.filter { (command: (title: String, symbol: String)) in
            let title: String = command.title.lowercased()
            return words.allSatisfy { title.contains($0) }
        }
    }

    var body: some View {
        ZStack(alignment: .bottom) {
            VStack(spacing: 0) {
                TourTopBar()
                Spacer(minLength: 0)
            }
            Color.black.opacity(0.15)
            VStack(alignment: .leading, spacing: 6) {
                Capsule()
                    .fill(fm.border)
                    .frame(width: 36, height: 5)
                    .frame(maxWidth: .infinity)
                HStack(spacing: 8) {
                    Image(systemName: "command")
                        .foregroundStyle(fm.textSecondary)
                    Text(typed.isEmpty ? "Type a command or a place" : typed)
                        .foregroundStyle(typed.isEmpty ? fm.textTertiary : fm.textPrimary)
                    Rectangle()
                        .fill(fm.accent)
                        .frame(width: 2, height: 18)
                    Spacer(minLength: 0)
                }
                .font(.system(size: 15))
                .padding(10)
                .background(fm.bgRaised, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
                TourSectionLabel(text: typed.isEmpty ? "Suggested" : "Results")
                    .padding(.horizontal, -16)
                ForEach(Array(results.enumerated()), id: \.element.title) { (entry: (offset: Int, element: (title: String, symbol: String))) in
                    result(entry.element.title, symbol: entry.element.symbol, first: entry.offset == 0)
                }
            }
            .padding(12)
            .frame(height: 230, alignment: .top)
            .background(fm.bgElevated, in: UnevenRoundedRectangle(topLeadingRadius: 22, topTrailingRadius: 22))
        }
    }
}

extension TourPaletteScene {
    fileprivate func result(_ title: String, symbol: String, first: Bool) -> some View {
        let fill: Color = first ? fm.accentSubtle : Color.clear
        let weight: Font.Weight = first ? .semibold : .regular
        return Button(action: { act(0) }) {
            HStack(spacing: 10) {
                Image(systemName: symbol)
                    .frame(width: 20)
                    .foregroundStyle(fm.accent)
                Text(title)
                    .font(.system(size: 14, weight: weight))
                    .foregroundStyle(fm.textPrimary)
                Spacer(minLength: 0)
                if first {
                    Image(systemName: "return")
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundStyle(fm.textTertiary)
                }
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 7)
            .background(fill, in: RoundedRectangle(cornerRadius: 10, style: .continuous))
        }
        .buttonStyle(.plain)
        .transition(.opacity)
    }
}

struct TourSearchScene: View {
    @Environment(\.fmPalette) private var fm

    let phase: Int
    let act: (Int) -> Void

    private static let scopes: [String] = ["All", "Code", "Issues & PRs", "Chat", "People"]
    private static let results: [[(bucket: String, symbol: String, title: String, detail: String)]] = [
        [("Repositories", "books.vertical", "forkmesh/app", "Swift · updated today"),
         ("Issues", "exclamationmark.circle", "#91 Login fails offline", "forkmesh/app"),
         ("Chat", "bubble.left", "\"login is fixed on main\"", "#mesh")],
        [("Code", "doc.text", "LoginView.swift", "func signIn(with token:)"),
         ("Code", "doc.text", "LoginTests.swift", "func testSignIn()")],
        [("Pull requests", "arrow.triangle.pull", "#128 Fix flaky login test", "Open · 12 checks passed"),
         ("Issues", "exclamationmark.circle", "#91 Login fails offline", "Assigned to you")],
        [("Chat", "bubble.left", "\"login is fixed on main\"", "#mesh · 2 hours ago"),
         ("Chat", "bubble.left", "\"who owns login?\"", "Office room")],
        [("People", "person.crop.circle", "Alex", "Works on login"),
         ("People", "person.crop.circle", "Sam", "Reviewed #128")],
    ]

    private var scope: Int { phase % TourSearchScene.scopes.count }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Image(systemName: "magnifyingglass")
                    .foregroundStyle(fm.textSecondary)
                Text("login")
                    .foregroundStyle(fm.textPrimary)
                Spacer(minLength: 0)
                Image(systemName: "xmark.circle.fill")
                    .foregroundStyle(fm.textTertiary)
            }
            .font(.system(size: 15))
            .padding(10)
            .background(fm.bgRaised, in: Capsule())
            .padding(.horizontal, 12)
            .padding(.top, 14)
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 6) {
                    ForEach(0..<TourSearchScene.scopes.count, id: \.self) { (at: Int) in
                        Button(action: { act(at) }) {
                            TourPill(text: TourSearchScene.scopes[at], filled: at == scope)
                        }
                        .buttonStyle(.plain)
                    }
                }
                .padding(.horizontal, 12)
            }
            VStack(alignment: .leading, spacing: 4) {
                ForEach(0..<TourSearchScene.results[scope].count, id: \.self) { (at: Int) in
                    hit(at)
                }
            }
            .id(scope)
            .transition(.opacity)
            Spacer(minLength: 0)
        }
    }
}

extension TourSearchScene {
    @ViewBuilder fileprivate func hit(_ at: Int) -> some View {
        let list: [(bucket: String, symbol: String, title: String, detail: String)] = TourSearchScene.results[scope]
        let item: (bucket: String, symbol: String, title: String, detail: String) = list[at]
        let heads: Bool = at == 0 || list[at - 1].bucket != item.bucket
        if heads {
            TourSectionLabel(text: item.bucket)
        }
        TourRow(symbol: item.symbol, title: item.title, detail: item.detail) {
            EmptyView()
        }
    }
}

struct TourBarScene: View {
    @Environment(\.fmPalette) private var fm

    let phase: Int
    let act: (Int) -> Void

    private var shape: ShellBarShape { ShellBarShape.allCases[phase % ShellBarShape.allCases.count] }

    private var style: ShellBarStyle {
        var out: ShellBarStyle = ShellBarStyle()
        out.shape = shape
        return out
    }

    var body: some View {
        VStack(spacing: 10) {
            TourTopBar()
            Spacer(minLength: 0)
            VStack(alignment: .leading, spacing: 0) {
                menuRow("Move", symbol: "arrow.left.and.right")
                menuRow("Move to folder", symbol: "folder")
                menuRow("Hide", symbol: "eye.slash")
                menuRow("Bottom bar settings", symbol: "slider.horizontal.3")
            }
            .frame(width: 200)
            .background(fm.bgElevated, in: RoundedRectangle(cornerRadius: 14, style: .continuous))
            .shadow(color: Color.black.opacity(0.15), radius: 10, y: 4)
            .opacity(phase % 2 == 1 ? 1 : 0)
            .scaleEffect(phase % 2 == 1 ? 1 : 0.85, anchor: .bottom)
            Text(shape.title)
                .font(.system(size: 13, weight: .semibold))
                .foregroundStyle(fm.accent)
                .contentTransition(.opacity)
            Button(action: { act(phase + 1) }) {
                TourBottomBar(style: style)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(Text("Change the bar's shape"))
        }
    }

    private func menuRow(_ title: String, symbol: String) -> some View {
        HStack(spacing: 10) {
            Text(title)
                .font(.system(size: 13))
                .foregroundStyle(fm.textPrimary)
            Spacer(minLength: 0)
            Image(systemName: symbol)
                .font(.system(size: 13))
                .foregroundStyle(fm.textSecondary)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 7)
    }
}

struct TourChatScene: View {
    @Environment(\.fmPalette) private var fm

    let phase: Int
    let act: (Int) -> Void

    private static let messages: [(mine: Bool, who: String, text: String)] = [
        (false, "Alex", "login is fixed on main 🎉"),
        (false, "Sam", "nice, can someone check the release notes?"),
        (true, "You", "on it, starting an agent now"),
    ]

    private var step: Int { phase % 5 }
    private var shown: Int { min(TourChatScene.messages.count, step + 1) }
    private var reacted: Bool { step >= 3 }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Text("#")
                    .font(.system(size: 18, weight: .bold))
                    .foregroundStyle(fm.textSecondary)
                Text("mesh")
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundStyle(fm.textPrimary)
                Spacer(minLength: 0)
                TourSparkleButton(stars: 2, size: 30)
                AvatarWithDot(state: .connected, label: "You", size: 28, rounded: true)
            }
            .padding(.horizontal, 14)
            .padding(.top, 14)
            Spacer(minLength: 0)
            ForEach(0..<shown, id: \.self) { (at: Int) in
                bubble(TourChatScene.messages[at], at: at)
                    .transition(.move(edge: .bottom).combined(with: .opacity))
            }
            HStack(spacing: 8) {
                Image(systemName: "plus.circle")
                Text("Message #mesh")
                    .foregroundStyle(fm.textTertiary)
                Spacer(minLength: 0)
                Image(systemName: "mic")
            }
            .font(.system(size: 14))
            .foregroundStyle(fm.textSecondary)
            .padding(10)
            .background(fm.bgRaised, in: Capsule())
            .padding(.horizontal, 12)
            .padding(.bottom, 12)
        }
    }

    private func bubble(_ message: (mine: Bool, who: String, text: String), at: Int) -> some View {
        let ink: Color = message.mine ? Color.white : fm.textPrimary
        let fill: Color = message.mine ? fm.accent : fm.bgRaised
        let side: HorizontalAlignment = message.mine ? .trailing : .leading
        let room: CGFloat = at == 0 && reacted ? 8 : 0
        return HStack {
            if message.mine { Spacer(minLength: 40) }
            VStack(alignment: side, spacing: 2) {
                if !message.mine {
                    Text(message.who)
                        .font(.system(size: 11, weight: .semibold))
                        .foregroundStyle(fm.textSecondary)
                }
                Button(action: { act(3) }) {
                    Text(message.text)
                        .font(.system(size: 14))
                        .foregroundStyle(ink)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 8)
                        .background(fill, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
                }
                .buttonStyle(.plain)
                .overlay(alignment: .bottomTrailing) {
                    if at == 0 && reacted {
                        Text("👍 2")
                            .font(.system(size: 11))
                            .padding(.horizontal, 6)
                            .padding(.vertical, 2)
                            .background(fm.bgElevated, in: Capsule())
                            .overlay { Capsule().strokeBorder(fm.border, lineWidth: 0.5) }
                            .offset(x: 10, y: 12)
                            .transition(.scale.combined(with: .opacity))
                    }
                }
            }
            if !message.mine { Spacer(minLength: 40) }
        }
        .padding(.horizontal, 12)
        .padding(.bottom, room)
    }
}

struct TourProjectsScene: View {
    @Environment(\.fmPalette) private var fm

    let phase: Int
    let act: (Int) -> Void

    private static let projects: [(name: String, detail: String)] = [
        ("Personal", "Your own repositories"), ("Team", "Shared with 6 people"), ("Open source", "Public work"),
    ]

    private var open: Bool { phase % 4 != 0 }
    private var current: Int { (phase / 4) % TourProjectsScene.projects.count }
    private var name: String { TourProjectsScene.projects[current].name }

    var body: some View {
        ZStack(alignment: .topLeading) {
            VStack(spacing: 0) {
                TourTopBar(project: name, projectLit: !open, onProject: { tapped() })
                TourTitle(text: "Agents")
                    .padding(.top, 6)
                Spacer(minLength: 0)
                TourBottomBar()
            }
            if open {
                switcher
                    .padding(.top, 52)
                    .padding(.leading, 10)
                    .transition(.scale(scale: 0.2, anchor: .topLeading).combined(with: .opacity))
            }
        }
    }

    private func tapped() {
        act(open ? phase - phase % 4 + 4 : phase + 1)
    }

    private var switcher: some View {
        VStack(alignment: .leading, spacing: 4) {
            ForEach(0..<TourProjectsScene.projects.count, id: \.self) { (at: Int) in
                row(at)
            }
            Label("Invite people to \(name)", systemImage: "person.badge.plus")
                .font(.system(size: 12, weight: .medium))
                .foregroundStyle(fm.accent)
                .padding(8)
        }
        .padding(8)
        .frame(width: 250)
        .background(fm.bgElevated, in: RoundedRectangle(cornerRadius: 18, style: .continuous))
        .shadow(color: Color.black.opacity(0.2), radius: 14, y: 6)
    }

    private func row(_ at: Int) -> some View {
        let project: (name: String, detail: String) = TourProjectsScene.projects[at]
        let next: Bool = at == (current + 1) % TourProjectsScene.projects.count && phase % 4 == 3
        let fill: Color = next ? fm.accentSubtle : Color.clear
        let tile: Color = fm.accent.opacity(0.85 - Double(at) * 0.2)
        return Button(action: { act(at * 4) }) {
            HStack(spacing: 10) {
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .fill(tile)
                    .frame(width: 30, height: 30)
                    .overlay {
                        Text(String(project.name.prefix(1)))
                            .font(.system(size: 14, weight: .bold))
                            .foregroundStyle(Color.white)
                    }
                VStack(alignment: .leading, spacing: 0) {
                    Text(project.name)
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundStyle(fm.textPrimary)
                    Text(project.detail)
                        .font(.system(size: 11))
                        .foregroundStyle(fm.textSecondary)
                }
                Spacer(minLength: 0)
                if at == current {
                    Image(systemName: "checkmark")
                        .font(.system(size: 13, weight: .bold))
                        .foregroundStyle(fm.accent)
                }
            }
            .padding(8)
            .background(fill, in: RoundedRectangle(cornerRadius: 10, style: .continuous))
        }
        .buttonStyle(.plain)
    }
}

struct TourDesktopsScene: View {
    @Environment(\.fmPalette) private var fm

    let phase: Int
    let act: (Int) -> Void

    private var step: Int { phase % 4 }

    private var progress: Double {
        switch step {
        case 1: return 0.35
        case 2: return 0.8
        case 3: return 1
        default: return 0
        }
    }

    private var status: String {
        switch step {
        case 0: return "Build the app"
        case 3: return "Build the app · done"
        default: return "Build the app · running"
        }
    }

    private var button: String {
        switch step {
        case 0: return "Run"
        case 3: return "Again"
        default: return "Stop"
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            TourTopBar()
            TourTitle(text: "Desktops")
            desktop
            TourRow(symbol: "laptopcomputer", tint: fm.textSecondary, title: "Laptop", detail: "Offline since 9:12") {
                Circle().fill(fm.warning).frame(width: 8, height: 8)
            }
            TourRow(symbol: "server.rack", title: "Hosts & Network", detail: "2 hosts · 3 relays healthy") {
                Image(systemName: "chevron.right")
                    .font(.system(size: 12, weight: .semibold))
                    .foregroundStyle(fm.textTertiary)
            }
            Spacer(minLength: 0)
        }
    }

    private var desktop: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                Image(systemName: "desktopcomputer")
                    .font(.system(size: 18))
                    .foregroundStyle(fm.accent)
                VStack(alignment: .leading, spacing: 0) {
                    Text("Studio desktop")
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundStyle(fm.textPrimary)
                    Text("Online · 3 agents running")
                        .font(.system(size: 11))
                        .foregroundStyle(fm.textSecondary)
                }
                Spacer(minLength: 0)
                Circle().fill(fm.success).frame(width: 8, height: 8)
            }
            shortcut
        }
        .padding(12)
        .background(fm.bgRaised, in: RoundedRectangle(cornerRadius: 16, style: .continuous))
        .padding(.horizontal, 12)
    }

    private var shortcut: some View {
        let bar: Color = step == 3 ? fm.success : fm.accent
        return HStack(spacing: 8) {
            Image(systemName: "hammer")
                .foregroundStyle(fm.textSecondary)
            VStack(alignment: .leading, spacing: 4) {
                Text(status)
                    .font(.system(size: 13))
                    .foregroundStyle(fm.textPrimary)
                ProgressView(value: progress)
                    .tint(bar)
                    .opacity(step == 0 ? 0 : 1)
            }
            Spacer(minLength: 0)
            Button(action: { act(step == 0 ? 1 : 0) }) {
                TourPill(text: button, filled: step == 0, lit: step == 0)
            }
            .buttonStyle(.plain)
        }
    }
}

struct TourWorkspaceScene: View {
    @Environment(\.fmPalette) private var fm

    let phase: Int
    let act: (Int) -> Void

    private static let tasks: [String] = ["Write the release notes", "Review #128", "Book the design review"]
    private static let days: [String] = ["M", "T", "W", "T", "F", "S", "S"]
    private static let today: Int = 4

    private var checked: Int { phase % (TourWorkspaceScene.tasks.count + 1) }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            TourTopBar()
            TourTitle(text: "Tasks", trailing: "\(TourWorkspaceScene.tasks.count - checked) left")
            HStack(spacing: 0) {
                ForEach(0..<TourWorkspaceScene.days.count, id: \.self) { (at: Int) in
                    day(at)
                }
            }
            .padding(.horizontal, 12)
            ForEach(0..<TourWorkspaceScene.tasks.count, id: \.self) { (at: Int) in
                taskRow(at)
            }
            Spacer(minLength: 0)
        }
    }

    private func day(_ at: Int) -> some View {
        let today: Bool = at == TourWorkspaceScene.today
        let busy: Bool = at == 1 || today
        return VStack(spacing: 3) {
            Text(TourWorkspaceScene.days[at])
                .font(.system(size: 10, weight: .medium))
                .foregroundStyle(fm.textTertiary)
            Text("\(20 + at)")
                .font(.system(size: 13, weight: today ? .bold : .regular))
                .foregroundStyle(today ? Color.white : fm.textPrimary)
                .frame(width: 26, height: 26)
                .background(today ? fm.accent : Color.clear, in: Circle())
            Circle()
                .fill(busy ? fm.accent : Color.clear)
                .frame(width: 4, height: 4)
        }
        .frame(maxWidth: .infinity)
    }

    private func taskRow(_ at: Int) -> some View {
        let finished: Bool = at < checked
        let mark: Color = finished ? fm.accent : fm.textTertiary
        let ink: Color = finished ? fm.textTertiary : fm.textPrimary
        return Button(action: { act(finished ? at : at + 1) }) {
            HStack(spacing: 10) {
                Image(systemName: finished ? "checkmark.circle.fill" : "circle")
                    .font(.system(size: 18))
                    .foregroundStyle(mark)
                    .symbolEffect(.bounce, value: finished)
                    .tourTarget(at == checked, round: true)
                Text(TourWorkspaceScene.tasks[at])
                    .font(.system(size: 14))
                    .strikethrough(finished)
                    .foregroundStyle(ink)
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)
            .background(fm.bgRaised, in: RoundedRectangle(cornerRadius: 12, style: .continuous))
            .padding(.horizontal, 12)
        }
        .buttonStyle(.plain)
    }
}

struct TourMarketScene: View {
    @Environment(\.fmPalette) private var fm

    let phase: Int
    let act: (Int) -> Void

    private static let packages: [(title: String, symbol: String, detail: String)] = [
        ("Notes", "note.text", "Markdown notes"), ("Water", "drop", "Count your glasses"),
        ("Vault", "lock.shield", "Passwords and secrets"),
    ]

    private var installed: Int { phase % (TourMarketScene.packages.count + 1) }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            TourTopBar()
            home
            TourTitle(text: "Market")
            ForEach(0..<TourMarketScene.packages.count, id: \.self) { (at: Int) in
                listing(at)
            }
            Spacer(minLength: 0)
        }
    }

    private var home: some View {
        HStack(spacing: 0) {
            ForEach(0..<installed, id: \.self) { (at: Int) in
                icon(at)
            }
            Spacer(minLength: 0)
        }
        .frame(height: 56)
        .padding(.horizontal, 12)
        .background(fm.bgRaised.opacity(0.5), in: RoundedRectangle(cornerRadius: 14, style: .continuous))
        .overlay(alignment: .center) {
            if installed == 0 {
                Text("Home")
                    .font(.system(size: 12))
                    .foregroundStyle(fm.textTertiary)
            }
        }
        .padding(.horizontal, 12)
    }

    private func icon(_ at: Int) -> some View {
        let listing: (title: String, symbol: String, detail: String) = TourMarketScene.packages[at]
        let item: ShellBarItem = ShellBarItem(
            id: "tour-\(listing.title)", title: listing.title, symbol: listing.symbol, badge: 0, selected: false,
            kind: .action, pulse: installed, action: {})
        return ShellHomeIcon(item: item, style: .outline, iconSize: 18, labelSize: 10)
            .frame(width: 64)
            .transition(.scale.combined(with: .opacity))
    }

    private func listing(_ at: Int) -> some View {
        let listing: (title: String, symbol: String, detail: String) = TourMarketScene.packages[at]
        let got: Bool = at < installed
        let tint: Color? = got ? fm.success : nil
        return TourRow(symbol: listing.symbol, title: listing.title, detail: listing.detail) {
            Button(action: { act(got ? at : at + 1) }) {
                TourPill(text: got ? "Installed" : "Get", filled: !got, tint: tint, lit: at == installed)
            }
            .buttonStyle(.plain)
        }
    }
}

struct TourPersonalScene: View {
    @Environment(\.fmPalette) private var fm

    let phase: Int
    let act: (Int) -> Void

    private var glasses: Int { phase % 9 }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            TourTopBar()
            HStack(spacing: 16) {
                Button(action: { act(glasses + 1) }) {
                    ZStack(alignment: .bottom) {
                        UnevenRoundedRectangle(bottomLeadingRadius: 14, bottomTrailingRadius: 14)
                            .fill(fm.bgRaised)
                        UnevenRoundedRectangle(bottomLeadingRadius: 14, bottomTrailingRadius: 14)
                            .fill(fm.accent.opacity(0.75))
                            .frame(height: 100 * CGFloat(glasses) / 8)
                    }
                    .frame(width: 64, height: 100)
                    .overlay {
                        UnevenRoundedRectangle(bottomLeadingRadius: 14, bottomTrailingRadius: 14)
                            .stroke(fm.accent, lineWidth: 2)
                    }
                    .tourTarget(glasses < 8)
                }
                .buttonStyle(.plain)
                .accessibilityLabel(Text("Add a glass of water"))
                VStack(alignment: .leading, spacing: 4) {
                    Text("Water")
                        .font(.system(size: 13, weight: .semibold))
                        .foregroundStyle(fm.textSecondary)
                    Text("\(glasses) of 8")
                        .font(.system(size: 28, weight: .bold))
                        .foregroundStyle(fm.textPrimary)
                        .contentTransition(.numericText())
                    Text(glasses >= 8 ? "Done for today" : "glasses today")
                        .font(.system(size: 12))
                        .foregroundStyle(fm.textSecondary)
                }
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 20)
            TourRow(symbol: "lock.shield", title: "github.com", detail: "••••••••••••") {
                Image(systemName: "doc.on.doc")
                    .font(.system(size: 13))
                    .foregroundStyle(fm.textSecondary)
            }
            TourRow(symbol: "figure.walk", tint: fm.success, title: "Steps", detail: "6,240 of 8,000") {
                ProgressView(value: 0.78)
                    .tint(fm.success)
                    .frame(width: 60)
            }
            Spacer(minLength: 0)
        }
    }
}

struct TourSystemScene: View {
    @Environment(\.fmPalette) private var fm

    let phase: Int
    let act: (Int) -> Void

    private var approved: Bool { phase % 3 == 2 }
    private var shown: Bool { phase % 3 != 0 }

    var body: some View {
        ZStack(alignment: .top) {
            LinearGradient(
                colors: [fm.accent.opacity(0.85), Color.indigo.opacity(0.9), Color.black.opacity(0.85)],
                startPoint: .topLeading, endPoint: .bottomTrailing)
            VStack(spacing: 10) {
                Text("9:41")
                    .font(.system(size: 52, weight: .semibold, design: .rounded))
                    .foregroundStyle(Color.white.opacity(0.92))
                    .padding(.top, 14)
                HStack(spacing: 8) {
                    Image(systemName: "sparkles")
                    Text(approved ? "3 running" : "2 running · 1 needs you")
                        .contentTransition(.opacity)
                    Spacer(minLength: 0)
                    Text("6:30")
                        .monospacedDigit()
                }
                .font(.system(size: 12, weight: .semibold))
                .foregroundStyle(Color.white)
                .padding(.horizontal, 14)
                .padding(.vertical, 9)
                .background(Color.black.opacity(0.55), in: Capsule())
                .padding(.horizontal, 30)
                if shown {
                    notification
                        .transition(.move(edge: .top).combined(with: .opacity))
                }
                Spacer(minLength: 0)
            }
        }
    }

    private var notification: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 8) {
                RoundedRectangle(cornerRadius: 6, style: .continuous)
                    .fill(fm.accent)
                    .frame(width: 22, height: 22)
                    .overlay {
                        Image(systemName: "plus")
                            .font(.system(size: 11, weight: .bold))
                            .foregroundStyle(Color.white)
                    }
                Text("FORKMESH")
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundStyle(Color.white.opacity(0.7))
                Spacer(minLength: 0)
                Text("now")
                    .font(.system(size: 11))
                    .foregroundStyle(Color.white.opacity(0.6))
            }
            Text(approved ? "Approved: Agent 754 runs npm test" : "Agent 754 wants to run npm test")
                .font(.system(size: 14, weight: .semibold))
                .foregroundStyle(Color.white)
            if !approved {
                HStack(spacing: 8) {
                    Button(action: { act(2) }) {
                        Text("Approve")
                            .frame(maxWidth: .infinity, minHeight: 32)
                            .background(Color.white.opacity(0.25), in: Capsule())
                            .tourTarget(true)
                    }
                    .buttonStyle(.plain)
                    Text("Deny")
                        .frame(maxWidth: .infinity, minHeight: 32)
                        .background(Color.white.opacity(0.12), in: Capsule())
                }
                .font(.system(size: 13, weight: .semibold))
                .foregroundStyle(Color.white)
            }
        }
        .padding(12)
        .background(Color.white.opacity(0.18), in: RoundedRectangle(cornerRadius: 20, style: .continuous))
        .padding(.horizontal, 14)
    }
}

struct TourLooksScene: View {
    @Environment(\.fmPalette) private var fm

    let phase: Int
    let act: (Int) -> Void

    private var style: ShellHomeIconStyle { ShellHomeIconStyle.allCases[phase % ShellHomeIconStyle.allCases.count] }

    var body: some View {
        VStack(spacing: 12) {
            TourTopBar()
            Spacer(minLength: 0)
            Button(action: { act(phase + 1) }) {
                ShellHomeIconPreview(style: style, iconSize: 22, labelSize: 11, showsCounts: true, highlight: phase % 2 == 1)
                    .padding(.horizontal, 8)
                    .background(fm.bgRaised.opacity(0.5), in: RoundedRectangle(cornerRadius: 18, style: .continuous))
                    .padding(.horizontal, 12)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(Text("Change the icon style"))
            HStack(spacing: 6) {
                ForEach(ShellHomeIconStyle.allCases, id: \.self) { (option: ShellHomeIconStyle) in
                    Button(action: { act(ShellHomeIconStyle.allCases.firstIndex(of: option) ?? 0) }) {
                        TourPill(text: option.title, filled: option == style)
                    }
                    .buttonStyle(.plain)
                }
            }
            Spacer(minLength: 0)
            TourBottomBar()
        }
    }
}
