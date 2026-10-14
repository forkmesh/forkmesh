import Foundation

public struct DocsArea: Equatable, Sendable, Identifiable {
    public let key: String
    public let label: String
    public let blurb: String
    public var id: String { key }

    public init(key: String, label: String, blurb: String = "") {
        self.key = key
        self.label = label
        self.blurb = blurb
    }
}

public struct DocsPage: Equatable, Sendable, Identifiable {
    public let path: String
    public let title: String
    public let description: String
    public let area: String
    public let url: String
    public let asset: Bool
    public let markdown: String
    public var id: String { path }

    public init(path: String, title: String, description: String = "", area: String, url: String = "", asset: Bool = false, markdown: String) {
        self.path = path
        self.title = title
        self.description = description
        self.area = area
        self.url = url
        self.asset = asset
        self.markdown = markdown
    }
}

public struct DocsAuthor: Hashable, Sendable, Identifiable {
    public let login: String
    public let name: String
    public let profile: String
    public let projects: String
    public let website: String
    public let publicRepos: Int
    public let contributions: Int
    public var id: String { login }

    public var displayName: String { name == login ? name : "\(name) (@\(login))" }

    public var commitsLabel: String {
        switch contributions {
        case 0: return "No commits counted"
        case 1: return "1 commit"
        default: return "\(DocsLibrary.grouped(contributions)) commits"
        }
    }

    public var projectsLabel: String {
        switch publicRepos {
        case 0: return "Projects"
        case 1: return "1 project"
        default: return "\(DocsLibrary.grouped(publicRepos)) projects"
        }
    }
}

public struct DocsDependency: Hashable, Sendable, Identifiable {
    public let ecosystem: String
    public let name: String
    public let title: String
    public let version: String
    public let development: Bool
    public let usedBy: [String]
    public let description: String
    public let repository: String
    public let authors: [DocsAuthor]
    public let authorsNote: String
    public let fetched: String
    public var id: String { ecosystem + ":" + name }
    public var ecosystemLabel: String { DocsLibrary.ecosystemLabel(ecosystem) }

    public var versionLabel: String { version.isEmpty ? "unpinned" : version }

    public var authorsLabel: String {
        if authors.isEmpty { return authorsNote.isEmpty ? "Top authors not recorded yet." : authorsNote }
        return authors.count == 1 ? "1 top author" : "\(authors.count) top authors"
    }

    public var summary: String {
        var parts = [versionLabel]
        if development { parts.append("development") }
        parts.append(authors.isEmpty ? (repository.isEmpty ? "no GitHub repository" : "authors not recorded") : authorsLabel)
        return parts.joined(separator: " \u{00B7} ")
    }

    public var repositoryName: String {
        repository.hasPrefix(DocsLibrary.gitHub) ? String(repository.dropFirst(DocsLibrary.gitHub.count)) : repository
    }
}

public struct DocsDependencyGroup: Equatable, Sendable, Identifiable {
    public let ecosystem: String
    public let label: String
    public let dependencies: [DocsDependency]
    public var id: String { ecosystem }
}

public struct DocsHit: Equatable, Sendable, Identifiable {
    public let path: String
    public let title: String
    public let snippet: String
    public let score: Int
    public var id: String { path }
}

public enum DocsTarget: Equatable, Sendable {
    case anchor(String)
    case page(path: String, fragment: String)
    case external(URL)
}

public enum DocsLibraryError: Error, Equatable, Sendable {
    case unreadable(String)
    case wrongSchema(String)
    case empty
}

public struct DocsLibrary: Sendable {
    public static let schema = "forkmesh-docs-library-v1"
    public static let dependenciesArea = "dependencies"
    public static let gitHub = "https://github.com/"
    public static let linkScheme = "fmdocs"
    static let repositoryPrefix = "www/docs/"
    static let descriptionLimit = 180
    static let topAuthors = 3
    static let snippetWidth = 140

    public let webOrigin: String
    public let sourceTree: String
    public let areas: [DocsArea]
    public let pages: [DocsPage]
    public let dependencies: [DocsDependency]
    public let dependencyGroups: [DocsDependencyGroup]
    public let dependenciesGenerated: String
    let index: [String: Int]
    let searchTexts: [String]

    public init(data: Data) throws {
        let raw: RawLibrary
        do {
            raw = try JSONDecoder().decode(RawLibrary.self, from: data)
        } catch {
            throw DocsLibraryError.unreadable(String(describing: error))
        }
        guard raw.schema == Self.schema else { throw DocsLibraryError.wrongSchema(raw.schema) }
        let origin = Self.webLink(raw.webOrigin ?? "")
        webOrigin = origin.isEmpty ? "https://forkmesh.com" : (origin.hasSuffix("/") ? String(origin.dropLast()) : origin)
        let tree = Self.webLink(raw.sourceTree ?? "")
        sourceTree = tree.isEmpty ? "https://app.forkmesh.com/forkmesh/forkmesh/tree/main/" : (tree.hasSuffix("/") ? tree : tree + "/")
        var areaList: [DocsArea] = []
        for area in raw.areas ?? [] {
            let key = Self.simplified(area.key ?? "")
            guard !key.isEmpty, !areaList.contains(where: { $0.key == key }) else { continue }
            let label = Self.simplified(area.label ?? "")
            areaList.append(DocsArea(key: key, label: label.isEmpty ? key : label, blurb: Self.simplified(area.blurb ?? "")))
        }
        var pageList: [DocsPage] = []
        var positions: [String: Int] = [:]
        for page in raw.pages ?? [] {
            let path = (page.path ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            guard Self.isRelative(path), positions[path] == nil else { continue }
            let markdown = page.markdown ?? ""
            var title = Self.simplified(page.title ?? "")
            if title.isEmpty { title = String(path.split(separator: "/").last ?? Substring(path)) }
            let area = Self.simplified(page.area ?? "")
            positions[path] = pageList.count
            pageList.append(DocsPage(
                path: path, title: title, description: Self.simplified(page.description ?? ""),
                area: area.isEmpty ? "guides" : area, url: Self.webLink(page.url ?? ""), asset: page.asset ?? false,
                markdown: markdown))
        }
        guard !pageList.isEmpty else { throw DocsLibraryError.empty }
        areas = areaList
        pages = pageList
        index = positions
        searchTexts = pageList.map { $0.markdown.lowercased() }
        let deps = (raw.dependencies ?? []).compactMap(Self.dependency)
        dependencies = deps
        dependencyGroups = Self.groups(deps)
        dependenciesGenerated = Self.simplified(raw.dependenciesGenerated ?? "")
    }

    public static func ecosystemLabel(_ ecosystem: String) -> String {
        switch ecosystem {
        case "cpp": return "Desktop (C++)"
        case "python": return "Python"
        case "go": return "Go"
        case "npm": return "JavaScript (npm)"
        case "vendored": return "Bundled web libraries"
        case "maven": return "Android (Gradle)"
        case "swift": return "Swift packages"
        default: return ecosystem
        }
    }

    public static func groups(_ dependencies: [DocsDependency]) -> [DocsDependencyGroup] {
        var order: [String] = []
        var grouped: [String: [DocsDependency]] = [:]
        for dependency in dependencies {
            if grouped[dependency.ecosystem] == nil { order.append(dependency.ecosystem) }
            grouped[dependency.ecosystem, default: []].append(dependency)
        }
        return order.map { DocsDependencyGroup(ecosystem: $0, label: ecosystemLabel($0), dependencies: grouped[$0] ?? []) }
    }

    public var homePath: String {
        index["README.md"] != nil ? "README.md" : pages[0].path
    }

    public var dependenciesLabel: String {
        areas.first { $0.key == Self.dependenciesArea }?.label ?? "Dependencies"
    }

    public var documentAreas: [DocsArea] {
        areas.filter { $0.key != Self.dependenciesArea }
    }

    public func area(_ key: String) -> DocsArea? {
        areas.first { $0.key == key }
    }

    public func page(path: String) -> DocsPage? {
        index[path].map { pages[$0] }
    }

    public func pages(in area: String) -> [DocsPage] {
        pages.filter { $0.area == area }
    }

    public func previous(_ path: String) -> DocsPage? {
        guard let at = index[path], at > 0 else { return nil }
        return pages[at - 1]
    }

    public func next(_ path: String) -> DocsPage? {
        guard let at = index[path], at + 1 < pages.count else { return nil }
        return pages[at + 1]
    }

    public func sourceURL(_ repositoryPath: String) -> URL? {
        let encoded = repositoryPath.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? repositoryPath
        return URL(string: sourceTree + encoded)
    }

    public func search(_ query: String, limit: Int = 50) -> [DocsHit] {
        let tokens = Self.tokens(query)
        guard !tokens.isEmpty, limit > 0 else { return [] }
        let phrase = tokens.joined(separator: " ")
        var hits: [DocsHit] = []
        for (offset, page) in pages.enumerated() {
            let body = searchTexts[offset]
            let title = page.title.lowercased()
            let description = page.description.lowercased()
            let path = page.path.lowercased()
            var score = title.contains(phrase) ? 200 : 0
            var bodyToken = ""
            var matched = true
            for token in tokens {
                let count = Self.count(token, in: body, cap: 10)
                if title.contains(token) {
                    score += title.hasPrefix(token) ? 150 : 100
                } else if description.contains(token) {
                    score += 25
                } else if path.contains(token) {
                    score += 15
                } else if count == 0 {
                    matched = false
                    break
                }
                if count > 0 && bodyToken.isEmpty { bodyToken = token }
                score += count
            }
            guard matched else { continue }
            var snippet = body.range(of: phrase, options: .literal) != nil ? Self.snippet(page.markdown, needle: phrase) : ""
            if snippet.isEmpty && !bodyToken.isEmpty { snippet = Self.snippet(page.markdown, needle: bodyToken) }
            if snippet.isEmpty || snippet == page.title { snippet = page.description }
            hits.append(DocsHit(path: page.path, title: page.title, snippet: snippet, score: score))
        }
        let ranked = hits.enumerated().sorted { (a, b) in
            a.element.score != b.element.score ? a.element.score > b.element.score : a.offset < b.offset
        }
        return Array(ranked.prefix(limit).map(\.element))
    }

    public func searchDependencies(_ query: String) -> [DocsDependency] {
        Self.searchDependencies(dependencies, query: query)
    }

    public static func searchDependencies(_ dependencies: [DocsDependency], query: String) -> [DocsDependency] {
        let tokens = tokens(query)
        guard !tokens.isEmpty else { return dependencies }
        let phrase = tokens.joined(separator: " ")
        var scored: [(score: Int, offset: Int, dependency: DocsDependency)] = []
        for (offset, dependency) in dependencies.enumerated() {
            let title = dependency.title.lowercased()
            let name = dependency.name.lowercased()
            var fields = [title, name, dependency.description.lowercased(), dependency.ecosystemLabel.lowercased(), dependency.repositoryName.lowercased()]
            for author in dependency.authors {
                fields.append(author.name.lowercased())
                fields.append(author.login.lowercased())
            }
            let haystack = fields.joined(separator: "\n")
            guard tokens.allSatisfy({ haystack.contains($0) }) else { continue }
            var score = 0
            if title == phrase || name == phrase { score += 300 }
            if title.contains(phrase) || name.contains(phrase) { score += 100 }
            for token in tokens where title.hasPrefix(token) || name.hasPrefix(token) { score += 50 }
            scored.append((score, offset, dependency))
        }
        return scored.sorted { $0.score != $1.score ? $0.score > $1.score : $0.offset < $1.offset }.map(\.dependency)
    }

    public func resolve(from path: String, href: String) -> DocsTarget? {
        let source = href.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !source.isEmpty else { return nil }
        if source.hasPrefix("#") {
            let fragment = String(source.dropFirst())
            return .anchor(fragment.removingPercentEncoding ?? fragment)
        }
        if source.hasPrefix("//") {
            return Self.makeURL("https:" + source).map(DocsTarget.external)
        }
        if source.hasPrefix("/") {
            return Self.makeURL(webOrigin + source).map(DocsTarget.external)
        }
        if Self.hasScheme(source) {
            let scheme = source.prefix { $0 != ":" }.lowercased()
            guard scheme == "http" || scheme == "https" || scheme == "mailto" else { return nil }
            return Self.makeURL(source).map(DocsTarget.external)
        }
        let link = Self.repositoryLink(base: Self.repositoryPrefix + path, href: source)
        if let page = pagePath(forRepository: link.path) {
            return .page(path: page, fragment: link.fragment)
        }
        let encodedPath: String
        let suffix = link.fragment.isEmpty ? "" : "#" + (link.fragment.addingPercentEncoding(withAllowedCharacters: .urlFragmentAllowed) ?? link.fragment)
        if link.path.hasPrefix("www/public/") {
            let rest = String(link.path.dropFirst("www/public/".count))
            encodedPath = webOrigin + "/" + (rest.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? rest)
        } else {
            encodedPath = sourceTree + (link.path.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? link.path)
        }
        return URL(string: encodedPath + suffix).map(DocsTarget.external)
    }

    public func linkURL(from path: String, href: String) -> URL? {
        resolve(from: path, href: href).flatMap(Self.url)
    }

    public static func url(for target: DocsTarget) -> URL? {
        var components = URLComponents()
        components.scheme = linkScheme
        switch target {
        case let .external(url):
            return url
        case let .anchor(fragment):
            components.host = "anchor"
            components.queryItems = [URLQueryItem(name: "fragment", value: fragment)]
        case let .page(path, fragment):
            components.host = "page"
            components.queryItems = [URLQueryItem(name: "path", value: path), URLQueryItem(name: "fragment", value: fragment)]
        }
        return components.url
    }

    public static func target(for url: URL) -> DocsTarget? {
        let scheme = url.scheme?.lowercased() ?? ""
        if scheme == "http" || scheme == "https" || scheme == "mailto" { return .external(url) }
        guard scheme == linkScheme, let components = URLComponents(url: url, resolvingAgainstBaseURL: false) else { return nil }
        let items = components.queryItems ?? []
        let fragment = items.first { $0.name == "fragment" }?.value ?? ""
        switch components.host {
        case "anchor":
            return .anchor(fragment)
        case "page":
            guard let path = items.first(where: { $0.name == "path" })?.value, !path.isEmpty else { return nil }
            return .page(path: path, fragment: fragment)
        default:
            return nil
        }
    }

    func pagePath(forRepository repositoryPath: String) -> String? {
        if repositoryPath == "www/docs" {
            return index["README.md"] != nil ? "README.md" : nil
        }
        guard repositoryPath.hasPrefix(Self.repositoryPrefix) else { return nil }
        let inner = String(repositoryPath.dropFirst(Self.repositoryPrefix.count))
        if index[inner] != nil { return inner }
        let readme = inner + "/README.md"
        return index[readme] != nil ? readme : nil
    }

    static func repositoryLink(base: String, href: String) -> (path: String, fragment: String) {
        var target = href
        var fragment = ""
        if let hash = target.firstIndex(of: "#") {
            let raw = String(target[target.index(after: hash)...])
            fragment = raw.removingPercentEncoding ?? raw
            target = String(target[..<hash])
        }
        if let query = target.firstIndex(of: "?") {
            target = String(target[..<query])
        }
        target = target.removingPercentEncoding ?? target
        var parts = base.split(separator: "/", omittingEmptySubsequences: true).map(String.init)
        if !parts.isEmpty { parts.removeLast() }
        for part in target.split(separator: "/", omittingEmptySubsequences: true) {
            if part == "." { continue }
            if part == ".." {
                if !parts.isEmpty { parts.removeLast() }
            } else {
                parts.append(String(part))
            }
        }
        return (parts.joined(separator: "/"), fragment)
    }

    static func hasScheme(_ source: String) -> Bool {
        guard let first = source.first, first.isASCII, first.isLetter else { return false }
        for ch in source.dropFirst() {
            if ch == ":" { return true }
            guard ch.isASCII, ch.isLetter || ch.isNumber || ch == "+" || ch == "." || ch == "-" else { return false }
        }
        return false
    }

    static func makeURL(_ text: String) -> URL? {
        if let url = URL(string: text) { return url }
        let allowed = CharacterSet.urlFragmentAllowed.union(CharacterSet(charactersIn: "#%"))
        return text.addingPercentEncoding(withAllowedCharacters: allowed).flatMap { URL(string: $0) }
    }

    public var onGitHub: Int { dependencies.filter { !$0.repository.isEmpty }.count }

    static func grouped(_ value: Int) -> String {
        let digits = Array(String(value))
        var out = ""
        for (offset, digit) in digits.enumerated() {
            if offset > 0 && (digits.count - offset) % 3 == 0 { out.append(",") }
            out.append(digit)
        }
        return out
    }

    static func tokens(_ query: String) -> [String] {
        query.lowercased().split(whereSeparator: { $0.isWhitespace }).map(String.init)
    }

    static func count(_ needle: String, in haystack: String, cap: Int) -> Int {
        var total = 0
        var from = haystack.startIndex
        while total < cap, let found = haystack.range(of: needle, options: .literal, range: from..<haystack.endIndex) {
            total += 1
            from = found.upperBound
        }
        return total
    }

    static func snippet(_ markdown: String, needle: String) -> String {
        guard let found = markdown.range(of: needle, options: .caseInsensitive) else { return "" }
        let start = markdown[..<found.lowerBound].lastIndex(of: "\n").map { markdown.index(after: $0) } ?? markdown.startIndex
        let end = markdown[found.lowerBound...].firstIndex(of: "\n") ?? markdown.endIndex
        var line = Substring(markdown[start..<end]).trimmingCharacters(in: .whitespaces)
        line = stripLead(line)
        line = DocsMarkdown.plain(line)
        guard line.count > snippetWidth else { return line }
        let chars = Array(line)
        let hit = line.range(of: needle, options: .caseInsensitive).map { line.distance(from: line.startIndex, to: $0.lowerBound) } ?? 0
        let from = max(0, min(hit - snippetWidth / 3, chars.count - snippetWidth))
        let body = String(chars[from..<min(chars.count, from + snippetWidth)]).trimmingCharacters(in: .whitespaces)
        return (from > 0 ? "\u{2026}" : "") + body + (from + snippetWidth < chars.count ? "\u{2026}" : "")
    }

    static func stripLead(_ line: String) -> String {
        var rest = Substring(line)
        let digits = rest.prefix { $0.isASCII && $0.isNumber }
        if !digits.isEmpty, rest.dropFirst(digits.count).first == "." {
            rest = rest.dropFirst(digits.count + 1)
        } else {
            rest = rest.drop { "#>|*+-".contains($0) }
        }
        return rest.trimmingCharacters(in: .whitespaces)
    }

    static func simplified(_ text: String) -> String {
        text.split(whereSeparator: { $0.isWhitespace }).joined(separator: " ")
    }

    static func truncated(_ text: String, limit: Int) -> String {
        var value = simplified(text)
        while value.hasSuffix(":") { value.removeLast() }
        guard value.count > limit else { return value }
        var cut = String(value.prefix(limit - 1))
        if let space = cut.lastIndex(of: " "), space > cut.startIndex {
            cut = String(cut[..<space])
        }
        while let last = cut.last, ",;:".contains(last) { cut.removeLast() }
        return cut + "\u{2026}"
    }

    static func isRelative(_ path: String) -> Bool {
        !path.isEmpty && !path.hasPrefix("/") && !path.contains("..") && !path.contains("://")
    }

    public static func webLink(_ value: String) -> String {
        let text = value.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let url = URL(string: text), let scheme = url.scheme?.lowercased(), scheme == "https" || scheme == "http",
            let host = url.host, !host.isEmpty
        else { return "" }
        return text
    }

    public static func gitHubLink(_ value: String) -> String {
        let link = webLink(value)
        return link.hasPrefix(gitHub) && link.count > gitHub.count ? link : ""
    }

    static func isLogin(_ login: String) -> Bool {
        guard let first = login.first, first.isASCII, first.isLetter || first.isNumber, login.count <= 39 else { return false }
        return login.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "-") }
    }

    static func dependency(_ raw: RawDependency) -> DocsDependency? {
        let ecosystem = simplified(raw.ecosystem ?? "")
        let name = simplified(raw.name ?? "")
        guard !ecosystem.isEmpty, !name.isEmpty else { return nil }
        let title = simplified(raw.title ?? "")
        var authors: [DocsAuthor] = []
        for author in raw.authors ?? [] {
            let login = simplified(author.login ?? "")
            guard isLogin(login) else { continue }
            let named = simplified(author.name ?? "")
            let profile = gitHubLink(author.profile ?? "")
            let projects = gitHubLink(author.projects ?? "")
            authors.append(DocsAuthor(
                login: login, name: named.isEmpty ? login : named,
                profile: profile.isEmpty ? gitHub + login : profile,
                projects: projects.isEmpty ? gitHub + login + "?tab=repositories" : projects,
                website: webLink(author.website ?? ""),
                publicRepos: max(0, author.publicRepos ?? 0), contributions: max(0, author.contributions ?? 0)))
            if authors.count == topAuthors { break }
        }
        let usedBy = (raw.usedBy ?? []).map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }.filter { isRelative($0) }
        return DocsDependency(
            ecosystem: ecosystem, name: name, title: title.isEmpty ? name : title, version: simplified(raw.version ?? ""),
            development: raw.development ?? false, usedBy: usedBy,
            description: truncated(raw.description ?? "", limit: descriptionLimit),
            repository: gitHubLink(raw.repository ?? ""), authors: authors, authorsNote: simplified(raw.authorsNote ?? ""),
            fetched: simplified(raw.fetched ?? ""))
    }
}

struct RawLibrary: Decodable {
    let schema: String
    let webOrigin: String?
    let sourceTree: String?
    let areas: [RawArea]?
    let pages: [RawPage]?
    let dependenciesGenerated: String?
    let dependencies: [RawDependency]?

    enum CodingKeys: String, CodingKey {
        case schema, webOrigin, sourceTree, areas, pages, dependenciesGenerated, dependencies
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        schema = (try? c.decode(String.self, forKey: .schema)) ?? ""
        webOrigin = try? c.decode(String.self, forKey: .webOrigin)
        sourceTree = try? c.decode(String.self, forKey: .sourceTree)
        areas = try? c.decode([Lenient<RawArea>].self, forKey: .areas).compactMap(\.value)
        pages = try? c.decode([Lenient<RawPage>].self, forKey: .pages).compactMap(\.value)
        dependenciesGenerated = try? c.decode(String.self, forKey: .dependenciesGenerated)
        dependencies = try? c.decode([Lenient<RawDependency>].self, forKey: .dependencies).compactMap(\.value)
    }
}

struct Lenient<Value: Decodable>: Decodable {
    let value: Value?

    init(from decoder: Decoder) throws {
        value = try? Value(from: decoder)
    }
}

struct RawArea: Decodable {
    let key: String?
    let label: String?
    let blurb: String?
}

struct RawPage: Decodable {
    let path: String?
    let title: String?
    let description: String?
    let area: String?
    let url: String?
    let asset: Bool?
    let markdown: String?
}

struct RawAuthor: Decodable {
    let login: String?
    let name: String?
    let profile: String?
    let projects: String?
    let website: String?
    let publicRepos: Int?
    let contributions: Int?

    enum CodingKeys: String, CodingKey {
        case login, name, profile, projects, website, publicRepos, contributions
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        login = try? c.decode(String.self, forKey: .login)
        name = try? c.decode(String.self, forKey: .name)
        profile = try? c.decode(String.self, forKey: .profile)
        projects = try? c.decode(String.self, forKey: .projects)
        website = try? c.decode(String.self, forKey: .website)
        publicRepos = try? c.decode(Int.self, forKey: .publicRepos)
        contributions = try? c.decode(Int.self, forKey: .contributions)
    }
}

struct RawDependency: Decodable {
    let ecosystem: String?
    let name: String?
    let title: String?
    let version: String?
    let development: Bool?
    let usedBy: [String]?
    let description: String?
    let repository: String?
    let authors: [RawAuthor]?
    let authorsNote: String?
    let fetched: String?

    enum CodingKeys: String, CodingKey {
        case ecosystem, name, title, version, development, usedBy, description, repository, authors, authorsNote, fetched
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        ecosystem = try? c.decode(String.self, forKey: .ecosystem)
        name = try? c.decode(String.self, forKey: .name)
        title = try? c.decode(String.self, forKey: .title)
        version = try? c.decode(String.self, forKey: .version)
        development = try? c.decode(Bool.self, forKey: .development)
        usedBy = try? c.decode([String].self, forKey: .usedBy)
        description = try? c.decode(String.self, forKey: .description)
        repository = try? c.decode(String.self, forKey: .repository)
        authors = try? c.decode([Lenient<RawAuthor>].self, forKey: .authors).compactMap(\.value)
        authorsNote = try? c.decode(String.self, forKey: .authorsNote)
        fetched = try? c.decode(String.self, forKey: .fetched)
    }
}
