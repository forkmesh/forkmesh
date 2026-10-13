import FMApi
import FMBase
import FMCode
import SwiftUI

struct CodeMapScreen: View {
    let repo: RepoRef
    let ref: String
    @State private var root: CodeMapNode?
    @State private var focusPath = ""
    @State private var error: String?
    @Environment(\.colorScheme) private var scheme

    static let palette: [Color] = [
        Color(red: 0.23, green: 0.53, blue: 0.90), Color(red: 0.10, green: 0.62, blue: 0.44),
        Color(red: 0.79, green: 0.52, blue: 0.00), Color(red: 0.00, green: 0.51, blue: 0.00),
        Color(red: 0.56, green: 0.52, blue: 0.91), Color(red: 0.90, green: 0.40, blue: 0.40),
        Color(red: 0.84, green: 0.32, blue: 0.51), Color(red: 0.85, green: 0.35, blue: 0.15),
        Color.gray,
    ]

    private var focus: CodeMapNode? {
        guard let root else { return nil }
        return focusPath.isEmpty ? root : root.find(focusPath) ?? root
    }

    var body: some View {
        VStack(spacing: 0) {
            if let focus {
                breadcrumb
                GeometryReader { proxy in
                    let cells = CodeMapLayout.cells(
                        focus, in: CodeMapRect(x: 0, y: 0, width: proxy.size.width, height: proxy.size.height), depth: 3)
                    ZStack(alignment: .topLeading) {
                        ForEach(cells) { (cell: CodeMapCell) in
                            cellView(cell)
                        }
                    }
                    .frame(width: proxy.size.width, height: proxy.size.height, alignment: .topLeading)
                }
                .padding(8)
                Text("\(focus.fileCount) files · \(CodeMapLayout.sizeLabel(focus.size))")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .padding(.bottom, 8)
            } else if let error {
                ContentUnavailableView {
                    Label("No code map", systemImage: "square.grid.3x3")
                } description: {
                    Text(error)
                } actions: {
                    FmMessageTools(error)
                }
            } else {
                ProgressView("Measuring the repository…").frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .navigationTitle("Code map")
        .navigationBarTitleDisplayMode(.inline)
        .task { await load() }
    }

    private var breadcrumb: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 4) {
                ForEach(CodeMapLayout.breadcrumb(focusPath)) { (item: CodeMapCrumb) in
                    Button(item.name) { focusPath = item.path }
                        .buttonStyle(.bordered)
                        .font(.caption.monospaced())
                }
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 6)
        }
    }

    private func cellView(_ cell: CodeMapCell) -> some View {
        let base = CodeMapScreen.palette[min(cell.colorSlot, CodeMapScreen.palette.count - 1)]
        let shade = min(0.55, Double(cell.depth - 1) * 0.16)
        let fill = scheme == .dark ? base.opacity(0.85 - shade) : base.opacity(0.75 - shade * 0.8)
        return Group {
            if cell.node.isFile {
                NavigationLink(value: Route.file(repo, ref: ref, path: cell.node.path)) {
                    cellBody(cell, fill: fill)
                }
                .buttonStyle(.plain)
            } else {
                Button {
                    focusPath = cell.node.path
                } label: {
                    cellBody(cell, fill: fill)
                }
                .buttonStyle(.plain)
            }
        }
        .frame(width: max(0, cell.rect.width - 1), height: max(0, cell.rect.height - 1))
        .offset(x: cell.rect.x, y: cell.rect.y)
        .accessibilityLabel("\(cell.node.path), \(CodeMapLayout.sizeLabel(cell.node.size))")
    }

    private func cellBody(_ cell: CodeMapCell, fill: Color) -> some View {
        ZStack(alignment: .topLeading) {
            Rectangle().fill(fill)
            Rectangle().strokeBorder(Color.black.opacity(0.15), lineWidth: 0.5)
            if cell.rect.width > 44 && cell.rect.height > 16 {
                Text(cell.node.name)
                    .font(.system(size: 10, weight: cell.node.isFile ? .regular : .semibold, design: .monospaced))
                    .foregroundStyle(.white)
                    .lineLimit(1)
                    .padding(2)
            }
        }
    }

    private func load() async {
        do {
            let node = try await ReviewEnv.settings(repo).sizes(ref: ref)
            if node.size <= 0 || node.children.isEmpty {
                error = "No file size data is available for this repository yet. It appears once an online mirror reports it."
            } else {
                root = node
            }
        } catch {
            self.error = dartErrorText(error)
        }
    }
}

struct CodeToolsSheet: View {
    let repo: RepoRef
    let ref: String
    let path: String
    let text: String
    @Environment(\.dismiss) private var dismiss
    @State private var mode = 0
    @State private var find = FindInText()
    @State private var query = ""
    @State private var caseSensitive = false
    @State private var wholeWord = false
    @State private var goTo = ""
    @State private var goToLine: Int?

    private var lines: [String] { text.components(separatedBy: "\n") }

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                Picker("Tool", selection: $mode) {
                    Text("Find").tag(0)
                    Text("Outline").tag(1)
                }
                .pickerStyle(.segmented)
                .padding()
                if mode == 0 { findView } else { outlineView }
            }
            .navigationTitle((path as NSString).lastPathComponent)
            .navigationBarTitleDisplayMode(.inline)
            .routeDestinations()
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Done") { dismiss() } }
                ToolbarItem(placement: .topBarTrailing) {
                    NavigationLink(value: Route.fileHistory(repo, ref: ref, path: path)) {
                        Image(systemName: "clock.arrow.circlepath")
                    }
                    .accessibilityLabel("File history")
                }
            }
        }
    }

    private var findView: some View {
        List {
            Section {
                TextField("Find in file", text: $query)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .onSubmit(runFind)
                Toggle("Match case", isOn: $caseSensitive).onChange(of: caseSensitive) { _, _ in runFind() }
                Toggle("Whole word", isOn: $wholeWord).onChange(of: wholeWord) { _, _ in runFind() }
                HStack {
                    Text(find.status).font(.caption).foregroundStyle(.secondary)
                    Spacer()
                    Button { find.previous() } label: { Image(systemName: "chevron.up") }
                        .accessibilityLabel("Previous match")
                    Button { find.next() } label: { Image(systemName: "chevron.down") }
                        .accessibilityLabel("Next match")
                }
                .buttonStyle(.borderless)
                HStack {
                    TextField("Go to line", text: $goTo).keyboardType(.numberPad)
                    Button("Go") {
                        if let line = GoToLine.parse(goTo, lineCount: lines.count) {
                            find.jump(toLine: line)
                            goToLine = line
                        }
                    }
                }
            }
            if let line = goToLine, !lines.isEmpty {
                Section("Line \(line)") { contextView(line) }
            }
            Section {
                ForEach(find.matches.prefix(300)) { (match: TextMatch) in
                    let current = find.currentMatch == match
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Line \(match.line)").font(.caption2).foregroundStyle(.secondary)
                        Text(line(match.line)).font(.caption.monospaced()).lineLimit(2)
                    }
                    .listRowBackground(current ? Color.accentColor.opacity(0.15) : nil)
                }
            }
        }
    }

    private var outlineView: some View {
        let symbols = CodeOutline.symbols(text)
        return List {
            if symbols.isEmpty {
                Text("No definitions found.").foregroundStyle(.secondary)
            }
            ForEach(symbols) { (symbol: CodeSymbol) in
                DisclosureGroup {
                    let refs = CodeOutline.references(to: symbol.name, in: text, excludingLine: symbol.line)
                    if refs.isEmpty {
                        Text("No other uses in this file.").font(.caption).foregroundStyle(.secondary)
                    }
                    ForEach(refs.prefix(50)) { (ref: CodeReference) in
                        HStack(alignment: .top) {
                            Text("\(ref.line)").font(.caption2.monospaced()).foregroundStyle(.secondary).frame(width: 36, alignment: .trailing)
                            Text(ref.text).font(.caption.monospaced()).lineLimit(2)
                            if ref.isCall { Image(systemName: "phone.arrow.up.right").font(.caption2).foregroundStyle(.secondary) }
                        }
                    }
                } label: {
                    HStack {
                        Image(systemName: symbol.kind == "type" ? "cube" : symbol.kind == "const" ? "character" : "function")
                            .foregroundStyle(.secondary)
                        Text(symbol.name).font(.footnote.monospaced())
                            .padding(.leading, CGFloat(symbol.depth) * 10)
                        Spacer()
                        Text("\(symbol.line)").font(.caption2.monospaced()).foregroundStyle(.secondary)
                    }
                }
            }
        }
    }

    private func line(_ number: Int) -> String {
        lines.indices.contains(number - 1) ? lines[number - 1].trimmingCharacters(in: .whitespaces) : ""
    }

    private func contextView(_ number: Int) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            ForEach(max(1, number - 3)...min(lines.count, number + 3), id: \.self) { (n: Int) in
                HStack(alignment: .top, spacing: 6) {
                    Text("\(n)").frame(width: 36, alignment: .trailing).foregroundStyle(.secondary)
                    Text(lines[n - 1]).fontWeight(n == number ? .semibold : .regular)
                }
                .font(.caption.monospaced())
            }
        }
    }

    private func runFind() {
        find.options = FindOptions(caseSensitive: caseSensitive, wholeWord: wholeWord)
        find.search(query, in: text)
    }
}

struct FileHistoryScreen: View {
    let repo: RepoRef
    let ref: String
    let path: String
    @State private var commits: [JSONObject] = []
    @State private var error: String?
    @State private var loaded = false

    var body: some View {
        List {
            Section {
                Text(path).font(.footnote.monospaced())
                Text("Recent commits on \(ref.isEmpty ? "the default branch" : ref). The relay's history is not filtered by path yet, so this lists the branch's latest commits.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            if let error { Text(error).foregroundStyle(.red).frame(maxWidth: .infinity, alignment: .leading).fmMessageTools(error) }
            ForEach(commits.indices, id: \.self) { (index: Int) in
                let commit = commits[index]
                let hash = commit.firstStr("hash", "sha")
                NavigationLink(value: Route.commit(repo, sha: hash)) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(commit.firstStr("subject", "message").split(separator: "\n").first.map(String.init) ?? "Commit")
                            .lineLimit(2)
                        Text("\(String(hash.prefix(7))) · \(commit.firstStr("author", "authorName"))")
                            .font(.caption.monospaced())
                            .foregroundStyle(.secondary)
                    }
                }
            }
            if loaded && commits.isEmpty && error == nil {
                Text("No commits reported.").foregroundStyle(.secondary)
            }
        }
        .navigationTitle("History")
        .navigationBarTitleDisplayMode(.inline)
        .task {
            do {
                commits = Array(try await ReviewEnv.settings(repo).history(ref: ref).prefix(60))
            } catch {
                self.error = dartErrorText(error)
            }
            loaded = true
        }
    }
}
