// The diff views: DiffStatSummary, DiffViewMenu, the inline diff preview and
// PatchFilesScreen. The review stream is DiffReviewStream.swift. Parsing,
// pairing, intra-line marks and highlighting are FMDiff's; the rows, notices
// and runs are FMCode's DiffViewing.swift and DiffReview.swift.

import FMApi
import FMBase
import FMCode
import FMDiff
import SwiftUI
import UIKit

// MARK: - DiffViewer (the inline preview)

/// A patch's coloured lines, at most 420pt tall, and the way
/// into the full viewer (PatchFilesScreen).
struct DiffViewer: View {
    @Environment(\.fmPalette) private var fm

    private let diff: String
    /// The full-screen viewer's title.
    private let title: String

    init(diff: String, title: String = "Diff") {
        self.diff = diff
        self.title = title
    }

    /// A parsed patch, shown from its files' own text.
    init(diff: [DiffFile], title: String = "Diff") {
        self.diff = diff.map(\.raw).joined(separator: "\n")
        self.title = title
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            NavigationLink {
                PatchFilesScreen(title: title, patch: diff)
                    .shellPageClearance()
            } label: {
                HStack(spacing: FmSpace.x2) {
                    FmIcon("open_in_full", size: 18, color: fm.accent)
                    Text("Open in diff viewer")
                        .fmText(.labelLarge, color: fm.accent)
                }
                .padding(.horizontal, FmSpace.x3)
                .frame(minHeight: 44)
            }
            .buttonStyle(.plain)
            .accessibilityIdentifier("open-diff-viewer")
            preview
        }
    }

    private var preview: some View {
        let lines: [String] = dartLines(diff)
        return ScrollView {
            LazyVStack(spacing: 0) {
                ForEach(lines.indices, id: \.self) { (index: Int) in
                    previewLine(lines[index])
                }
            }
        }
        .frame(maxHeight: 420)
        .fixedSize(horizontal: false, vertical: lines.count < 20)
        .background(fm.bgBase)
        .clipShape(FmRoundedShape(radius: FmRadius.lg))
    }

    private func previewLine(_ line: String) -> some View {
        let kind: PatchPreviewLineKind = PatchPreviewLineKind(line: line)
        return Text(line)
            .fmMono(size: FmType.meta, color: ink(kind), height: 1.35)
            .textSelection(.enabled)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, 12)
            .padding(.vertical, 3)
            .background(wash(kind))
    }

    private func ink(_ kind: PatchPreviewLineKind) -> Color {
        switch kind {
        case .added: return fm.success
        case .removed: return fm.danger
        case .hunk: return fm.accent
        case .context: return fm.textPrimary
        }
    }

    private func wash(_ kind: PatchPreviewLineKind) -> Color {
        switch kind {
        case .added: return fm.success.opacity(0.12)
        case .removed: return fm.danger.opacity(0.10)
        case .hunk: return fm.accent.opacity(0.10)
        case .context: return Color.clear
        }
    }
}

// MARK: - Summary and rows

/// DiffStatSummary: "+12 −3" and GitHub's five-square proportion bar.
struct DiffStatSummary: View {
    @Environment(\.fmPalette) private var fm

    let additions: Int
    let deletions: Int
    let showBar: Bool

    init(additions: Int, deletions: Int, showBar: Bool = true) {
        self.additions = additions
        self.deletions = deletions
        self.showBar = showBar
    }

    var body: some View {
        HStack(spacing: FmSpace.x1) {
            Text("+\(additions)")
                .fmMono(weight: FmType.emphasis, color: fm.success)
            Text("−\(deletions)")
                .fmMono(weight: FmType.emphasis, color: fm.danger)
            if showBar {
                HStack(spacing: 1.5) {
                    ForEach(squares.indices, id: \.self) { (index: Int) in
                        RoundedRectangle(cornerRadius: 1.5)
                            .fill(squareColor(squares[index]))
                            .frame(width: 7, height: 7)
                    }
                }
                .padding(.leading, FmSpace.x1)
            }
        }
    }

    private var squares: [DiffStatSquare] { diffStatBar(additions: additions, deletions: deletions) }

    private func squareColor(_ square: DiffStatSquare) -> Color {
        switch square {
        case .added: return fm.success
        case .removed: return fm.danger
        case .empty: return fm.bgOverlay
        }
    }
}

// MARK: - The menu

/// An extra entry of DiffViewMenu: a label, an SF Symbol and the action.
struct DiffMenuAction {
    let label: String
    let systemImage: String
    let action: () -> Void
}

/// DiffViewMenu: the layout switches and text size, plus `extra` entries.
struct DiffViewMenu: View {
    @Environment(\.fmPalette) private var fm

    private let extra: [DiffMenuAction]

    init(extra: [DiffMenuAction] = []) {
        self.extra = extra
    }

    var body: some View {
        let settings: DiffViewSettings = DiffViewSettings.shared
        Menu {
            Toggle("Side by side", isOn: Bindable(settings).split)
            Toggle("Wrap lines", isOn: Bindable(settings).wrap)
            Toggle("Syntax highlighting", isOn: Bindable(settings).syntax)
            Divider()
            Button(action: { settings.setFontSize(settings.fontSize - 1) }) {
                Label("Smaller text", systemImage: "textformat.size.smaller")
            }
            .disabled(!settings.canShrink)
            Button(action: { settings.setFontSize(settings.fontSize + 1) }) {
                Label("Larger text", systemImage: "textformat.size.larger")
            }
            .disabled(!settings.canGrow)
            if !extra.isEmpty {
                Divider()
                ForEach(extra.indices, id: \.self) { (index: Int) in
                    Button(action: extra[index].action) {
                        Label(extra[index].label, systemImage: extra[index].systemImage)
                    }
                }
            }
        } label: {
            FmIcon("tune", size: 22, color: fm.textPrimary)
                .frame(width: 40, height: 40)
        }
        .accessibilityLabel(Text("View options"))
        .accessibilityIdentifier("diff-view-menu")
    }
}

/// The clipboard and "$what copied".
@MainActor
func diffCopy(_ text: String, _ what: String) {
    collabCopy(text, label: what)
}

/// PatchFilesScreen: any unified diff (a commit's or a pull request's) in
/// the review stream.
struct PatchFilesScreen: View {
    @State private var model: DiffReviewModel

    private let title: String
    private let patch: String

    init(title: String, patch: String) {
        self.title = title
        self.patch = patch
        _model = State(initialValue: DiffReviewModel(
            files: UnifiedDiff.parse(patch), scope: DiffReviewModel.patchScope(patch),
            split: DiffViewSettings.shared.split, store: Services.get(KeyValueStore.self)))
    }

    var body: some View {
        let patch: String = self.patch
        DiffReviewStream(
            model: model,
            extra: [DiffMenuAction(label: "Copy patch", systemImage: "doc.on.doc") { diffCopy(patch, "Patch") }]
        ) {
            EmptyView()
        }
        .navigationTitle(title)
        .navigationBarTitleDisplayMode(.inline)
    }
}
