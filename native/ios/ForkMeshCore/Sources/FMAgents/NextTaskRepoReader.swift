import FMApi
import FMBase
import Foundation

public enum NextTaskRepoReader {
    public static let readmeMost = 2000
    public static let docMost = 1200
    public static let docBudget = 5000
    public static let docsRead = 4
    public static let indexMost = 40
    static let wanted = ["readme", "index", "roadmap", "todo", "plan", "architecture", "changelog", "agents"]
    static let textExtensions: Set<String> = ["md", "markdown", "txt", "rst", "adoc", ""]
    static let docFolders: Set<String> = ["docs", "doc", "documentation"]

    @MainActor
    public static func load(api: ApiService, owner: String, name: String) async -> NextTaskContext {
        async let listed: [Issue]? = try? api.issues(owner, name)
        async let history: [JSONObject]? = try? api.commits(owner, name)
        let root = (try? await api.tree(owner, name).entries) ?? []
        var folder: [RepoTreeEntry] = []
        if let docs = root.first(where: { $0.type == "dir" && docFolders.contains($0.name.lowercased()) }) {
            folder = (try? await api.tree(owner, name, path: docs.path).entries) ?? []
        }
        let picked = pick(root: root, docs: folder)
        let blobs = await api.blobs(owner, name, picked)
        let issues = await listed ?? []
        return NextTaskContext(
            repo: "\(owner)/\(name)", issues: NextTaskContext.issues(issues), closed: NextTaskContext.closed(issues),
            commits: NextTaskContext.commits(await history ?? []),
            docs: documents(picked.compactMap { (path: String) -> (String, String)? in
                guard let blob = blobs[path] else { return nil }
                return (path, text(blob))
            }),
            docIndex: index(folder))
    }

    public static func pick(root: [RepoTreeEntry], docs: [RepoTreeEntry]) -> [String] {
        let ranked = (root.map { ($0, 0) } + docs.map { ($0, 1) }).compactMap { (entry: RepoTreeEntry, depth: Int) -> (String, Int, Int)? in
            guard let rank = rank(entry) else { return nil }
            return (entry.path.isEmpty ? entry.name : entry.path, rank, depth)
        }
        return ranked.sorted { ($0.1, $0.2, $0.0) < ($1.1, $1.2, $1.0) }.prefix(docsRead).map(\.0)
    }

    public static func index(_ docs: [RepoTreeEntry]) -> [String] {
        docs.filter { $0.type == "dir" || isText($0.name) }
            .map { $0.type == "dir" ? "\($0.name)/" : $0.name }
            .sorted()
            .prefix(indexMost).map { $0 }
    }

    public static func documents(_ files: [(path: String, text: String)]) -> [NextTaskDoc] {
        var left = docBudget
        var out: [NextTaskDoc] = []
        for file in files where left > 200 {
            let most = min(left, isReadme(file.path) ? readmeMost : docMost)
            let text = excerpt(file.text, most)
            if text.isEmpty { continue }
            out.append(NextTaskDoc(path: file.path, text: text))
            left -= text.count
        }
        return out
    }

    public static func excerpt(_ text: String, _ most: Int) -> String {
        var lines: [String] = []
        var fenced = false
        for raw in text.components(separatedBy: .newlines) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.hasPrefix("```") || line.hasPrefix("~~~") {
                fenced.toggle()
                continue
            }
            if fenced || line.isEmpty || line.hasPrefix("<") || line.hasPrefix("[![") || line.hasPrefix("![") { continue }
            if line.hasPrefix("|") && line.allSatisfy({ "|-: ".contains($0) }) { continue }
            lines.append(line)
        }
        return NextTaskPrompt.clip(lines.joined(separator: "\n"), most)
    }

    static func text(_ blob: RepoBlob) -> String {
        guard blob.encoding.lowercased() == "base64" else { return blob.content }
        guard let data = Data(base64Encoded: blob.content.filter { !$0.isWhitespace }) else { return "" }
        return String(decoding: data, as: UTF8.self)
    }

    static func rank(_ entry: RepoTreeEntry) -> Int? {
        guard entry.type != "dir", isText(entry.name) else { return nil }
        let stem = entry.name.lowercased().split(separator: ".").first.map(String.init) ?? ""
        return wanted.firstIndex { stem == $0 || stem.hasPrefix($0 + "_") || stem.hasPrefix($0 + "-") }
    }

    static func isText(_ name: String) -> Bool {
        let parts = name.lowercased().split(separator: ".", omittingEmptySubsequences: false)
        return textExtensions.contains(parts.count > 1 ? String(parts.last ?? "") : "")
    }

    static func isReadme(_ path: String) -> Bool {
        (path.split(separator: "/").last ?? "").lowercased().hasPrefix("readme")
    }
}
