import Foundation

public struct AppChangelog: Codable, Equatable, Sendable {
    public struct Section: Codable, Equatable, Sendable, Identifiable {
        public var title: String
        public var items: [String]

        public var id: String { title }

        public init(title: String, items: [String]) {
            self.title = title
            self.items = items
        }

        public init(from decoder: Decoder) throws {
            let container = try decoder.container(keyedBy: CodingKeys.self)
            title = try container.decodeIfPresent(String.self, forKey: .title) ?? ""
            items = try container.decodeIfPresent([String].self, forKey: .items) ?? []
        }
    }

    public struct Entry: Codable, Equatable, Sendable, Identifiable {
        public var version: String
        public var build: String
        public var date: String
        public var title: String
        public var pitch: String
        public var sections: [Section]

        public var id: String { "\(version)-\(build)-\(date)-\(title)" }

        public var heading: String {
            if !title.isEmpty { return title }
            if !version.isEmpty || !build.isEmpty { return AppChangelog.label(version: version, build: build) }
            return formattedDate()
        }

        public func detail(locale: Locale = .current) -> String {
            var parts: [String] = []
            if !title.isEmpty, !version.isEmpty {
                parts.append(AppChangelog.label(version: version, build: build))
            }
            if !date.isEmpty, !title.isEmpty || !version.isEmpty || !build.isEmpty {
                parts.append(formattedDate(locale: locale))
            }
            return parts.joined(separator: " · ")
        }

        public init(
            version: String, build: String, date: String = "", sections: [Section] = [],
            title: String = "", pitch: String = ""
        ) {
            self.version = version
            self.build = build
            self.date = date
            self.title = title
            self.pitch = pitch
            self.sections = sections
        }

        public init(from decoder: Decoder) throws {
            let container = try decoder.container(keyedBy: CodingKeys.self)
            version = try container.decodeIfPresent(String.self, forKey: .version) ?? ""
            build = try container.decodeIfPresent(String.self, forKey: .build) ?? ""
            date = try container.decodeIfPresent(String.self, forKey: .date) ?? ""
            title = try container.decodeIfPresent(String.self, forKey: .title) ?? ""
            pitch = try container.decodeIfPresent(String.self, forKey: .pitch) ?? ""
            sections = (try container.decodeIfPresent([Section].self, forKey: .sections) ?? [])
                .filter { (section: Section) -> Bool in !section.title.isEmpty && !section.items.isEmpty }
        }

        public func formattedDate(locale: Locale = .current) -> String {
            let parser: DateFormatter = DateFormatter()
            parser.locale = Locale(identifier: "en_US_POSIX")
            parser.timeZone = TimeZone(identifier: "UTC")
            parser.dateFormat = "yyyy-MM-dd"
            let output: DateFormatter = DateFormatter()
            output.locale = locale
            output.timeZone = TimeZone(identifier: "UTC")
            if let parsed: Date = parser.date(from: date) {
                output.dateStyle = .long
                output.timeStyle = .none
                return output.string(from: parsed)
            }
            parser.dateFormat = "yyyy-MM"
            guard date.count == 7, let month: Date = parser.date(from: date) else { return date }
            output.setLocalizedDateFormatFromTemplate("MMMMyyyy")
            return output.string(from: month)
        }
    }

    public var schema: Int
    public var entries: [Entry]

    public init(schema: Int = 1, entries: [Entry] = []) {
        self.schema = schema
        self.entries = entries
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        schema = try container.decodeIfPresent(Int.self, forKey: .schema) ?? 1
        entries = try container.decodeIfPresent([Entry].self, forKey: .entries) ?? []
    }

    public static func decode(_ data: Data) -> AppChangelog? {
        try? JSONDecoder().decode(AppChangelog.self, from: data)
    }

    public static func label(version: String, build: String) -> String {
        let shown: String = version.isEmpty ? "0.0.0" : version
        return build.isEmpty ? "Version \(shown)" : "Version \(shown) (\(build))"
    }

    public static func label(infoDictionary: [String: Any]?) -> String {
        label(
            version: infoDictionary?["CFBundleShortVersionString"] as? String ?? "",
            build: infoDictionary?["CFBundleVersion"] as? String ?? "")
    }

    public func forRunningBuild(version: String, build: String) -> AppChangelog {
        if let first: Entry = entries.first, first.build == build, !build.isEmpty {
            var copy: AppChangelog = self
            copy.entries[0].version = version.isEmpty ? first.version : version
            return copy
        }
        if !entries.isEmpty { return self }
        return AppChangelog(schema: schema, entries: [Entry(version: version, build: build)])
    }

    public static func load(data: Data?, infoDictionary: [String: Any]?) -> AppChangelog {
        let bundled: AppChangelog = data.flatMap { (bytes: Data) -> AppChangelog? in decode(bytes) } ?? AppChangelog()
        return bundled.forRunningBuild(
            version: infoDictionary?["CFBundleShortVersionString"] as? String ?? "",
            build: infoDictionary?["CFBundleVersion"] as? String ?? "")
    }

    public static func bundled(_ bundle: Bundle = .main) -> AppChangelog {
        let data: Data? = bundle.url(forResource: "Changelog", withExtension: "json")
            .flatMap { (url: URL) -> Data? in try? Data(contentsOf: url) }
        return load(data: data, infoDictionary: bundle.infoDictionary)
    }
}
