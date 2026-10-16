import FMTools
import SwiftUI

struct DocsBlockItem: Identifiable, Sendable {
    let id: Int
    let block: DocsBlock
}

struct DocsRendered: Sendable {
    let items: [DocsBlockItem]
    let blocks: [DocsBlock]
    let anchors: [String: Int]

    init(markdown: String) {
        let parsed: [DocsBlock] = DocsMarkdown.blocks(markdown)
        var map: [String: Int] = [:]
        var list: [DocsBlockItem] = []
        for (offset, block) in parsed.enumerated() {
            list.append(DocsBlockItem(id: offset, block: block))
            if case let .heading(_, _, anchor) = block, map[anchor] == nil {
                map[anchor] = offset
            }
        }
        items = list
        blocks = parsed
        anchors = map
    }

    func index(for fragment: String) -> Int? {
        DocsMarkdown.anchorMatching(fragment, in: blocks).flatMap { (anchor: String) -> Int? in anchors[anchor] }
    }
}

enum DocsText {
    static func attributed(_ text: String, library: DocsLibrary, path: String) -> AttributedString {
        var result: AttributedString = AttributedString()
        for run in DocsInline.runs(text) {
            var part: AttributedString = AttributedString(run.text)
            var intent: InlinePresentationIntent = []
            if run.bold { intent.insert(.stronglyEmphasized) }
            if run.italic { intent.insert(.emphasized) }
            if run.code { intent.insert(.code) }
            if run.strike { intent.insert(.strikethrough) }
            if !intent.isEmpty {
                part.inlinePresentationIntent = intent
            }
            if let href = run.link, let url = library.linkURL(from: path, href: href) {
                part.link = url
            }
            result.append(part)
        }
        return result
    }
}

struct DocsPageScreen: View {
    @Environment(\.fmPalette) private var fm
    @Environment(\.openURL) private var openURL

    let library: DocsLibrary
    @State private var path: String
    @State private var fragment: String
    @State private var rendered: DocsRendered?
    @State private var pushed: DocsPageLink?

    init(library: DocsLibrary, path: String, fragment: String = "") {
        self.library = library
        _path = State(initialValue: path)
        _fragment = State(initialValue: fragment)
    }

    private var page: DocsPage? { library.page(path: path) }

    var body: some View {
        ScrollViewReader { (proxy: ScrollViewProxy) in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: FmSpace.x3) {
                    breadcrumb
                        .id("docs-top")
                    if let rendered = rendered {
                        ForEach(rendered.items) { (item: DocsBlockItem) in
                            DocsBlockView(block: item.block, library: library, path: path)
                                .id(item.id)
                        }
                        footer
                    } else if page == nil {
                        FmEmptyState(icon: "book_outlined", title: "Page not found", message: "This page is not in the documentation library.")
                    } else {
                        FmCircularProgressIndicator(size: 20, strokeWidth: 2)
                            .frame(maxWidth: .infinity)
                            .padding(.top, FmSpace.x6)
                    }
                }
                .padding(FmSpace.x4)
                .frame(maxWidth: 820, alignment: .leading)
                .frame(maxWidth: .infinity)
            }
            .id(path)
            .tint(fm.accent)
            .environment(\.openURL, OpenURLAction { (url: URL) -> OpenURLAction.Result in
                MainActor.assumeIsolated { () -> OpenURLAction.Result in
                    open(url, proxy: proxy)
                }
            })
            .task(id: path) { await render(proxy: proxy) }
        }
        .background(fm.canvas.ignoresSafeArea())
        .navigationTitle(page?.title ?? "Docs")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            if let address = page?.url, let url = URL(string: address) {
                ToolbarItemGroup(placement: .primaryAction) {
                    ShareLink(item: url) {
                        Image(systemName: "square.and.arrow.up")
                    }
                    .accessibilityLabel("Share")
                    FmIconButton(icon: "open_in_new", label: "Open on web", onPressed: { openURL(url) })
                        .accessibilityIdentifier("docs-open-web")
                }
            }
        }
        .navigationDestination(item: $pushed) { (link: DocsPageLink) in
            DocsPageScreen(library: library, path: link.path, fragment: link.fragment)
                .shellPageClearance()
        }
        .accessibilityIdentifier("docs-page-screen")
    }

    @ViewBuilder private var breadcrumb: some View {
        if let page = page {
            let area: String = library.area(page.area)?.label ?? page.area
            Text("Docs \u{00B7} \(area)")
                .fmText(.labelSmall, color: fm.textTertiary)
                .accessibilityLabel("In \(area)")
        }
    }

    private var footer: some View {
        VStack(alignment: .leading, spacing: FmSpace.x3) {
            FmHairline()
                .padding(.top, FmSpace.x4)
            ViewThatFits(in: .horizontal) {
                HStack(alignment: .top, spacing: FmSpace.x3) {
                    neighbours
                }
                VStack(alignment: .leading, spacing: FmSpace.x3) {
                    neighbours
                }
            }
        }
    }

    @ViewBuilder private var neighbours: some View {
        if let previous = library.previous(path) {
            neighbour(label: "Previous", page: previous, icon: "chevron_left", leading: true)
        }
        if let next = library.next(path) {
            neighbour(label: "Next", page: next, icon: "chevron_right", leading: false)
        }
    }

    private func neighbour(label: String, page: DocsPage, icon: String, leading: Bool) -> some View {
        FmCard(onTap: { go(to: page.path) }) {
            HStack(spacing: FmSpace.x2) {
                if leading {
                    FmIcon(icon, size: 20, color: fm.textTertiary)
                }
                VStack(alignment: leading ? .leading : .trailing, spacing: 2) {
                    Text(label)
                        .fmText(.labelSmall, color: fm.textTertiary)
                    Text(page.title)
                        .fmText(.titleSmall)
                        .lineLimit(2)
                        .multilineTextAlignment(leading ? .leading : .trailing)
                }
                .frame(maxWidth: .infinity, alignment: leading ? .leading : .trailing)
                if !leading {
                    FmIcon(icon, size: 20, color: fm.textTertiary)
                }
            }
        }
        .frame(minWidth: 220)
        .accessibilityElement(children: .combine)
        .accessibilityLabel("\(label): \(page.title)")
        .accessibilityAddTraits(.isButton)
        .accessibilityIdentifier("docs-\(label.lowercased())")
    }

    private func go(to next: String) {
        FmHaptics.selection()
        rendered = nil
        fragment = ""
        path = next
    }

    private func render(proxy: ScrollViewProxy) async {
        let markdown: String = page?.markdown ?? ""
        let target: String = fragment
        if rendered == nil {
            let built: DocsRendered = await Task.detached(priority: .userInitiated) { () -> DocsRendered in
                DocsRendered(markdown: markdown)
            }.value
            if Task.isCancelled { return }
            rendered = built
        }
        guard !target.isEmpty else { return }
        try? await Task.sleep(for: .milliseconds(150))
        if Task.isCancelled { return }
        fragment = ""
        scroll(to: target, proxy: proxy)
    }

    private func scroll(to target: String, proxy: ScrollViewProxy) {
        guard let rendered = rendered, let index = rendered.index(for: target) else { return }
        withAnimation(.easeInOut(duration: 0.25)) {
            proxy.scrollTo(index, anchor: .top)
        }
    }

    private func open(_ url: URL, proxy: ScrollViewProxy) -> OpenURLAction.Result {
        guard let target = DocsLibrary.target(for: url) else {
            return .systemAction
        }
        switch target {
        case let .anchor(name):
            scroll(to: name, proxy: proxy)
            return .handled
        case let .page(next, name):
            if next == path {
                if name.isEmpty {
                    withAnimation(.easeInOut(duration: 0.25)) {
                        proxy.scrollTo("docs-top", anchor: .top)
                    }
                } else {
                    scroll(to: name, proxy: proxy)
                }
                return .handled
            }
            FmHaptics.tap()
            pushed = DocsPageLink(path: next, fragment: name)
            return .handled
        case let .external(address):
            return referenceOpenResult(address)
        }
    }
}

struct DocsBlockView: View {
    @Environment(\.fmPalette) private var fm

    let block: DocsBlock
    let library: DocsLibrary
    let path: String

    private func text(_ source: String) -> Text {
        Text(DocsText.attributed(source, library: library, path: path))
    }

    var body: some View {
        switch block {
        case let .heading(level, source, _):
            heading(level: level, source: source)
        case let .paragraph(source):
            text(source)
                .fmText(.bodyMedium)
                .fixedSize(horizontal: false, vertical: true)
        case let .listItem(ordered, number, depth, source):
            listItem(ordered: ordered, number: number, depth: depth, source: source)
        case let .quote(source):
            HStack(alignment: .top, spacing: FmSpace.x3) {
                RoundedRectangle(cornerRadius: 2)
                    .fill(fm.border)
                    .frame(width: 3)
                text(source)
                    .fmText(.bodyMedium, color: fm.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            .fixedSize(horizontal: false, vertical: true)
        case let .code(language, source):
            DocsCodeView(language: language, source: source)
        case let .table(header, rows):
            DocsTableView(header: header, rows: rows, library: library, path: path)
        case .rule:
            FmHairline()
                .padding(.vertical, FmSpace.x2)
        }
    }

    private func heading(level: Int, source: String) -> some View {
        let role: FmTextRole
        switch level {
        case 1: role = .titleLarge
        case 2: role = .titleMedium
        default: role = .titleSmall
        }
        return VStack(alignment: .leading, spacing: FmSpace.x1) {
            text(source)
                .fmText(role, color: level >= 4 ? fm.textSecondary : nil)
                .fixedSize(horizontal: false, vertical: true)
                .accessibilityAddTraits(.isHeader)
            if level <= 2 {
                FmHairline()
            }
        }
        .padding(.top, level <= 2 ? FmSpace.x3 : FmSpace.x1)
    }

    private func listItem(ordered: Bool, number: Int, depth: Int, source: String) -> some View {
        let bullets: [String] = ["\u{2022}", "\u{25E6}", "\u{25AA}"]
        let marker: String = ordered ? "\(number)." : bullets[depth % bullets.count]
        return HStack(alignment: .firstTextBaseline, spacing: FmSpace.x2) {
            Text(marker)
                .fmText(.bodyMedium, color: fm.textSecondary, tabular: true)
                .frame(minWidth: ordered ? 22 : 12, alignment: .trailing)
                .accessibilityHidden(!ordered)
            text(source)
                .fmText(.bodyMedium)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(.leading, CGFloat(min(depth, 6)) * FmSpace.x4)
    }
}

struct DocsCodeView: View {
    @Environment(\.fmPalette) private var fm

    let language: String
    let source: String

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            if !language.isEmpty {
                Text(language)
                    .fmText(.labelSmall, color: fm.textTertiary)
                    .padding(.horizontal, FmSpace.x3)
                    .padding(.top, FmSpace.x2)
            }
            ScrollView(.horizontal, showsIndicators: true) {
                Text(source)
                    .fmMono(size: 13)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: true, vertical: true)
                    .padding(FmSpace.x3)
            }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(fm.bgOverlay)
        .clipShape(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
        .accessibilityElement(children: .combine)
        .accessibilityLabel(language.isEmpty ? "Code" : "\(language) code")
        .accessibilityValue(source)
    }
}

struct DocsTableView: View {
    @Environment(\.fmPalette) private var fm
    @ScaledMetric(relativeTo: .body) private var charWidth: CGFloat = 7.2

    let header: [String]
    let rows: [[String]]
    let library: DocsLibrary
    let path: String

    private func widths() -> [CGFloat] {
        var longest: [Int] = header.map { (cell: String) -> Int in DocsMarkdown.plain(cell).count }
        for row in rows {
            for (column, cell) in row.enumerated() where column < longest.count {
                longest[column] = max(longest[column], DocsMarkdown.plain(cell).count)
            }
        }
        return longest.map { (count: Int) -> CGFloat in
            min(max(CGFloat(count) * charWidth + FmSpace.x4, 64), 300)
        }
    }

    var body: some View {
        let columns: [CGFloat] = widths()
        ScrollView(.horizontal, showsIndicators: true) {
            Grid(alignment: .topLeading, horizontalSpacing: 0, verticalSpacing: 0) {
                GridRow {
                    ForEach(Array(header.enumerated()), id: \.offset) { (pair: (offset: Int, element: String)) in
                        cell(pair.element, width: columns[pair.offset], bold: true)
                    }
                }
                .background(fm.bgOverlay)
                ForEach(Array(rows.enumerated()), id: \.offset) { (row: (offset: Int, element: [String])) in
                    Divider()
                        .gridCellUnsizedAxes(.horizontal)
                    GridRow {
                        ForEach(Array(row.element.enumerated()), id: \.offset) { (pair: (offset: Int, element: String)) in
                            cell(pair.element, width: columns[min(pair.offset, columns.count - 1)], bold: false)
                        }
                    }
                }
            }
            .overlay(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous).stroke(fm.border, lineWidth: 1))
            .clipShape(RoundedRectangle(cornerRadius: FmRadius.md, style: .continuous))
            .padding(.vertical, 1)
        }
    }

    private func cell(_ source: String, width: CGFloat, bold: Bool) -> some View {
        Text(DocsText.attributed(source, library: library, path: path))
            .fmText(.bodySmall, weight: bold ? .semibold : nil)
            .fixedSize(horizontal: false, vertical: true)
            .frame(width: width, alignment: .leading)
            .padding(.horizontal, FmSpace.x2)
            .padding(.vertical, FmSpace.x2)
    }
}
