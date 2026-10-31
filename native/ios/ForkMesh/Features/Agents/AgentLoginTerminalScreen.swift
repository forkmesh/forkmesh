// A node's own provider login (Claude Code, Codex), streamed here through the relay, the
// way the dashboard's "Sign in to Claude Code on <node>" terminal does in a
// browser. The emulator and the session are FMAgents' TerminalScreen and
// AgentLoginTerminal; this draws the grid and the keys a phone lacks.

import FMAccount
import FMAgents
import FMApi
import FMBase
import FMNet
import SwiftUI
import UIKit

struct AgentLoginTerminalScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.openURL) private var openURL
    let node: String
    let provider: String
    @State private var terminal: AgentLoginTerminal?
    @State private var input: String = ""
    @State private var ctrl: Bool = false
    @State private var notice: FmNotice?

    init(node: String, provider: String) {
        self.node = node
        self.provider = provider
        _terminal = State(initialValue: AgentLoginTerminalScreen.makeTerminal(node: node, provider: provider))
    }

    /// A terminal on its own channel to `node`, or nil signed out.
    @MainActor
    static func makeTerminal(node: String, provider: String) -> AgentLoginTerminal? {
        guard let api = Services.get(ApiService.self),
              let connector = Services.get(WebSocketConnector.self),
              let auth = Services.get(AuthService.self),
              !(auth.session?.sessionToken ?? "").isEmpty else {
            return nil
        }
        let channel: AccountEventChannel = AccountEventChannel(
            api: api,
            sessionToken: { auth.session?.sessionToken ?? "" },
            connector: connector,
            nodeName: node
        )
        return AgentLoginTerminal(node: node, provider: provider, channel: channel)
    }

    var body: some View {
        content
            .background(fm.canvas)
            .navigationTitle("Sign in to \(agentProviderLabel(provider))")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { toolbarItems }
            .onDisappear {
                let current: AgentLoginTerminal? = terminal
                Task { await current?.close() }
            }
    }

    @ToolbarContentBuilder private var toolbarItems: some ToolbarContent {
        ToolbarItem(placement: .principal) {
            VStack(spacing: 0) {
                Text("Sign in to \(agentProviderLabel(provider))").fmText(.titleSmall).lineLimit(1)
                Text("on \(node)").fmText(.bodySmall, color: fm.textTertiary).lineLimit(1)
            }
        }
        ToolbarItem(placement: .topBarTrailing) {
            if let terminal = terminal {
                Button {
                    copy(terminal.screen.allRows.map { $0.text }.joined(separator: "\n").dartTrimmed, what: "Terminal text")
                } label: {
                    FmIcon("copy_all_outlined", size: 22, color: fm.textSecondary)
                }
                .accessibilityLabel(Text("Copy terminal text"))
            }
        }
    }

    @ViewBuilder private var content: some View {
        if let terminal = terminal {
            VStack(spacing: 0) {
                TerminalStatusLine(terminal: terminal, onRestart: restartAvailable(terminal) ? { restart() } : nil, notice: $notice)
                if !terminal.signInLink.isEmpty {
                    TerminalLinkBar(
                        url: terminal.signInLink,
                        onCopy: { copy(terminal.signInLink, what: "Link") },
                        onOpen: {
                            if let url = URL(string: terminal.signInLink) {
                                openURL(url)
                            }
                        }
                    )
                }
                TerminalGridView(terminal: terminal)
                    .id(ObjectIdentifier(terminal))
                TerminalKeyBar(terminal: terminal, ctrl: $ctrl, onPaste: {
                    if let text = UIPasteboard.general.string, !text.isEmpty {
                        terminal.paste(text)
                    }
                })
                inputLine
            }
        } else {
            FmEmptyState(
                icon: "lock_outline",
                title: "Sign in to ForkMesh first",
                message: "A node only streams its agent sign-in to the account that owns it."
            )
        }
    }

    private func restartAvailable(_ terminal: AgentLoginTerminal) -> Bool {
        return terminal.exitCode != nil || terminal.statusLevel == .error
    }

    /// A finished or refused login is started over under a fresh session id;
    /// the old one is closed on the node first.
    private func restart() {
        let old: AgentLoginTerminal? = terminal
        terminal = AgentLoginTerminalScreen.makeTerminal(node: node, provider: provider)
        Task { await old?.close() }
    }

    private func copy(_ text: String, what: String) {
        UIPasteboard.general.string = text
        notice = FmNotice("\(what) copied")
    }

    private var inputLine: some View {
        HStack(spacing: FmSpace.x2) {
            TextField("Type or paste, then send (sends Enter)", text: $input)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled(true)
                .fmMono(size: FmType.small, color: fm.textPrimary)
                .submitLabel(.send)
                .onSubmit { submitLine() }
                .terminalChords(text: $input, ctrl: $ctrl, terminal: terminal)
            Button {
                submitLine()
            } label: {
                FmIcon("keyboard_return", size: 22, color: fm.accent)
                    .frame(width: 44, height: 44)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(Text("Send with Enter"))
        }
        .padding(EdgeInsets.fmLTRB(FmSpace.x3, 0, FmSpace.x3, FmSpace.x3))
    }

    private func submitLine() {
        guard let terminal = terminal else {
            return
        }
        let text: String = input
        input = ""
        terminal.paste(text)
        // Enter goes as its own frame after the text, so a CLI that reads the
        // text as a paste does not take the Enter as part of it.
        if text.isEmpty {
            terminal.key(.enter)
        } else {
            Task {
                try? await Task.sleep(nanoseconds: 120_000_000)
                if !terminal.closed {
                    terminal.key(.enter)
                }
            }
        }
    }
}

struct TerminalStatusLine: View {
    @Environment(\.fmPalette) private var fm
    let terminal: any StreamedTerminal
    let onRestart: (() -> Void)?
    var notice: Binding<FmNotice?> = .constant(nil)

    private var color: Color {
        switch terminal.statusLevel {
        case .error: return fm.danger
        case .ok: return fm.success
        case .pending: return fm.textSecondary
        }
    }

    private var icon: String {
        switch terminal.statusLevel {
        case .error: return "error_outline"
        case .ok: return "check_circle_outline"
        case .pending: return "sync"
        }
    }

    var body: some View {
        HStack(spacing: FmSpace.x2) {
            FmIcon(icon, size: 16, color: color)
            // Two lines are held whatever the status says, so the grid under
            // it never changes height (a resize makes the CLI redraw).
            statusText
                .fmText(.bodySmall, color: color, height: 1.35)
                .lineLimit(2, reservesSpace: true)
                .frame(maxWidth: .infinity, alignment: .leading)
            FmMessageTools(statusString)
            if notice.wrappedValue != nil {
                FmIconButton(label: "Dismiss", onPressed: { notice.wrappedValue = nil }) {
                    FmIcon("close", size: 14, color: fm.textTertiary)
                }
                .frame(width: 28, height: 28)
            }
            if let onRestart = onRestart {
                FmToneButton(label: "Start again", icon: "refresh", onPressed: onRestart)
            }
        }
        .padding(.horizontal, FmSpace.x4)
        .padding(.vertical, FmSpace.x2)
        .onChange(of: terminal.statusText) { (_: String, _: String) in
            notice.wrappedValue = nil
        }
    }

    private var statusString: String {
        guard let shown = notice.wrappedValue else {
            return terminal.statusText
        }
        return shown.message + "\n" + terminal.statusText
    }

    private var statusText: Text {
        guard let shown = notice.wrappedValue else {
            return Text(terminal.statusText)
        }
        return Text(shown.message).foregroundColor(shown.isError ? fm.danger : fm.textPrimary)
            + Text("\n" + terminal.statusText).foregroundColor(color)
    }
}

/// The sign-in page as a button: a ~450-character URL wrapped across a dozen
/// rows cannot realistically be selected out whole on a phone.
struct TerminalLinkBar: View {
    @Environment(\.fmPalette) private var fm
    let url: String
    let onCopy: () -> Void
    let onOpen: () -> Void

    private var title: String {
        let host: String = URL(string: url)?.host ?? ""
        return host.isEmpty ? url : "Sign-in page on \(host)"
    }

    var body: some View {
        FmCard(radius: FmRadius.md) {
            HStack(spacing: FmSpace.x2) {
                FmIcon("link", size: 18, color: fm.accent)
                Text(title)
                    .fmText(.bodySmall)
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .frame(maxWidth: .infinity, alignment: .leading)
                FmToneButton(label: "Copy", icon: "copy", onPressed: onCopy)
                FmToneButton(label: "Open", icon: "open_in_new", tone: .accent, filled: true, onPressed: onOpen)
            }
        }
        .padding(EdgeInsets.fmLTRB(FmSpace.x4, 0, FmSpace.x4, FmSpace.x2))
    }
}

/// The keys a phone keyboard does not have.
struct TerminalKeyBar: View {
    @Environment(\.fmPalette) private var fm
    let terminal: any StreamedTerminal
    @Binding var ctrl: Bool
    let onPaste: () -> Void

    var body: some View {
        Grid(horizontalSpacing: FmSpace.x1, verticalSpacing: FmSpace.x1) {
            GridRow {
                button("Esc") { send(.escape) }
                button("Tab") { send(.tab) }
                button("Ctrl", spoken: "Control", latched: ctrl) { ctrl.toggle() }
                button("^C", spoken: "Control-C") { send(.ctrlC) }
                button("⌫", spoken: "Backspace") { send(.backspace) }
                button("", icon: "content_paste", spoken: "Paste") {
                    ctrl = false
                    onPaste()
                }
            }
            GridRow {
                button("←", spoken: "Left") { send(.left) }
                button("↑", spoken: "Up") { send(.up) }
                button("↓", spoken: "Down") { send(.down) }
                button("→", spoken: "Right") { send(.right) }
                button("Enter") { send(.enter) }
                    .gridCellColumns(2)
            }
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.vertical, FmSpace.x2)
        .accessibilityIdentifier("terminal-key-bar")
    }

    private func send(_ key: TerminalKey) {
        if ctrl {
            ctrl = false
            terminal.input(terminalKeyBytes(key, appCursorKeys: terminal.screen.appCursorKeys, control: true))
        } else {
            terminal.key(key)
        }
    }

    private func button(
        _ label: String, icon: String? = nil, spoken: String? = nil, latched: Bool = false,
        action: @escaping () -> Void
    ) -> some View {
        let color: Color = latched ? Color.white : fm.textSecondary
        return Button {
            FmHaptics.tap()
            action()
        } label: {
            Group {
                if let icon = icon {
                    FmIcon(icon, size: 18, color: color)
                } else {
                    Text(label)
                        .fmText(.labelMedium, color: color, height: 1)
                        .lineLimit(1)
                        .minimumScaleFactor(0.7)
                }
            }
            .frame(maxWidth: .infinity, minHeight: 36)
            .background(latched ? fm.accent : fm.bgOverlay)
            .clipShape(FmRoundedShape(radius: FmRadius.md))
        }
        .buttonStyle(FmPressStyle(highlight: color.opacity(0.12), cornerRadius: FmRadius.md))
        .disabled(terminal.closed)
        .opacity(terminal.closed ? 0.45 : 1)
        .accessibilityLabel(Text(spoken ?? label))
        .accessibilityAddTraits(latched ? .isSelected : [])
    }
}

struct TerminalChordInput: ViewModifier {
    @Binding var text: String
    @Binding var ctrl: Bool
    let terminal: (any StreamedTerminal)?
    @FocusState private var focused: Bool

    func body(content: Content) -> some View {
        content
            .focused($focused)
            .onChange(of: ctrl) { (_: Bool, on: Bool) in
                if on {
                    focused = true
                }
            }
            .onChange(of: text) { (old: String, new: String) in
                guard ctrl, new.count > old.count, let typed = TerminalChordInput.typed(old, new) else {
                    return
                }
                ctrl = false
                let bytes: String = terminalControlBytes(String(typed))
                if bytes.isEmpty {
                    return
                }
                text = old
                terminal?.input(bytes)
            }
    }

    static func typed(_ old: String, _ new: String) -> Character? {
        let before: [Character] = Array(old)
        let after: [Character] = Array(new)
        for index in after.indices where index >= before.count || before[index] != after[index] {
            return after[index]
        }
        return nil
    }
}

extension View {
    func terminalChords(text: Binding<String>, ctrl: Binding<Bool>, terminal: (any StreamedTerminal)?) -> some View {
        modifier(TerminalChordInput(text: text, ctrl: ctrl, terminal: terminal))
    }
}

// MARK: - Grid

extension FmPalette {
    var terminalSurface: Color { return wallpaper ? TerminalPalette.background.opacity(0.8) : TerminalPalette.background }
}

/// xterm's 16 base colours, then the 6x6x6 cube and greyscale ramp: the
/// palette the dashboard's terminal draws with.
enum TerminalPalette {
    static let background: Color = Color(fmARGB: 0xFF0D0D10)
    static let foreground: Color = Color(fmARGB: 0xFFC7C9CF)
    static let base: [UInt32] = [
        0xFF1E1E21, 0xFFE05561, 0xFF8CC265, 0xFFD5A24A, 0xFF4D9FE8, 0xFFC162DE, 0xFF42B3C2, 0xFFC7C9CF,
        0xFF5C6370, 0xFFFF6D78, 0xFFA5E075, 0xFFF0C674, 0xFF61AFEF, 0xFFD982F0, 0xFF56CFE1, 0xFFF2F3F5,
    ]

    static func color(_ index: Int) -> Color? {
        if index < 0 {
            return nil
        }
        if index < 16 {
            return Color(fmARGB: base[index])
        }
        if index < 232 {
            let levels: [Double] = [0, 95, 135, 175, 215, 255]
            let value: Int = index - 16
            return Color(
                red: levels[(value / 36) % 6] / 255,
                green: levels[(value / 6) % 6] / 255,
                blue: levels[value % 6] / 255
            )
        }
        if index < 256 {
            let grey: Double = Double(8 + (index - 232) * 10) / 255
            return Color(red: grey, green: grey, blue: grey)
        }
        let rgb: Int = index - AgentTerminalConfig.trueColor
        return Color(
            red: Double((rgb >> 16) & 255) / 255,
            green: Double((rgb >> 8) & 255) / 255,
            blue: Double(rgb & 255) / 255
        )
    }
}

/// The grid itself: a fixed-pitch row per line, scrollback above, pinned to
/// the bottom. Sized to the pane; a settled layout change is sent once.
struct TerminalGridView: View {
    @Environment(\.fmPalette) private var fm
    let terminal: any StreamedTerminal
    @State private var resizeTask: Task<Void, Never>?
    @State private var pitch: CGFloat = TerminalGridView.cellWidth
    @State private var bottomRow: Int?
    @State private var following: Bool = true
    @State private var jump: Int = 0

    private static let fontSize: CGFloat = FmType.meta
    private static let lineHeight: CGFloat = (FmType.meta * 1.25).rounded(.up)
    private static let rulerLength: Int = 100
    private static let uiFont: UIFont = UIFont.monospacedSystemFont(ofSize: TerminalGridView.fontSize, weight: .regular)
    private static let cellWidth: CGFloat = {
        let width: CGFloat = ("0000000000" as NSString).size(withAttributes: [.font: TerminalGridView.uiFont]).width
        return width / 10
    }()
    @MainActor private static var glyphWidths: [String: CGFloat] = [:]

    var body: some View {
        GeometryReader { (proxy: GeometryProxy) in
            rows(width: max(0, proxy.size.width - FmSpace.x2 * 2))
                .onAppear { fit(proxy.size) }
                .onChange(of: proxy.size) { (_: CGSize, size: CGSize) in
                    fit(size)
                }
                .onChange(of: pitch) { (_: CGFloat, _: CGFloat) in
                    fit(proxy.size)
                }
        }
        .background(alignment: .topLeading) { ruler }
        .background(fm.terminalSurface)
        .clipped()
    }

    private var ruler: some View {
        Text(String(repeating: "0", count: TerminalGridView.rulerLength))
            .font(.system(size: TerminalGridView.fontSize, design: .monospaced))
            .lineLimit(1)
            .fixedSize()
            .hidden()
            .onGeometryChange(for: CGFloat.self, of: { $0.size.width }) { (width: CGFloat) in
                if width > 0 {
                    pitch = width / CGFloat(TerminalGridView.rulerLength)
                }
            }
    }

    private func rows(width: CGFloat) -> some View {
        let screen: TerminalScreen = terminal.screen
        let _: Int = terminal.revision
        let all: [TerminalRow] = screen.allRows
        let urls: [[Int: String]?] = TerminalScreen.wrappedUrlTargets(all)
        let cursorRow: Int = screen.cursorVisible ? screen.scrollback.count + screen.cy : -1
        return ScrollViewReader { (reader: ScrollViewProxy) in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 0) {
                    ForEach(Array(all.indices), id: \.self) { (index: Int) in
                        Text(TerminalGridView.attributed(all[index], urls: urls[index], cursorCol: index == cursorRow ? screen.cx : -1))
                            .font(.system(size: TerminalGridView.fontSize, design: .monospaced))
                            .foregroundColor(TerminalPalette.foreground)
                            .lineLimit(1)
                            .fixedSize(horizontal: true, vertical: false)
                            .frame(width: width, height: TerminalGridView.lineHeight, alignment: .leading)
                            .clipped()
                            .id(index)
                    }
                }
                .scrollTargetLayout()
                .frame(width: width, alignment: .leading)
                .padding(FmSpace.x2)
            }
            .defaultScrollAnchor(.bottom)
            .scrollPosition(id: $bottomRow, anchor: .bottom)
            .onChange(of: bottomRow) { (_: Int?, row: Int?) in
                following = row.map { $0 >= all.count - 2 } ?? true
            }
            .onChange(of: terminal.revision) { (_: Int, _: Int) in
                if following {
                    reader.scrollTo(all.count - 1, anchor: .bottom)
                }
            }
            .onChange(of: jump) { (_: Int, _: Int) in
                following = true
                reader.scrollTo(all.count - 1, anchor: .bottom)
            }
            .overlay(alignment: .bottomTrailing) {
                if !following {
                    Button {
                        FmHaptics.tap()
                        jump += 1
                    } label: {
                        FmIcon("arrow_downward", size: 20, color: Color.white)
                            .frame(width: 40, height: 40)
                            .background(Circle().fill(fm.accent))
                    }
                    .buttonStyle(.plain)
                    .padding(FmSpace.x3)
                    .accessibilityLabel(Text("Scroll to the newest output"))
                    .accessibilityIdentifier("terminal-scroll-to-bottom")
                }
            }
        }
    }

    /// Size the grid to the pane. The first measurement starts the session;
    /// later ones are debounced, since the remote redraws on every SIGWINCH.
    private func fit(_ size: CGSize) {
        let cols: Int = Int(((size.width - FmSpace.x2 * 2) / pitch).rounded(.down))
        let rows: Int = Int(((size.height - FmSpace.x2 * 2) / TerminalGridView.lineHeight).rounded(.down))
        if cols <= 0 || rows <= 0 {
            return
        }
        resizeTask?.cancel()
        let terminal: any StreamedTerminal = self.terminal
        resizeTask = Task { @MainActor in
            try? await Task.sleep(nanoseconds: 150_000_000)
            if Task.isCancelled {
                return
            }
            // `start` is a no-op once started; `resize` before start only
            // records the size.
            terminal.resize(cols: cols, rows: rows)
            terminal.start(cols: cols, rows: rows)
        }
    }

    /// One row as styled runs. Trailing default blanks are dropped; the
    /// cursor cell is drawn inverted.
    @MainActor static func attributed(_ row: TerminalRow, urls: [Int: String]?, cursorCol: Int) -> AttributedString {
        let cells: [TerminalCell] = row.cells
        var width: Int = cells.count
        while width > 0 {
            let cell: TerminalCell = cells[width - 1]
            if cell.ch != " " || cell.bg != AgentTerminalConfig.defaultColor || cell.underline {
                break
            }
            width -= 1
        }
        if cursorCol + 1 > width {
            width = min(cursorCol + 1, cells.count)
        }
        var out: AttributedString = AttributedString()
        var index: Int = 0
        while index < width {
            let cell: TerminalCell = cells[index]
            var piece: AttributedString = AttributedString(cell.ch)
            let linked: Bool = !cell.link.isEmpty || !(urls?[index] ?? "").isEmpty
            var fg: Color? = TerminalPalette.color(cell.inverse ? cell.bg : cell.fg) ?? (cell.inverse ? TerminalPalette.background : nil)
            var bg: Color? = TerminalPalette.color(cell.inverse ? cell.fg : cell.bg) ?? (cell.inverse ? TerminalPalette.foreground : nil)
            if index == cursorCol {
                fg = TerminalPalette.background
                bg = TerminalPalette.foreground
            }
            if cell.dim {
                fg = (fg ?? TerminalPalette.foreground).opacity(0.7)
            }
            if let fg = fg {
                piece.foregroundColor = fg
            }
            if let bg = bg {
                piece.backgroundColor = bg
            }
            var size: CGFloat = TerminalGridView.fontSize
            if cell.ch.utf8.count > 1 {
                let natural: CGFloat = TerminalGridView.glyphWidth(cell.ch)
                if natural > TerminalGridView.cellWidth * 1.25 {
                    size = TerminalGridView.fontSize * TerminalGridView.cellWidth / natural
                    piece.font = Font.system(size: size, design: .monospaced)
                } else if abs(TerminalGridView.cellWidth - natural) > 0.01 {
                    piece[AttributeScopes.SwiftUIAttributes.KerningAttribute.self] = TerminalGridView.cellWidth - natural
                }
            }
            if cell.bold {
                piece.font = Font.system(size: size, weight: .semibold, design: .monospaced)
            }
            if cell.underline || linked {
                piece.underlineStyle = Text.LineStyle.single
            }
            out.append(piece)
            index += 1
        }
        return out
    }

    @MainActor private static func glyphWidth(_ ch: String) -> CGFloat {
        if let known = glyphWidths[ch] {
            return known
        }
        let width: CGFloat = (ch as NSString).size(withAttributes: [.font: TerminalGridView.uiFont]).width
        glyphWidths[ch] = width
        return width
    }
}
