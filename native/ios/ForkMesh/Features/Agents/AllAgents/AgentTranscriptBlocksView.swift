import FMApi
import FMCode
import FMControl
import SwiftUI
import UIKit

struct AgentTranscriptBlocksView: View {
    let blocks: [TranscriptBlock]
    var repo: RepoRef? = nil

    var body: some View {
        LazyVStack(alignment: .leading, spacing: FmSpace.x2) {
            ForEach(blocks.indices, id: \.self) { (index: Int) in
                TranscriptBlockRow(block: blocks[index])
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .id(index)
            }
        }
        .environment(\.referenceRepo, repo)
        .environment(\.openURL, referenceOpener)
    }
}

struct TranscriptBlockRow: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.referenceRepo) private var referenceRepo
    let block: TranscriptBlock

    var body: some View {
        switch block {
        case .status(let lines):
            VStack(alignment: .leading, spacing: 2) {
                ForEach(lines.indices, id: \.self) { (index: Int) in
                    HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                        Circle().fill(fm.textTertiary).frame(width: 4, height: 4)
                        Text(lines[index])
                            .fmText(.bodySmall, color: fm.textTertiary)
                            .lineLimit(3)
                    }
                }
            }
        case .error(let text):
            HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                FmIcon("error_outline", size: 14, color: fm.danger)
                Text(text)
                    .fmText(.bodySmall, color: fm.danger)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .fmMessageTools(text)
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(fm.danger.opacity(0.08), in: RoundedRectangle(cornerRadius: FmRadius.md))
        case .prompt(let text):
            TranscriptYouBubble(label: "You", text: text)
        case .steer(let from, let text):
            if TranscriptBlock.steerFromYou(from) {
                TranscriptYouBubble(label: "You · \(from)", text: text)
            } else {
                TranscriptPeerBubble(from: from, text: text)
            }
        case .tools(let calls):
            TranscriptToolRun(calls: calls)
        case .heading(let level, let text):
            transcriptInline(text, fm, referenceRepo)
                .fmText(level <= 1 ? .titleMedium : level == 2 ? .titleSmall : .bodyMedium, weight: level > 2 ? FmType.emphasis : nil)
                .padding(.top, 2)
        case .bullet(let marker, let depth, let text):
            HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
                Text(marker)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                transcriptInline(text, fm, referenceRepo)
                    .fmText(.bodyMedium)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(.leading, CGFloat(depth) * 14)
        case .code(let language, let text):
            TranscriptCodeBox(language: language, text: text)
        case .json(let text):
            TranscriptJsonBox(text: text)
        case .table(let table):
            TranscriptTableBox(table: table)
        case .task(let task):
            TranscriptTaskCard(task: task)
        case .rule:
            Divider()
        case .text(let text):
            transcriptInline(text, fm, referenceRepo)
                .fmText(.bodyMedium)
                .textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}

private struct TranscriptYouBubble: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.referenceRepo) private var referenceRepo
    let label: String
    let text: String

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(label)
                .fmText(.labelSmall, color: fm.accent, weight: FmType.emphasis)
            transcriptInline(text, fm, referenceRepo)
                .fmText(.bodyMedium)
                .textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, FmSpace.x2)
        .background(fm.accentSubtle, in: RoundedRectangle(cornerRadius: FmRadius.lg))
        .padding(.leading, FmSpace.x6)
        .frame(maxWidth: .infinity, alignment: .trailing)
    }
}

private struct TranscriptPeerBubble: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.referenceRepo) private var referenceRepo
    let from: String
    let text: String

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: FmSpace.x1) {
                FmIcon("forum_outlined", size: 12, color: fm.textSecondary)
                Text("From \(from)")
                    .fmText(.labelSmall, color: fm.textSecondary, weight: FmType.emphasis)
            }
            transcriptInline(text, fm, referenceRepo)
                .fmText(.bodyMedium)
                .textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, FmSpace.x2)
        .background(fm.bgOverlay, in: RoundedRectangle(cornerRadius: FmRadius.lg))
        .padding(.trailing, FmSpace.x6)
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

private struct TranscriptToolRun: View {
    @Environment(\.fmPalette) private var fm
    let calls: [TranscriptTool]
    @State private var open = false

    private var folds: Bool { calls.count > 3 }

    private var summary: String {
        var counts: [String: Int] = [:]
        var order: [String] = []
        for call in calls {
            if counts[call.name] == nil { order.append(call.name) }
            counts[call.name, default: 0] += 1
        }
        let top = order.enumerated().sorted { (counts[$0.element] ?? 0, -$0.offset) > (counts[$1.element] ?? 0, -$1.offset) }.prefix(3)
        return top.map { counts[$0.element, default: 0] > 1 ? "\($0.element) ×\(counts[$0.element, default: 0])" : $0.element }
            .joined(separator: " · ")
    }

    var body: some View {
        let shown = !folds || open ? calls : Array(calls.suffix(1))
        VStack(alignment: .leading, spacing: 0) {
            if folds {
                Button {
                    open.toggle()
                } label: {
                    HStack(spacing: 6) {
                        FmIcon(open ? "expand_less" : "expand_more", size: 14, color: fm.textTertiary)
                        Text("\(calls.count) tool calls · \(summary)")
                            .fmText(.bodySmall, color: fm.textTertiary)
                            .lineLimit(1)
                        Spacer(minLength: 0)
                    }
                    .padding(.horizontal, 10)
                    .padding(.vertical, 6)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                Divider().overlay(fm.border.opacity(0.5))
            }
            ForEach(shown.indices, id: \.self) { (index: Int) in
                if index > 0 { Divider().overlay(fm.border.opacity(0.5)).padding(.leading, 30) }
                TranscriptToolChip(call: shown[index])
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(fm.bgOverlay.opacity(0.55), in: RoundedRectangle(cornerRadius: FmRadius.md))
        .overlay(RoundedRectangle(cornerRadius: FmRadius.md).stroke(fm.border.opacity(0.5), lineWidth: 0.5))
    }
}

private struct TranscriptToolChip: View {
    @Environment(\.fmPalette) private var fm
    let call: TranscriptTool
    @State private var open = false

    private var icon: String {
        let name = call.name.lowercased()
        if name.hasPrefix("mcp__") { return "extension_outlined" }
        switch name {
        case "bash", "shell", "exec", "exec_command", "local_shell", "bashoutput", "killshell": return "terminal_rounded"
        case "read", "view", "notebookread": return "description_outlined"
        case "edit", "multiedit", "write", "apply_patch", "notebookedit": return "edit_outlined"
        case "grep", "glob", "search", "ls": return "search"
        case "webfetch", "websearch", "web_search": return "language"
        case "agent", "task": return "smart_toy_outlined"
        case "todowrite", "update_plan": return "task_alt"
        default: return "build_outlined"
        }
    }

    private var multiLine: Bool { call.detail.contains("\n") }

    private var oneLine: String {
        (TranscriptJson.readable(call.detail) ?? call.detail).split(separator: "\n").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
            .joined(separator: " ⏎ ")
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(alignment: .firstTextBaseline, spacing: 6) {
                FmIcon(icon, size: 13, color: fm.textTertiary)
                    .frame(width: 16)
                Text(call.name)
                    .fmMono(weight: .semibold, color: fm.textSecondary)
                    .lineLimit(1)
                    .layoutPriority(1)
                if !call.detail.isEmpty && !open {
                    Text(oneLine)
                        .fmMono(color: fm.textTertiary)
                        .lineLimit(1)
                        .truncationMode(multiLine ? .tail : .middle)
                }
                Spacer(minLength: 0)
                if multiLine || open {
                    FmIcon(open ? "expand_less" : "expand_more", size: 14, color: fm.textTertiary)
                }
            }
            if open {
                Text(TranscriptJson.readable(call.detail) ?? call.detail)
                    .fmMono(color: fm.textSecondary)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.leading, 22)
            }
        }
        .padding(.horizontal, FmSpace.x2)
        .padding(.vertical, 6)
        .frame(maxWidth: .infinity, alignment: .leading)
        .contentShape(Rectangle())
        .onTapGesture { if !call.detail.isEmpty { open.toggle() } }
    }
}

private struct TranscriptCodeBox: View {
    @Environment(\.fmPalette) private var fm
    let language: String
    let text: String

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            if !language.isEmpty {
                Text(language)
                    .fmMono(size: 10, color: fm.textTertiary)
                    .padding(.horizontal, 10)
                    .padding(.top, 6)
            }
            ScrollView(.horizontal, showsIndicators: false) {
                Text(text)
                    .fmMono(color: fm.textPrimary)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: true, vertical: true)
                    .padding(.horizontal, 10)
                    .padding(.vertical, FmSpace.x2)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(fm.bgOverlay, in: RoundedRectangle(cornerRadius: FmRadius.md))
    }
}

private struct TranscriptJsonBox: View {
    @Environment(\.fmPalette) private var fm
    let text: String
    @State private var open = false

    private var readable: String { TranscriptJson.readable(text) ?? text }
    private var lines: [Substring] { readable.split(separator: "\n", omittingEmptySubsequences: false) }

    var body: some View {
        let count = lines.count
        let folds = count > 12
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: FmSpace.x1) {
                Text("Details")
                    .fmMono(size: 10, color: fm.textTertiary)
                Spacer(minLength: 0)
                if folds {
                    Text(open ? "Collapse" : "Show all \(count) lines")
                        .fmText(.labelSmall, color: fm.accent)
                    FmIcon(open ? "expand_less" : "expand_more", size: 14, color: fm.accent)
                }
            }
            .padding(.horizontal, 10)
            .padding(.top, 6)
            .contentShape(Rectangle())
            .onTapGesture { if folds { open.toggle() } }
            VStack(alignment: .leading) {
                Text(!folds || open ? readable : lines.prefix(8).joined(separator: "\n") + "\n  …")
                    .fmMono(color: fm.textPrimary)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.horizontal, 10)
                    .padding(.vertical, FmSpace.x2)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(fm.bgOverlay, in: RoundedRectangle(cornerRadius: FmRadius.md))
    }
}

private struct TranscriptTableBox: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.referenceRepo) private var referenceRepo
    let table: TranscriptTable

    private static let widest: CGFloat = 260
    private static let padding: CGFloat = 10

    private func measure(_ text: String, bold: Bool) -> CGFloat {
        let plain = AgentTranscriptFormat.inline(text).map(\.text).joined()
        let font = UIFont.systemFont(ofSize: 12, weight: bold ? .semibold : .regular)
        let width = (plain as NSString).size(withAttributes: [.font: font]).width
        return min(Self.widest, ceil(width) + 4) + Self.padding * 2
    }

    private var widths: [CGFloat] {
        table.header.indices.map { column in
            var width = measure(table.header[column], bold: true)
            for row in table.rows where column < row.count {
                width = max(width, measure(row[column], bold: false))
            }
            return max(width, 36)
        }
    }

    private func alignment(_ column: Int) -> Alignment {
        switch column < table.align.count ? table.align[column] : .start {
        case .start: return .topLeading
        case .center: return .top
        case .end: return .topTrailing
        }
    }

    private func cell(_ text: String, column: Int, width: CGFloat, header: Bool) -> some View {
        transcriptInline(text, fm, referenceRepo)
            .fmText(.bodySmall, color: fm.textPrimary, weight: header ? FmType.emphasis : nil)
            .fixedSize(horizontal: false, vertical: true)
            .padding(.horizontal, Self.padding)
            .padding(.vertical, 6)
            .frame(width: width, alignment: alignment(column))
            .frame(maxHeight: .infinity, alignment: alignment(column))
    }

    var body: some View {
        let widths = self.widths
        ScrollView(.horizontal, showsIndicators: false) {
            Grid(alignment: .topLeading, horizontalSpacing: 0, verticalSpacing: 0) {
                GridRow {
                    ForEach(table.header.indices, id: \.self) { (column: Int) in
                        cell(table.header[column], column: column, width: widths[column], header: true)
                            .background(fm.bgOverlay)
                    }
                }
                ForEach(table.rows.indices, id: \.self) { (row: Int) in
                    Divider().gridCellUnsizedAxes(.horizontal)
                    GridRow {
                        ForEach(table.header.indices, id: \.self) { (column: Int) in
                            cell(column < table.rows[row].count ? table.rows[row][column] : "", column: column, width: widths[column], header: false)
                        }
                    }
                }
            }
            .overlay(RoundedRectangle(cornerRadius: FmRadius.md).stroke(fm.border, lineWidth: 1))
            .clipShape(RoundedRectangle(cornerRadius: FmRadius.md))
        }
    }
}

private struct TranscriptTaskCard: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.referenceRepo) private var referenceRepo
    let task: TranscriptTask
    @State private var open = false
    @State private var parsed: [TranscriptBlock]?

    private var tone: Color {
        switch task.tone {
        case .done: return fm.success
        case .failed: return fm.danger
        case .running: return fm.warning
        }
    }

    private var icon: String {
        switch task.tone {
        case .done: return "check_circle_outline"
        case .failed: return "error_outline"
        case .running: return "timelapse"
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x2) {
            HStack(alignment: .top, spacing: FmSpace.x2) {
                FmIcon(icon, size: 18, color: tone)
                VStack(alignment: .leading, spacing: 2) {
                    Text("Task " + task.status.replacingOccurrences(of: "_", with: " "))
                        .fmText(.labelSmall, color: tone, weight: FmType.emphasis)
                    transcriptInline(task.summary, fm, referenceRepo)
                        .fmText(.bodyMedium, weight: FmType.emphasis)
                        .textSelection(.enabled)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            if !task.body.isEmpty {
                Button {
                    if parsed == nil { parsed = AgentTranscriptFormat.blocks(task.body) }
                    open.toggle()
                } label: {
                    HStack(spacing: FmSpace.x1) {
                        FmIcon(open ? "expand_less" : "expand_more", size: 16, color: fm.accent)
                        Text(open ? "Hide result" : "Show result")
                            .fmText(.labelMedium, color: fm.accent)
                    }
                }
                .buttonStyle(.plain)
                if open, let inner = parsed {
                    VStack(alignment: .leading, spacing: FmSpace.x2) {
                        ForEach(inner.indices, id: \.self) { (index: Int) in
                            AnyView(TranscriptBlockRow(block: inner[index]))
                        }
                    }
                }
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(tone.opacity(0.07), in: RoundedRectangle(cornerRadius: FmRadius.lg))
        .overlay(RoundedRectangle(cornerRadius: FmRadius.lg).stroke(tone.opacity(0.35), lineWidth: 1))
    }
}

func transcriptInline(_ text: String, _ fm: FmPalette, _ repo: RepoRef? = nil) -> Text {
    var out = AttributedString()
    for span in AgentTranscriptFormat.inline(text) {
        var piece = AttributedString(span.text)
        switch span.style {
        case .plain:
            piece = referenceAttributed(ReferenceLinks.runs(span.text, repo: repo, commits: repo != nil))
            for run in piece.runs where run.link != nil {
                piece[run.range].foregroundColor = fm.accent
            }
        case .bold:
            piece.inlinePresentationIntent = .stronglyEmphasized
        case .italic:
            piece.inlinePresentationIntent = .emphasized
        case .code:
            piece.font = .system(.footnote, design: .monospaced)
            piece.backgroundColor = fm.bgOverlay
        case .link:
            if let url = URL(string: span.url) { piece.link = url }
            piece.foregroundColor = fm.accent
        }
        out += piece
    }
    return Text(out)
}
