// RepoFileScreen: a file's preview (header, code lines, editor status bar, or a
// notice for unsupported files) with highlighted code (the tokens come from
// FMDiff's CodeHighlighter).

import FMApi
import FMBase
import FMCode
import FMDiff
import SwiftUI

struct RepoFileScreen: View {
    @Environment(\.fmPalette) private var fm
    @State private var model: RepoFileModel
    @State private var notice: FmNotice?
    @State private var showTools: Bool = false

    init(repo: Repository, path: String, ref: String = "", servedBlobSha: String = "") {
        _model = State(initialValue: RepoFileModel(
            client: RepoServices.client, repo: repo, path: path, ref: ref, servedBlobSha: servedBlobSha))
    }

    var body: some View {
        VStack(spacing: 0) {
            if notice != nil {
                FmNoticeLine($notice)
                    .padding(.horizontal, FmSpace.x4)
            }
            content
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .navigationTitle(model.fileName)
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItemGroup(placement: .topBarTrailing) {
                // Decided from the path so the action does not wait on the
                // blob; the editor still refuses binary or oversized bytes.
                if model.editable {
                    NavigationLink {
                        QuickEditScreen(
                            repo: model.repo, path: model.path, ref: model.ref,
                            servedBlobSha: model.servedBlobSha)
                            .shellPageClearance()
                    } label: {
                        FmIcon("edit_outlined", label: "Edit this file")
                    }
                }
                if model.editable && model.blob.data != nil {
                    Button {
                        showTools = true
                    } label: {
                        Image(systemName: "magnifyingglass")
                    }
                    .accessibilityLabel("Find and outline")
                }
                FmIconButton(icon: "copy_all_outlined", label: "Copy path", onPressed: {
                    notice = repoCopy("Path", model.path)
                })
                FmIconButton(icon: "link", label: "Copy raw URL", onPressed: {
                    notice = repoCopy("Raw URL", model.rawURL)
                })
            }
        }
        .sheet(isPresented: $showTools) {
            CodeToolsSheet(
                repo: RepoRef(owner: model.repo.owner, name: model.repo.name), ref: model.ref, path: model.path,
                text: model.loadedBlob.content)
        }
        .task {
            await model.start()
        }
    }

    @ViewBuilder private var content: some View {
        if model.blob.loading {
            FmSkeletonList(rows: 10, hasLeading: false, label: "Loading file")
                .frame(maxHeight: .infinity, alignment: .top)
        } else if let error = model.blob.error {
            VStack(spacing: FmSpace.x2) {
                Text(error)
                    .multilineTextAlignment(.center)
                FmMessageTools(error)
            }
            .padding(24)
        } else {
            RepoFilePreviewView(blob: model.loadedBlob, rawURL: model.rawURL)
        }
    }
}

struct RepoFilePreviewView: View {
    @Environment(\.fmPalette) private var fm

    let blob: RepoBlob
    let rawURL: String

    var body: some View {
        switch RepoFileModel.preview(for: blob) {
        case .image:
            RepoImagePreview(blob: blob, rawURL: rawURL)
        case .unsupported(let message):
            RepoUnsupportedPreview(path: blob.path, message: message, rawURL: rawURL, source: blob.source)
        case .text(let text):
            RepoCodePreviewView(path: blob.path, content: text, source: blob.source)
        }
    }
}

/// The image file preview: pinch to zoom between 0.5x and 5x.
struct RepoImagePreview: View {
    @Environment(\.fmPalette) private var fm
    @State private var scale: CGFloat = 1
    @State private var settledScale: CGFloat = 1

    let blob: RepoBlob
    let rawURL: String

    private var zoom: some Gesture {
        MagnifyGesture()
            .onChanged { (value: MagnifyGesture.Value) in
                scale = min(max(settledScale * value.magnification, 0.5), 5)
            }
            .onEnded { (_: MagnifyGesture.Value) in
                settledScale = scale
            }
    }

    var body: some View {
        VStack(spacing: 0) {
            RepoFilePreviewHeader(path: blob.path, source: blob.source)
            AsyncImage(url: URL(string: rawURL)) { (phase: AsyncImagePhase) in
                switch phase {
                case .success(let image):
                    image
                        .resizable()
                        .scaledToFit()
                        .scaleEffect(scale)
                        .gesture(zoom)
                case .failure(let error):
                    RepoUnsupportedPreview(
                        path: blob.path,
                        message: RepoFileModel.imageFailureMessage(error.localizedDescription),
                        rawURL: rawURL,
                        source: blob.source)
                default:
                    FmCircularProgressIndicator()
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .background(fm.canvas)
    }
}

struct RepoUnsupportedPreview: View {
    @Environment(\.fmPalette) private var fm

    let path: String
    let message: String
    let rawURL: String
    let source: String

    var body: some View {
        ScrollView {
            RepoInfoCard(title: RepoDisplay.baseName(path)) {
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    if RepoDisplay.sourceLabel(source) != nil {
                        RepoSourceChip(source: source)
                    }
                    Text(message)
                        .fmText(.bodyMedium, color: fm.textSecondary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .fmMessageTools(message)
                    RepoPreviewValue(label: "Path", value: path, accent: false)
                    if !rawURL.isEmpty {
                        RepoPreviewValue(label: "Raw URL", value: rawURL, accent: true)
                    }
                }
            }
            .padding(FmSpace.x5)
        }
        .background(fm.canvas)
    }
}

/// A labelled selectable value.
struct RepoPreviewValue: View {
    @Environment(\.fmPalette) private var fm

    let label: String
    let value: String
    let accent: Bool

    @ViewBuilder private var valueText: some View {
        if accent {
            Text(value)
                .fmText(.bodySmall, color: fm.accent)
        } else {
            Text(value)
                .fmMono(size: FmType.meta, color: fm.textSecondary)
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: FmSpace.x1) {
            Text(label)
                .font(.system(size: FmType.micro, weight: FmType.emphasis))
                .foregroundStyle(fm.textTertiary)
            valueText
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, FmSpace.x3)
                .padding(.vertical, FmSpace.x2)
                .background(fm.bgBase)
                .clipShape(FmRoundedShape(radius: FmRadius.sm))
                .overlay {
                    FmRoundedShape(radius: FmRadius.sm).strokeBorder(fm.border, lineWidth: 1)
                }
        }
    }
}

/// An editor-style tab with the full path.
struct RepoFilePreviewHeader: View {
    @Environment(\.fmPalette) private var fm

    let path: String
    let source: String

    var body: some View {
        HStack(spacing: FmSpace.x2) {
            HStack(spacing: FmSpace.x2) {
                FmIcon(RepoDisplay.fileGlyph(RepoDisplay.baseName(path)), size: 15, color: fm.accent)
                Text(path)
                    .fmText(.bodySmall, color: fm.textPrimary, weight: FmType.emphasis)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            .padding(.horizontal, FmSpace.x3)
            .padding(.vertical, 6)
            .background(fm.bgOverlay)
            .clipShape(FmRoundedShape(radius: FmRadius.md))
            if RepoDisplay.sourceLabel(source) != nil {
                RepoSourceChip(source: source)
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.vertical, FmSpace.x2)
        .frame(minHeight: 42)
        .background(fm.editorSurface)
        .overlay(alignment: .bottom) {
            FmHairline()
        }
    }
}

/// Colours of the highlighter's token kinds over FmTheme.
enum RepoCodeColors {
    static func color(_ kind: TokenKind, _ fm: FmPalette) -> Color {
        switch kind.themeColorName {
        case "codeKeyword": return fm.codeKeyword
        case "codeType": return fm.codeType
        case "codeString": return fm.codeString
        case "codeComment": return fm.codeComment
        case "codeNumber": return fm.codeNumber
        case "codeFunction": return fm.codeFunction
        default: return base(fm)
        }
    }

    /// `codeBaseColor`.
    static func base(_ fm: FmPalette) -> Color {
        return fm.dark ? Color(fmARGB: CodeHighlighter.baseColorDarkARGB) : fm.textPrimary
    }

    /// One highlighted line as an AttributedString in the mono face.
    static func line(_ pieces: [(text: String, kind: TokenKind)], size: CGFloat, fm: FmPalette) -> AttributedString {
        var out = AttributedString()
        for piece in pieces {
            var run = AttributedString(piece.text)
            run.foregroundColor = color(piece.kind, fm)
            var font: Font = FmFonts.monoFont(size: size, weight: piece.kind.isEmphasized ? FmType.emphasis : FmType.regular)
            if piece.kind.isItalic {
                font = font.italic()
            }
            run.font = font
            out.append(run)
        }
        return out
    }
}

/// Gutter, highlighted lines (built as they scroll in), the
/// tapped line highlighted, and the status bar.
struct RepoCodePreviewView: View {
    @Environment(\.fmPalette) private var fm
    @State private var preview: RepoCodePreview
    @State private var activeLine: Int = 0

    let source: String

    init(path: String, content: String, source: String) {
        _preview = State(initialValue: RepoCodePreview(path: path, content: content))
        self.source = source
    }

    private func codeLine(_ index: Int) -> some View {
        let active: Bool = index == activeLine
        let gutter: CGFloat = CGFloat(preview.gutterWidth)
        return HStack(alignment: .top, spacing: 0) {
            Text(String(index + 1))
                .font(FmFonts.monoFont(size: FmType.meta, weight: active ? FmType.emphasis : FmType.regular))
                .foregroundStyle(active ? fm.textSecondary : fm.textDisabled)
                .frame(width: gutter - FmSpace.x3, alignment: .trailing)
                .padding(.trailing, FmSpace.x3)
                .padding(.top, 2)
                .contentShape(Rectangle())
                .onTapGesture {
                    activeLine = index
                }
            Text(RepoCodeColors.line(preview.pieces(index), size: FmType.small, fm: fm))
                .fixedSize(horizontal: false, vertical: true)
                .textSelection(.enabled)
                .padding(EdgeInsets.fmLTRB(FmSpace.x3, 2, FmSpace.x4, 2))
                .frame(maxWidth: .infinity, alignment: .leading)
                .contentShape(Rectangle())
                .onTapGesture {
                    activeLine = index
                }
        }
        .background(alignment: .leading) {
            ZStack(alignment: .leading) {
                Rectangle().fill(active ? fm.editorActiveLine : fm.readingSurface)
                Rectangle()
                    .fill(active ? fm.editorActiveLine : fm.readingGutter)
                    .frame(width: gutter)
            }
        }
    }

    private var lines: some View {
        ScrollView(.vertical) {
            LazyVStack(alignment: .leading, spacing: 0) {
                ForEach(0..<preview.lineCount, id: \.self) { (index: Int) in
                    codeLine(index)
                }
            }
            .padding(.bottom, FmSpace.x5)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
    }

    private var statusBar: some View {
        FmBottomBar(
            actions: [
                AnyView(
                    Text(preview.languageLabel)
                        .fmText(.bodySmall, color: fm.textTertiary, weight: FmType.emphasis)),
            ],
            height: 40
        ) {
            Text(preview.statusLine(activeLine: activeLine + 1))
                .fmText(.bodySmall, color: fm.textTertiary, weight: FmType.emphasis)
                .lineLimit(1)
                .truncationMode(.tail)
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            RepoFilePreviewHeader(path: preview.path, source: source)
            lines
            statusBar
        }
        .background(fm.readingSurface)
    }
}
