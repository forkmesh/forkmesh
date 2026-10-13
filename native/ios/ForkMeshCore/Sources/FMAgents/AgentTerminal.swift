// A provider login terminal, streamed from one of the account's own nodes.
//
// This is the phone's half of the dashboard's "Sign in to Claude Code on
// `<node>`" terminal (`app/public/agent-terminal.js`). A node (typically a
// managed mirror) can only run agents once its provider CLI is signed in, and
// a mirror has no screen of its own to do that on. So the node runs the
// provider's own login command in a pty and streams it here: both halves hold
// a WebSocket to the per-owner `ForkMeshNodes` relay object, and the relay
// forwards `{"type":"terminal"}` frames between them without keeping a copy
// (`app/src/node_terminal.py`, `server/terminal.go`). The node runs exactly
// one thing, the provider's login, and decides whether to run it.
//
// What this file owns is the stream's client half: a small VT100/xterm subset,
// enough for the provider CLIs' full-screen redraws, and the session that
// feeds it. The view lives in Features/Agents/AgentLoginTerminalScreen.swift.

import FMAccount
import FMApi
import FMBase
import Foundation
import Observation

public enum AgentTerminalConfig {
    /// The node coalesces output, so these are redraw bursts rather than lines.
    public static let maxScrollback = 10_000
    /// Keystrokes are batched at roughly a frame.
    public static let inputFlush: TimeInterval = 0.025
    /// How long to wait for the node to answer an open before saying it is
    /// not there. The relay is a wire, not a party: silence is the only signal.
    public static let openTimeout: TimeInterval = 9
    /// An OSC string longer than this is dropped whole. A sign-in URL is ~450
    /// characters; nothing a login prompt sends comes close.
    static let maxOscChars = 4096
    /// Input bytes per frame. The relay caps `data` at 12000 base64
    /// characters, which is 9000 bytes; stay under it with room to spare.
    static let maxInputBytesPerFrame = 8000

    /// Default colours are "unset": ordinary text carries no colour of its
    /// own, and the view resolves it against the terminal's palette.
    public static let defaultColor = -1
    /// 24-bit colours are packed as this | rgb, so one int describes a cell's
    /// colour whatever its source.
    public static let trueColor = 0x1000000
}

/// Providers the node will run a login for, with the names people know them
/// by, in order.
public let agentLoginProviders: [(id: String, label: String)] = [
    ("claude-code", "Claude Code"),
    ("codex", "Codex"),
]

public func isAgentLoginProvider(_ provider: String) -> Bool {
    agentLoginProviders.contains { $0.id == provider }
}

public func agentProviderLabel(_ provider: String) -> String {
    if provider == "local-llama" { return "Gemma" }
    return agentLoginProviders.first { $0.id == provider }?.label ?? provider
}

/// Only web links are kept: the target comes from the remote pty, and
/// anything else must stay inert text.
private let safeLinkPattern = try! NSRegularExpression(pattern: "^https?://[\\x21-\\x7e]+$", options: [.caseInsensitive])
private let plainUrlPattern = try! NSRegularExpression(pattern: "https?://[\\x21-\\x7e]+")
private let urlTrailerPattern = try! NSRegularExpression(pattern: "[.,;:!?)\\]}>'\"]+$")

func safeTerminalLink(_ uri: String) -> String {
    let range = NSRange(uri.startIndex..., in: uri)
    return safeLinkPattern.firstMatch(in: uri, range: range) != nil ? uri : ""
}

public struct TerminalCell: Equatable, Sendable {
    public var ch: String
    public var fg: Int
    public var bg: Int
    public var bold: Bool
    public var dim: Bool
    public var inverse: Bool
    public var underline: Bool
    public var link: String

    public init(
        _ ch: String,
        fg: Int = AgentTerminalConfig.defaultColor,
        bg: Int = AgentTerminalConfig.defaultColor,
        bold: Bool = false,
        dim: Bool = false,
        inverse: Bool = false,
        underline: Bool = false,
        link: String = ""
    ) {
        self.ch = ch
        self.fg = fg
        self.bg = bg
        self.bold = bold
        self.dim = dim
        self.inverse = inverse
        self.underline = underline
        self.link = link
    }

    public static let blank = TerminalCell(" ")

    /// Whether this cell draws the same way as `other`, so the two can share
    /// one text run.
    public func sameStyle(_ other: TerminalCell) -> Bool {
        fg == other.fg && bg == other.bg && bold == other.bold && dim == other.dim
            && inverse == other.inverse && underline == other.underline
            && link.isEmpty == other.link.isEmpty
    }
}

/// One grid line. A class, because the scrollback and a resized screen
/// share row objects with the grid they came from.
public final class TerminalRow {
    public var cells: [TerminalCell]

    /// Set when the grid soft-wrapped out of this row, so a URL it split can
    /// be found again as one string.
    public var wrapped = false

    public init(_ cols: Int) {
        cells = Array(repeating: .blank, count: cols)
    }

    /// The row's text with trailing blanks dropped.
    public var text: String {
        var width = cells.count
        while width > 0 && cells[width - 1].ch == " " { width -= 1 }
        return cells.prefix(width).map(\.ch).joined()
    }
}

/// A minimal VT parser over decoded text. Everything it does not understand is
/// dropped rather than printed: a stray escape sequence painted literally is
/// worse than a missing one, and the provider CLIs stay inside this subset.
public final class TerminalScreen {
    private enum ParseState { case ground, esc, escCharset, csi, osc, oscEsc }

    public let rows: Int
    public let cols: Int
    public private(set) var grid: [TerminalRow]
    private var alt: [TerminalRow]?
    public private(set) var scrollback: [TerminalRow] = []
    public private(set) var cx = 0
    public private(set) var cy = 0
    private var savedCx = 0
    private var savedCy = 0
    public private(set) var scrollTop = 0
    public private(set) var scrollBottom: Int
    private var fg = AgentTerminalConfig.defaultColor
    private var bg = AgentTerminalConfig.defaultColor
    private var bold = false
    private var dim = false
    private var inverse = false
    private var underline = false
    private var wrap = true
    private var wrapPending = false
    public private(set) var cursorVisible = true
    public private(set) var appCursorKeys = false
    public private(set) var bracketedPaste = false
    private var state = ParseState.ground
    private var params = ""
    private var intermediate = ""
    private var osc = ""
    private var oscLength = 0
    private var link = ""

    public init(_ rows: Int, _ cols: Int) {
        self.rows = rows
        self.cols = cols
        scrollBottom = rows - 1
        grid = (0..<rows).map { _ in TerminalRow(cols) }
    }

    public var onAltScreen: Bool { alt != nil }

    /// Scrollback followed by the live grid: everything the view can show.
    public var allRows: [TerminalRow] { scrollback + grid }

    private func clampCursor() {
        cx = min(max(cx, 0), cols - 1)
        cy = min(max(cy, 0), rows - 1)
    }

    private func scrollUp(_ top: Int, _ bottom: Int, _ count: Int) {
        for _ in 0..<max(0, count) {
            let removed = grid.remove(at: top)
            // Only the primary screen keeps history; an alternate-screen
            // redraw is transient by definition and would otherwise flood the
            // scrollback.
            if alt == nil && top == 0 {
                scrollback.append(removed)
                if scrollback.count > AgentTerminalConfig.maxScrollback { scrollback.removeFirst() }
            }
            grid.insert(TerminalRow(cols), at: bottom)
        }
    }

    private func scrollDown(_ top: Int, _ bottom: Int, _ count: Int) {
        for _ in 0..<max(0, count) {
            grid.remove(at: bottom)
            grid.insert(TerminalRow(cols), at: top)
        }
    }

    private func newLine() {
        if cy == scrollBottom {
            scrollUp(scrollTop, scrollBottom, 1)
        } else if cy < rows - 1 {
            cy += 1
        }
    }

    private func putChar(_ ch: String) {
        if wrapPending && wrap {
            grid[cy].wrapped = true
            cx = 0
            newLine()
        }
        wrapPending = false
        clampCursor()
        grid[cy].cells[cx] = TerminalCell(ch, fg: fg, bg: bg, bold: bold, dim: dim, inverse: inverse, underline: underline, link: link)
        if cx + 1 >= cols {
            wrapPending = true
        } else {
            cx += 1
        }
    }

    private func erase(_ row: TerminalRow, _ from: Int, _ to: Int) {
        if from <= 0 && to >= row.cells.count - 1 { row.wrapped = false }
        let cell = TerminalCell(" ", bg: bg)
        var i = max(0, from)
        while i <= to && i < row.cells.count {
            row.cells[i] = cell
            i += 1
        }
    }

    static func sgrCodes(_ raw: String) -> [Int] {
        raw.split(separator: ";", omittingEmptySubsequences: false).flatMap { group -> [Int] in
            let parts = group.split(separator: ":", omittingEmptySubsequences: false).map { Int($0) ?? 0 }
            if parts.count == 1 { return parts }
            if parts[0] == 4 { return [parts[1] == 0 ? 24 : 4] }
            if (parts[0] == 38 || parts[0] == 48) && parts[1] == 5 && parts.count >= 3 { return Array(parts.prefix(3)) }
            if (parts[0] == 38 || parts[0] == 48) && parts[1] == 2 && parts.count >= 5 { return [parts[0], 2] + parts.suffix(3) }
            return []
        }
    }

    private func applySgr(_ params: [Int]) {
        let codes = params.isEmpty ? [0] : params
        func at(_ i: Int) -> Int { i < codes.count ? codes[i] : 0 }
        var i = 0
        while i < codes.count {
            let code = codes[i]
            if code == 0 {
                fg = AgentTerminalConfig.defaultColor
                bg = AgentTerminalConfig.defaultColor
                bold = false
                dim = false
                inverse = false
                underline = false
            } else if code == 1 {
                bold = true
            } else if code == 2 {
                dim = true
            } else if code == 4 {
                underline = true
            } else if code == 7 {
                inverse = true
            } else if code == 22 {
                bold = false
                dim = false
            } else if code == 24 {
                underline = false
            } else if code == 27 {
                inverse = false
            } else if code >= 30 && code <= 37 {
                fg = code - 30
            } else if code == 39 {
                fg = AgentTerminalConfig.defaultColor
            } else if code >= 40 && code <= 47 {
                bg = code - 40
            } else if code == 49 {
                bg = AgentTerminalConfig.defaultColor
            } else if code >= 90 && code <= 97 {
                fg = code - 90 + 8
            } else if code >= 100 && code <= 107 {
                bg = code - 100 + 8
            } else if code == 38 || code == 48 {
                var value: Int?
                if at(i + 1) == 5 {
                    value = min(max(at(i + 2), 0), 255)
                    i += 2
                } else if at(i + 1) == 2 {
                    let r = min(max(at(i + 2), 0), 255)
                    let g = min(max(at(i + 3), 0), 255)
                    let b = min(max(at(i + 4), 0), 255)
                    value = AgentTerminalConfig.trueColor + ((r << 16) | (g << 8) | b)
                    i += 4
                }
                if let value {
                    if code == 38 { fg = value } else { bg = value }
                }
            }
            i += 1
        }
    }

    private func useAltScreen(_ on: Bool) {
        if on && alt == nil {
            alt = grid
            grid = (0..<rows).map { _ in TerminalRow(cols) }
            cx = 0
            cy = 0
        } else if !on, let primary = alt {
            grid = primary
            alt = nil
        }
    }

    private func handleMode(_ params: [Int], _ enable: Bool, _ isPrivate: Bool) {
        if !isPrivate { return }
        for code in params {
            if code == 1 {
                appCursorKeys = enable
            } else if code == 7 {
                wrap = enable
            } else if code == 25 {
                cursorVisible = enable
            } else if code == 2004 {
                bracketedPaste = enable
            } else if code == 47 || code == 1047 || code == 1049 {
                if code == 1049 && enable {
                    savedCx = cx
                    savedCy = cy
                }
                useAltScreen(enable)
                if code == 1049 && !enable {
                    cx = savedCx
                    cy = savedCy
                }
            }
        }
    }

    private func handleCsi(_ finalByte: Character) {
        let isPrivate = intermediate == "?"
        if !intermediate.isEmpty && !(isPrivate && "hlJK".contains(finalByte)) { return }
        let raw = params
        let values = raw.split(separator: ";", omittingEmptySubsequences: false).map { part in
            part.isEmpty ? 0 : (Int(part) ?? 0)
        }
        let first = values.first ?? 0
        let count = max(1, first)
        func param(_ i: Int, _ fallback: Int) -> Int {
            i < values.count && values[i] != 0 ? values[i] : fallback
        }
        switch finalByte {
        case "A":
            cy = max(scrollTop, cy - count)
        case "B":
            cy = min(scrollBottom, cy + count)
        case "C":
            cx = min(cols - 1, cx + count)
            wrapPending = false
        case "D":
            cx = max(0, cx - count)
            wrapPending = false
        case "E":
            cx = 0
            cy = min(rows - 1, cy + count)
        case "F":
            cx = 0
            cy = max(0, cy - count)
        case "G", "`":
            cx = count - 1
            wrapPending = false
        case "d":
            cy = count - 1
        case "H", "f":
            cy = param(0, 1) - 1
            cx = param(1, 1) - 1
            wrapPending = false
        case "J":
            clampCursor()
            if first == 0 {
                erase(grid[cy], cx, cols - 1)
                for y in (cy + 1)..<max(cy + 1, rows) { erase(grid[y], 0, cols - 1) }
            } else if first == 1 {
                for y in 0..<cy { erase(grid[y], 0, cols - 1) }
                erase(grid[cy], 0, cx)
            } else {
                for y in 0..<rows { erase(grid[y], 0, cols - 1) }
            }
        case "K":
            clampCursor()
            if first == 0 {
                erase(grid[cy], cx, cols - 1)
            } else if first == 1 {
                erase(grid[cy], 0, cx)
            } else {
                erase(grid[cy], 0, cols - 1)
            }
        case "L":
            scrollDown(cy, scrollBottom, count)
        case "M":
            scrollUp(cy, scrollBottom, count)
        case "P":
            clampCursor()
            let row = grid[cy]
            let n = min(count, cols - cx)
            row.cells.removeSubrange(cx..<(cx + n))
            row.cells.append(contentsOf: Array(repeating: TerminalCell.blank, count: n))
        case "@":
            clampCursor()
            let row = grid[cy]
            let n = min(count, cols - cx)
            row.cells.insert(contentsOf: Array(repeating: TerminalCell.blank, count: n), at: cx)
            row.cells.removeSubrange(cols..<row.cells.count)
        case "X":
            clampCursor()
            erase(grid[cy], cx, cx + count - 1)
        case "S":
            scrollUp(scrollTop, scrollBottom, count)
        case "T":
            scrollDown(scrollTop, scrollBottom, count)
        case "r":
            scrollTop = max(0, param(0, 1) - 1)
            scrollBottom = min(rows - 1, param(1, rows) - 1)
            if scrollBottom <= scrollTop {
                scrollTop = 0
                scrollBottom = rows - 1
            }
            cx = 0
            cy = scrollTop
        case "h":
            handleMode(values, true, isPrivate)
        case "l":
            handleMode(values, false, isPrivate)
        case "m":
            applySgr(raw.isEmpty ? [] : Self.sgrCodes(raw))
        case "s":
            savedCx = cx
            savedCy = cy
        case "u":
            cx = savedCx
            cy = savedCy
        default:
            break
        }
        clampCursor()
    }

    /// OSC 8 (`8;params;URI`) opens or closes a hyperlink over the cells that
    /// follow. Claude Code prints its sign-in URL that way, and the link
    /// target is the one copy of the URL no row wrap can split. Every other
    /// OSC addresses window chrome this emulator does not have.
    private func endOsc() {
        let text = osc
        osc = ""
        oscLength = 0
        if text.utf16.count > AgentTerminalConfig.maxOscChars || !text.hasPrefix("8;") { return }
        let afterPrefix = text.index(text.startIndex, offsetBy: 2)
        guard let split = text[afterPrefix...].firstIndex(of: ";") else { return }
        link = safeTerminalLink(String(text[text.index(after: split)...]))
    }

    private func reset() {
        grid = (0..<rows).map { _ in TerminalRow(cols) }
        alt = nil
        cx = 0
        cy = 0
        savedCx = 0
        savedCy = 0
        scrollTop = 0
        scrollBottom = rows - 1
        fg = AgentTerminalConfig.defaultColor
        bg = AgentTerminalConfig.defaultColor
        bold = false
        dim = false
        inverse = false
        underline = false
        wrap = true
        wrapPending = false
        cursorVisible = true
        appCursorKeys = false
        bracketedPaste = false
        link = ""
    }

    /// This screen at a new size, drawn the way a real terminal keeps it: rows
    /// are cut or padded rather than reflowed, and a shorter grid gives up its
    /// top rows to the scrollback so the cursor keeps its place in the text.
    ///
    /// The remote program redraws after SIGWINCH relative to where it left the
    /// cursor, so a blank grid with the cursor sent home turns that redraw
    /// into a torn, half-painted screen and loses whatever it does not
    /// repaint, the sign-in URL among it.
    public func resized(_ rows: Int, _ cols: Int) -> TerminalScreen {
        let next = TerminalScreen(rows, cols)
        func fit(_ row: TerminalRow) -> TerminalRow {
            if row.cells.count == cols { return row }
            let out = TerminalRow(cols)
            var i = 0
            while i < cols && i < row.cells.count {
                out.cells[i] = row.cells[i]
                i += 1
            }
            return out
        }
        func refit(_ from: [TerminalRow], _ cursorRow: Int, _ keep: Bool) -> [TerminalRow] {
            var kept = from
            // Blank rows under the cursor go first, then rows off the top.
            while kept.count > rows && kept.count - 1 > cursorRow { kept.removeLast() }
            while kept.count > rows {
                let removed = kept.removeFirst()
                if keep { next.scrollback.append(removed) }
            }
            while kept.count < rows { kept.append(TerminalRow(cols)) }
            return kept.map(fit)
        }

        next.scrollback = scrollback
        let shift = max(0, cy + 1 - rows)
        // On the alternate screen the saved cursor is the primary's.
        let savedShift = alt == nil ? shift : max(0, savedCy + 1 - rows)
        next.grid = refit(grid, cy, alt == nil)
        if let alt { next.alt = refit(alt, savedCy, true) }
        if next.scrollback.count > AgentTerminalConfig.maxScrollback {
            next.scrollback.removeFirst(next.scrollback.count - AgentTerminalConfig.maxScrollback)
        }
        next.cx = min(cx, cols - 1)
        next.cy = cy - shift
        next.savedCx = min(savedCx, cols - 1)
        next.savedCy = min(max(0, savedCy - savedShift), rows - 1)
        next.fg = fg
        next.bg = bg
        next.bold = bold
        next.dim = dim
        next.inverse = inverse
        next.underline = underline
        next.wrap = wrap
        next.wrapPending = wrapPending && cols == self.cols
        next.cursorVisible = cursorVisible
        next.appCursorKeys = appCursorKeys
        next.bracketedPaste = bracketedPaste
        next.state = state
        next.link = link
        next.params = params
        next.intermediate = intermediate
        next.osc = osc
        next.oscLength = oscLength
        return next
    }

    public func feed(_ text: String) {
        for scalar in text.unicodeScalars {
            let rune = scalar.value
            let ch = Character(scalar)
            switch state {
            case .osc:
                if rune == 0x07 {
                    endOsc()
                    state = .ground
                } else if rune == 0x1b {
                    state = .oscEsc
                } else if oscLength <= AgentTerminalConfig.maxOscChars {
                    osc.unicodeScalars.append(scalar)
                    oscLength += scalar.utf16.count
                }
                continue
            case .oscEsc:
                if ch == "\\" {
                    endOsc()
                    state = .ground
                } else {
                    state = .osc
                }
                continue
            case .esc:
                state = .ground
                if ch == "[" {
                    state = .csi
                    params = ""
                    intermediate = ""
                } else if ch == "]" {
                    state = .osc
                    osc = ""
                    oscLength = 0
                } else if ch == "(" || ch == ")" || ch == "#" || ch == "%" {
                    state = .escCharset
                } else if ch == "7" {
                    savedCx = cx
                    savedCy = cy
                } else if ch == "8" {
                    cx = savedCx
                    cy = savedCy
                } else if ch == "D" {
                    newLine()
                } else if ch == "E" {
                    cx = 0
                    newLine()
                } else if ch == "M" {
                    if cy == scrollTop {
                        scrollDown(scrollTop, scrollBottom, 1)
                    } else {
                        cy = max(0, cy - 1)
                    }
                } else if ch == "c" {
                    reset()
                }
                continue
            case .escCharset:
                state = .ground
                continue
            case .csi:
                if rune >= 0x30 && rune <= 0x3f {
                    if ch == "?" || ch == "<" || ch == "=" || ch == ">" {
                        intermediate.append(ch)
                    } else {
                        params.append(ch)
                    }
                    continue
                }
                if rune >= 0x20 && rune <= 0x2f {
                    intermediate.append(ch)
                    continue
                }
                handleCsi(ch)
                state = .ground
                continue
            case .ground:
                break
            }
            if rune == 0x1b {
                state = .esc
            } else if rune == 0x0d {
                cx = 0
                wrapPending = false
            } else if rune == 0x0a || rune == 0x0b || rune == 0x0c {
                newLine()
                wrapPending = false
            } else if rune == 0x08 {
                cx = max(0, cx - 1)
                wrapPending = false
            } else if rune == 0x09 {
                cx = min(cols - 1, (cx / 8 + 1) * 8)
                wrapPending = false
            } else if rune >= 0x20 && rune != 0x7f {
                putChar(String(ch))
            }
        }
    }

    /// Plain-text URLs, found across rows the grid soft-wrapped, keyed by row
    /// index into `rows` and then column. A URL that wraps in a narrow pane
    /// cannot be selected out whole, and pasting the first rows of a sign-in
    /// URL is exactly how a browser ends up at "Missing code_challenge
    /// parameter". So each wrapped piece stands for the whole URL.
    public static func wrappedUrlTargets(_ rows: [TerminalRow]) -> [[Int: String]?] {
        var targets = [[Int: String]?](repeating: nil, count: rows.count)
        var start = 0
        while start < rows.count {
            var end = start
            while end < rows.count - 1 && rows[end].wrapped { end += 1 }
            var units: [UInt16] = []
            var where_: [(Int, Int)] = []
            for r in start...end {
                let cells = rows[r].cells
                for col in 0..<cells.count {
                    // One string index per cell: a URL is ASCII, so a wider
                    // character can stand in as a blank without shifting any
                    // match.
                    let cellUnits = Array(cells[col].ch.utf16)
                    units.append(cellUnits.count == 1 ? cellUnits[0] : 0x20)
                    where_.append((r, col))
                }
            }
            let joined = String(decoding: units, as: UTF16.self)
            if joined.contains("://") {
                let range = NSRange(location: 0, length: units.count)
                for match in plainUrlPattern.matches(in: joined, range: range) {
                    let found = (joined as NSString).substring(with: match.range)
                    let trimmed = urlTrailerPattern.stringByReplacingMatches(
                        in: found, range: NSRange(location: 0, length: (found as NSString).length), withTemplate: "")
                    let url = safeTerminalLink(trimmed)
                    let length = url.utf16.count
                    for i in 0..<length {
                        let (r, col) = where_[match.range.location + i]
                        var map = targets[r] ?? [:]
                        map[col] = url
                        targets[r] = map
                    }
                }
            }
            start = end + 1
        }
        return targets
    }

    /// Every link on screen, OSC 8 targets and plain URLs alike, in the order
    /// they appear and without repeats.
    public func links() -> [String] {
        let rows = allRows
        let wrapped = Self.wrappedUrlTargets(rows)
        var seen = Set<String>()
        var found: [String] = []
        for r in 0..<rows.count {
            let cells = rows[r].cells
            for col in 0..<cells.count {
                let link = cells[col].link.isEmpty ? (wrapped[r]?[col] ?? "") : cells[col].link
                if !link.isEmpty && seen.insert(link).inserted { found.append(link) }
            }
        }
        return found
    }
}

/// The bytes a named key sends, honouring DECCKM for the cursor keys.
public enum TerminalKey: String, CaseIterable, Sendable {
    case enter, backspace, tab, escape, up, down, right, left, home, end, pageUp, pageDown, delete, ctrlC
}

public func terminalKeyBytes(_ key: TerminalKey, appCursorKeys: Bool = false) -> String {
    let esc = "\u{1b}"
    // The cursor keys change their own prefix when the remote program asks
    // for application mode, which the provider CLIs' menus rely on.
    func cursor(_ letter: String) -> String { "\(esc)\(appCursorKeys ? "O" : "[")\(letter)" }
    switch key {
    case .enter: return "\r"
    case .backspace: return "\u{7f}"
    case .tab: return "\t"
    case .escape: return esc
    case .up: return cursor("A")
    case .down: return cursor("B")
    case .right: return cursor("C")
    case .left: return cursor("D")
    case .home: return cursor("H")
    case .end: return cursor("F")
    case .pageUp: return "\(esc)[5~"
    case .pageDown: return "\(esc)[6~"
    case .delete: return "\(esc)[3~"
    case .ctrlC: return "\u{03}"
    }
}

/// The byte a control chord sends, or "" when the chord has none.
public func terminalControlBytes(_ character: String) -> String {
    let units = Array(character.utf16)
    if units.count != 1 { return "" }
    let upper = Array(character.uppercased().utf16).first ?? 0
    if upper >= 0x41 && upper <= 0x5a {
        return String(decoding: [upper - 64], as: UTF16.self)
    }
    switch character {
    case " ": return "\u{00}"
    case "[": return "\u{1b}"
    case "\\": return "\u{1c}"
    case "]": return "\u{1d}"
    default: return ""
    }
}

public enum TerminalStatusLevel: String, Sendable {
    case pending, ok, error
}

/// A chunked UTF-8 decoder: a multi-byte character split across two output
/// frames still decodes as one; malformed bytes become U+FFFD.
struct ChunkedUtf8Decoder {
    private var pending: [UInt8] = []

    mutating func take(_ bytes: Data) -> String {
        pending.append(contentsOf: bytes)
        var cut = pending.count
        var index = pending.count - 1
        var back = 0
        while index >= 0 && back < 4 {
            let byte = pending[index]
            if byte & 0xC0 == 0x80 {
                index -= 1
                back += 1
                continue
            }
            let need = byte >= 0xF0 && byte < 0xF8 ? 4 : byte >= 0xE0 && byte < 0xF0 ? 3 : byte >= 0xC0 && byte < 0xE0 ? 2 : 1
            if need > 1 && pending.count - index < need { cut = index }
            break
        }
        let text = String(decoding: pending[0..<cut], as: UTF8.self)
        pending.removeFirst(cut)
        return text
    }
}

@MainActor
public protocol StreamedTerminal: AnyObject, Sendable {
    var screen: TerminalScreen { get }
    var revision: Int { get }
    var statusText: String { get }
    var statusLevel: TerminalStatusLevel { get }
    var closed: Bool { get }
    var exitCode: Int? { get }
    func start(cols: Int, rows: Int)
    func resize(cols: Int, rows: Int)
    func input(_ text: String)
    func paste(_ text: String)
    func key(_ key: TerminalKey)
}

/// One streamed login session on one node.
///
/// Owns the channel it is given: `close()` tells the node to end the session
/// and drops the socket, and nothing survives it on either side. A reconnect
/// in between (the phone went to the browser to finish the sign-in and the OS
/// took the socket) costs a redraw, not the sign-in: the session re-opens
/// under the same id and the node replays what was drawn meanwhile.
@MainActor
@Observable
public final class AgentLoginTerminal: StreamedTerminal {
    public let node: String
    public let provider: String
    public let session: String
    @ObservationIgnored public let openTimeout: TimeInterval
    @ObservationIgnored public let inputFlush: TimeInterval
    @ObservationIgnored private let channel: AccountEventChannel

    /// Replaced on every (re)open and resize; drawn into in place otherwise
    /// (watch `revision` for that).
    public private(set) var screen = TerminalScreen(24, 80)
    /// Bumped once per output burst.
    public private(set) var revision = 0
    @ObservationIgnored private var cols = 80
    @ObservationIgnored private var rows = 24

    public private(set) var statusText = ""
    public private(set) var statusLevel = TerminalStatusLevel.pending

    /// True once the node has answered and the pty is running.
    public private(set) var live = false
    /// The login command's exit status once it has finished.
    public private(set) var exitCode: Int?
    public private(set) var closed = false

    @ObservationIgnored private var answered = false
    @ObservationIgnored private var started = false
    @ObservationIgnored private var openTimer: Task<Void, Never>?
    @ObservationIgnored private var inputTimer: Task<Void, Never>?
    @ObservationIgnored private var notifyTimer: Task<Void, Never>?
    @ObservationIgnored private var pendingInput = ""
    @ObservationIgnored private var decoder = ChunkedUtf8Decoder()
    @ObservationIgnored private var cachedLinks: [String]?
    @ObservationIgnored private var lastSignInLink = ""
    @ObservationIgnored public let listeners = Listeners()

    public init(
        node: String,
        provider: String,
        channel: AccountEventChannel,
        session: String? = nil,
        openTimeout: TimeInterval = AgentTerminalConfig.openTimeout,
        inputFlush: TimeInterval = AgentTerminalConfig.inputFlush
    ) {
        self.node = node
        self.provider = provider
        self.session = session ?? Hex.encode(Bytes.random(16))
        self.channel = channel
        self.openTimeout = openTimeout
        self.inputFlush = inputFlush
        statusText = "Connecting to \(node)…"
        channel.onConnected = { [weak self] in self?.onConnected() }
        channel.onFrame = { [weak self] frame in self?.onFrame(frame) }
        channel.onSignedOut = { [weak self] in
            self?.setStatus("Sign in to ForkMesh again to continue.", .error)
        }
    }

    /// Links on screen, newest last.
    public var links: [String] {
        if let cachedLinks { return cachedLinks }
        let found = screen.links()
        cachedLinks = found
        return found
    }

    /// The newest link the session has shown, the sign-in page. On a phone it
    /// is far easier to open from a button than to select out of a grid that
    /// wrapped it across a dozen rows. It outlives the rows that drew it: the
    /// CLI clears and redraws its prompt, and a button that came and went with
    /// every redraw would resize the grid under it each time.
    public var signInLink: String {
        let onScreen = links
        if let last = onScreen.last { lastSignInLink = last }
        return lastSignInLink
    }

    public var providerLabel: String { agentProviderLabel(provider) }

    public func start(cols: Int = 80, rows: Int = 24) {
        if started || closed { return }
        started = true
        self.cols = min(max(cols, 20), 500)
        self.rows = min(max(rows, 5), 200)
        screen = TerminalScreen(self.rows, self.cols)
        setStatus("Connecting to \(node)…", .pending)
        channel.start()
    }

    private func onConnected() {
        if closed { return }
        // Re-announce on every (re)connect: the node keeps the pty alive
        // across a reconnect and replays what was drawn while this side was
        // away.
        setStatus("Connecting to \(node)…", .pending)
        sendOpen()
    }

    @discardableResult
    private func send(_ frame: JSONObject) -> Bool {
        var out: JSONObject = ["type": "terminal", "session": .string(session), "node": .string(node)]
        for (key, value) in frame { out[key] = value }
        return channel.send(out)
    }

    private func sendOpen() {
        // The node answers a re-open by replaying everything the pty drew,
        // from the top, so the history is rebuilt from that rather than kept:
        // keeping it would show the whole sign-in twice after every trip to
        // the browser.
        screen = TerminalScreen(rows, cols)
        changed()
        let sent = send([
            "action": "open",
            "provider": .string(provider),
            "cols": .int(Int64(cols)),
            "rows": .int(Int64(rows)),
        ])
        if !sent { return }
        openTimer?.cancel()
        answered = false
        let timeout = openTimeout
        openTimer = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(timeout * 1_000_000_000))
            guard let self, !Task.isCancelled else { return }
            self.openTimer = nil
            if self.answered || self.closed { return }
            self.setStatus(
                "\(self.node) did not answer. It can only be reached here while it is online and configured to accept agent work from the website.",
                .error
            )
        }
    }

    private func onFrame(_ frame: JSONObject) {
        if closed || frame["type"] != .string("terminal") { return }
        if frame["session"] != .string(session) || frame["from"] != .string("node") { return }
        answered = true
        openTimer?.cancel()
        openTimer = nil
        switch frame["action"]?.stringValue {
        case "ready":
            live = true
            exitCode = nil
            setStatus("Live on \(node). Complete \(providerLabel)'s own sign-in below.", .ok)
        case "output":
            let data = frame["data"].map(lenientText) ?? ""
            guard let bytes = try? B64.decode(data) else { return }
            screen.feed(decoder.take(bytes))
            changed()
        case "error":
            let message = (frame["message"].map(lenientText) ?? "").dartTrimmed
            setStatus(message.isEmpty ? "The node refused the session." : message, .error)
        case "exit":
            let code = frame["code"]?.intValue ?? 0
            live = false
            exitCode = code
            setStatus(
                code == 0 ? "The sign-in command finished." : "The sign-in command exited with status \(code).",
                code == 0 ? .ok : .error
            )
        default:
            break
        }
    }

    /// Queue raw text for the pty. Batched so a burst costs one frame.
    public func input(_ text: String) {
        if text.isEmpty || closed { return }
        pendingInput += text
        // A paste can outrun one frame's budget; hand it over at once rather
        // than growing a buffer the relay would reject whole.
        if pendingInput.utf16.count > 4096 {
            inputTimer?.cancel()
            flushInput()
            return
        }
        if inputTimer == nil {
            let delay = inputFlush
            inputTimer = Task { [weak self] in
                try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
                guard let self, !Task.isCancelled else { return }
                self.flushInput()
            }
        }
    }

    /// Send a pasted or typed block, wrapped in bracketed-paste markers when
    /// the remote program asked for them, so it arrives as one paste rather
    /// than a run of keystrokes.
    public func paste(_ text: String) {
        if text.isEmpty { return }
        input(screen.bracketedPaste && text.utf16.count > 1 ? "\u{1b}[200~\(text)\u{1b}[201~" : text)
    }

    public func key(_ key: TerminalKey) {
        input(terminalKeyBytes(key, appCursorKeys: screen.appCursorKeys))
    }

    private func flushInput() {
        inputTimer = nil
        if pendingInput.isEmpty { return }
        let bytes = Array(pendingInput.utf8)
        pendingInput = ""
        var at = 0
        while at < bytes.count {
            let end = min(bytes.count, at + AgentTerminalConfig.maxInputBytesPerFrame)
            send(["action": "input", "data": .string(B64.encode(bytes[at..<end]))])
            at += AgentTerminalConfig.maxInputBytesPerFrame
        }
    }

    /// The view measured a different grid. The remote program redraws after
    /// SIGWINCH, so the grid is cut to size rather than reflowed: reflow would
    /// only invent a layout the program is about to replace.
    public func resize(cols: Int, rows: Int) {
        let cols = min(max(cols, 20), 500)
        let rows = min(max(rows, 5), 200)
        if cols == self.cols && rows == self.rows { return }
        self.cols = cols
        self.rows = rows
        if !started || closed { return }
        screen = screen.resized(rows, cols)
        send(["action": "resize", "cols": .int(Int64(cols)), "rows": .int(Int64(rows))])
        changed()
    }

    private func setStatus(_ text: String, _ level: TerminalStatusLevel) {
        if statusText == text && statusLevel == level { return }
        statusText = text
        statusLevel = level
        if !closed { listeners.notify() }
    }

    /// Output arrives in redraw bursts; repaint once per burst, not per frame.
    private func changed() {
        cachedLinks = nil
        if closed { return }
        if notifyTimer != nil { return }
        notifyTimer = Task { [weak self] in
            try? await Task.sleep(nanoseconds: 16_000_000)
            guard let self else { return }
            self.notifyTimer = nil
            if self.closed { return }
            // Seen on every burst, so a link a later burst clears is still
            // kept.
            _ = self.signInLink
            self.revision &+= 1
            self.listeners.notify()
        }
    }

    public func close() async {
        if closed { return }
        flushInput()
        send(["action": "close"])
        closed = true
        openTimer?.cancel()
        inputTimer?.cancel()
        notifyTimer?.cancel()
        // `send` writes from a task of its own; give the close frame a moment
        // to leave before the socket goes.
        try? await Task.sleep(nanoseconds: 50_000_000)
        await channel.dispose()
    }
}
