import FMBase
import Foundation

public enum RSSDate {
    static let months = [
        "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec",
        "january", "february", "march", "april", "may", "june", "july", "august", "september", "october",
        "november", "december",
    ]
    static let days = ["mon", "tue", "wed", "thu", "fri", "sat", "sun"]
    static let zones: [String: Int] = [
        "UT": 0, "UTC": 0, "GMT": 0, "Z": 0, "AST": -400, "ADT": -300, "EST": -500, "EDT": -400,
        "CST": -600, "CDT": -500, "MST": -700, "MDT": -600, "PST": -800, "PDT": -700,
    ]

    static func daysFromCivil(_ year: Int, _ month: Int, _ day: Int) -> Int {
        let y = month <= 2 ? year - 1 : year
        let era = (y >= 0 ? y : y - 399) / 400
        let yoe = y - era * 400
        let mp = (month + 9) % 12
        let doy = (153 * mp + 2) / 5 + day - 1
        let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy
        return era * 146_097 + doe - 719_468
    }

    static func monthLength(_ year: Int, _ month: Int) -> Int {
        switch month {
        case 2: return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0 ? 29 : 28
        case 4, 6, 9, 11: return 30
        default: return 31
        }
    }

    static func millis(
        year: Int, month: Int, day: Int, hour: Int, minute: Int, second: Int, micro: Int = 0, offsetSeconds: Int
    ) -> Int64? {
        guard (1...9999).contains(year), (1...12).contains(month), day >= 1, day <= monthLength(year, month),
              (0...23).contains(hour), (0...59).contains(minute), (0...59).contains(second)
        else { return nil }
        let days = daysFromCivil(year, month, day)
        let seconds = Int64(days) * 86_400 + Int64(hour * 3600 + minute * 60 + second) - Int64(offsetSeconds)
        let value = Double(seconds) + Double(micro) / 1_000_000
        return max(0, Int64(value * 1000))
    }

    static func int(_ text: String) -> Int? {
        let trimmed = text.trimmingCharacters(in: .whitespaces)
        if trimmed.hasPrefix("+") { return Int(trimmed.dropFirst()) }
        return Int(trimmed)
    }

    static func rfc822(_ text: String) -> Int64? {
        var data = text.split(whereSeparator: { $0.isWhitespace }).map(String.init)
        guard !data.isEmpty else { return nil }
        if data[0].hasSuffix(",") || days.contains(data[0].lowercased()) {
            data.removeFirst()
        } else if let comma = data[0].lastIndex(of: ",") {
            data[0] = String(data[0][data[0].index(after: comma)...])
        }
        if data.count == 3 {
            let stuff = data[0].split(separator: "-", omittingEmptySubsequences: false).map(String.init)
            if stuff.count == 3 { data = stuff + Array(data[1...]) }
        }
        if data.count == 4 {
            let s = data[3]
            var cut = s.firstIndex(of: "+")
            if cut == nil { cut = s.firstIndex(of: "-") }
            if let cut, cut > s.startIndex {
                data = Array(data[0..<3]) + [String(s[..<cut]), String(s[cut...])]
            } else {
                data.append("")
            }
        }
        guard data.count >= 5 else { return nil }
        var dd = data[0], mm = data[1], yy = data[2], tm = data[3], tz = data[4]
        guard !dd.isEmpty, !mm.isEmpty, !yy.isEmpty else { return nil }
        mm = mm.lowercased()
        if !months.contains(mm) {
            (dd, mm) = (mm, dd.lowercased())
            if !months.contains(mm) { return nil }
        }
        var month = (months.firstIndex(of: mm) ?? 0) + 1
        if month > 12 { month -= 12 }
        if dd.hasSuffix(",") { dd.removeLast() }
        if let colon = yy.firstIndex(of: ":"), colon > yy.startIndex { (yy, tm) = (tm, yy) }
        if yy.hasSuffix(",") { yy.removeLast() }
        if let first = yy.first, !first.isNumber { (yy, tz) = (tz, yy) }
        if tm.hasSuffix(",") { tm.removeLast() }
        var parts = tm.split(separator: ":", omittingEmptySubsequences: false).map(String.init)
        if parts.count == 1 && parts[0].contains(".") {
            parts = parts[0].split(separator: ".", omittingEmptySubsequences: false).map(String.init)
        }
        let thh: String, tmm: String, tss: String
        switch parts.count {
        case 2: (thh, tmm, tss) = (parts[0], parts[1], "0")
        case 3: (thh, tmm, tss) = (parts[0], parts[1], parts[2])
        default: return nil
        }
        guard var year = int(yy), let day = int(dd), let hour = int(thh), let minute = int(tmm), let second = int(tss) else {
            return nil
        }
        if year < 100 { year += year > 68 ? 1900 : 2000 }
        var offset = 0
        let zone = tz.uppercased()
        if let known = zones[zone] {
            offset = known
        } else if let number = int(zone) {
            offset = number
        }
        let sign = offset < 0 ? -1 : 1
        let magnitude = abs(offset)
        let seconds = sign * ((magnitude / 100) * 3600 + (magnitude % 100) * 60)
        return millis(year: year, month: month, day: day, hour: hour, minute: minute, second: second, offsetSeconds: seconds)
    }

    static func digits(_ scalars: [Character], _ index: inout Int, _ count: Int) -> Int? {
        guard index + count <= scalars.count else { return nil }
        var value = 0
        for offset in 0..<count {
            guard let digit = scalars[index + offset].wholeNumberValue, scalars[index + offset].isASCII else { return nil }
            value = value * 10 + digit
        }
        index += count
        return value
    }

    static func iso(_ text: String) -> Int64? {
        let chars = Array(text)
        var i = 0
        guard let year = digits(chars, &i, 4) else { return nil }
        var compact = true
        if i < chars.count, chars[i] == "-" {
            compact = false
            i += 1
        }
        guard let month = digits(chars, &i, 2) else { return nil }
        if !compact {
            guard i < chars.count, chars[i] == "-" else { return nil }
            i += 1
        }
        guard let day = digits(chars, &i, 2) else { return nil }
        var hour = 0, minute = 0, second = 0, micro = 0, offset = 0
        if i < chars.count {
            guard chars[i] == "T" || chars[i] == "t" || chars[i] == " " else { return nil }
            i += 1
            guard let h = digits(chars, &i, 2) else { return nil }
            hour = h
            if i < chars.count, chars[i] == ":" || chars[i].isNumber {
                if chars[i] == ":" { i += 1 }
                guard let m = digits(chars, &i, 2) else { return nil }
                minute = m
                if i < chars.count, chars[i] == ":" || chars[i].isNumber {
                    if chars[i] == ":" { i += 1 }
                    guard let s = digits(chars, &i, 2) else { return nil }
                    second = s
                    if i < chars.count, chars[i] == "." || chars[i] == "," {
                        i += 1
                        var fraction = ""
                        while i < chars.count, chars[i].isASCII, chars[i].isNumber {
                            fraction.append(chars[i])
                            i += 1
                        }
                        guard !fraction.isEmpty else { return nil }
                        let six = String((fraction + "000000").prefix(6))
                        micro = Int(six) ?? 0
                    }
                }
            }
            if i < chars.count {
                if chars[i] == "Z" || chars[i] == "z" {
                    i += 1
                } else if chars[i] == "+" || chars[i] == "-" {
                    let sign = chars[i] == "-" ? -1 : 1
                    i += 1
                    guard let oh = digits(chars, &i, 2) else { return nil }
                    var om = 0
                    if i < chars.count, chars[i] == ":" { i += 1 }
                    if i < chars.count {
                        guard let value = digits(chars, &i, 2) else { return nil }
                        om = value
                    }
                    guard oh < 24, om < 60 else { return nil }
                    offset = sign * (oh * 3600 + om * 60)
                } else {
                    return nil
                }
            }
            guard i == chars.count else { return nil }
        }
        return millis(
            year: year, month: month, day: day, hour: hour, minute: minute, second: second, micro: micro,
            offsetSeconds: offset)
    }

    public static func parse(_ value: String) -> Int64 {
        let text = RSSText.clean(value, 80)
        if text.isEmpty { return 0 }
        if let stamp = rfc822(text) { return stamp }
        if let stamp = iso(text) { return stamp }
        let chars = Array(text)
        var i = 0
        if let year = digits(chars, &i, 4), i < chars.count, chars[i] == "-" {
            i += 1
            if let month = digits(chars, &i, 2), i < chars.count, chars[i] == "-" {
                i += 1
                if let day = digits(chars, &i, 2) {
                    return millis(year: year, month: month, day: day, hour: 0, minute: 0, second: 0, offsetSeconds: 0) ?? 0
                }
            }
        }
        return 0
    }
}

public struct RSSEnclosure: Codable, Equatable, Hashable, Sendable {
    public var url: String
    public var type: String
    public var length: Int64

    public init(url: String, type: String = "", length: Int64 = 0) {
        self.url = url
        self.type = type
        self.length = length
    }

    public var isAudio: Bool { type.hasPrefix("audio") }
    public var isVideo: Bool { type.hasPrefix("video") }
    public var isImage: Bool { type.hasPrefix("image/") }
}

public struct RSSParsedItem: Equatable, Sendable {
    public var guid: String = ""
    public var title: String = ""
    public var url: String = ""
    public var externalUrl: String = ""
    public var author: String = ""
    public var published: Int64 = 0
    public var updated: Int64 = 0
    public var contentHtml: String = ""
    public var summaryHtml: String = ""
    public var commentsUrl: String = ""
    public var enclosures: [RSSEnclosure] = []
    public var images: [String] = []
    public var language: String = ""

    public init() {}

    public var articleGuid: String { RSSFeedParser.articleGuid(self) }
}

public struct RSSParsedFeed: Equatable, Sendable {
    public var title: String = ""
    public var siteUrl: String = ""
    public var iconUrl: String = ""
    public var description: String = ""
    public var language: String = ""
    public var format: String = ""
    public var items: [RSSParsedItem] = []

    public init() {}
}

public enum RSSFeedError: Error, Equatable, Sendable {
    case empty
    case notAFeed
    case unreadable

    public var code: String {
        switch self {
        case .empty: return "empty_feed"
        case .notAFeed: return "not_a_feed"
        case .unreadable: return "feed_unreadable"
        }
    }
}

public enum RSSFeedParser {
    public static let maxItems = 200

    public static func articleGuid(_ item: RSSParsedItem) -> String {
        var value = RSSText.clean(item.guid, 600)
        if value.isEmpty { value = RSSText.clean(item.url, 600) }
        if value.isEmpty {
            let stamp = item.published != 0 ? String(item.published) : ""
            value = RSSText.clean(RSSText.clean(item.title, 300) + "|" + stamp, 600)
        }
        return RSSText.strip(value, characters: ["|"])
    }

    static func text(_ element: RSSXMLElement?) -> String { element?.plain ?? "" }

    static func atomText(_ element: RSSXMLElement?, html: Bool = false) -> String {
        guard let element else { return "" }
        let kind = (element.get("type") ?? "text").lowercased()
        if kind == "xhtml" || kind == "application/xhtml+xml" {
            let holder = element.child("div") ?? element
            let content = holder.inner
            return html ? content : RSSHTML.plainText(content, limit: 1000)
        }
        if kind == "html" || kind == "text/html" {
            let content = element.children.isEmpty ? element.text : element.inner
            return html ? RSSText.strip(content) : RSSHTML.plainText(content, limit: 1000)
        }
        let value = text(element)
        if html { return RSSText.escape(value, quote: true).replacingOccurrences(of: "\n\n", with: "<br><br>") }
        return RSSText.clean(value, 1000)
    }

    static func rssTitle(_ element: RSSXMLElement?) -> String {
        guard let element else { return "" }
        let value = element.children.isEmpty ? text(element) : element.inner
        let lowered = value.lowercased()
        if value.contains("<"), lowered.contains("</") || lowered.range(of: "<br") != nil {
            return RSSHTML.plainText(value, limit: 1000)
        }
        return RSSText.clean(RSSEntities.decode(value), 1000)
    }

    static func enclosures(_ items: [(String?, String?, String?)]) -> [RSSEnclosure] {
        var result: [RSSEnclosure] = []
        for (url, kind, length) in items {
            let clean = RSSURL.http(url ?? "")
            if !clean.isEmpty && !result.contains(where: { $0.url == clean }) {
                let size = Int64(RSSText.strip(length ?? "")) ?? Int64(Double(RSSText.strip(length ?? "")) ?? 0)
                result.append(RSSEnclosure(url: clean, type: RSSText.clean(kind ?? "", 80).lowercased(), length: max(0, size)))
            }
        }
        return Array(result.prefix(4))
    }

    struct Media {
        var images: [String] = []
        var title = ""
        var descriptionHtml = ""
    }

    static func isImage(_ element: RSSXMLElement) -> Bool {
        if (element.get("medium") ?? "").lowercased() == "image" { return true }
        return (element.get("type") ?? "").lowercased().hasPrefix("image/")
    }

    static func media(_ element: RSSXMLElement, base: String, imageEnclosures: [String] = []) -> Media {
        var holders = [element]
        holders += element.all("media:group")
        var thumbnails: [String] = []
        var contents: [String] = []
        var media = Media()
        for holder in holders {
            thumbnails += holder.all("media:thumbnail").map { RSSURL.http($0.get("url") ?? "", base: base) }
            contents += holder.all("media:content").filter(isImage).map { RSSURL.http($0.get("url") ?? "", base: base) }
            if media.title.isEmpty { media.title = RSSText.clean(text(holder.child("media:title")), 1000) }
            if media.descriptionHtml.isEmpty, let node = holder.child("media:description") {
                let value = text(node)
                if !value.isEmpty {
                    media.descriptionHtml = (node.get("type") ?? "").lowercased() == "html" ? value : RSSHTML.plainToHTML(value)
                }
            }
        }
        for url in thumbnails + contents + imageEnclosures where !url.isEmpty && !media.images.contains(url) {
            media.images.append(url)
        }
        return media
    }

    static func language(_ value: String) -> String { RSSText.clean(value, 35) }

    static func author(_ raw: String) -> String {
        let trimmed = raw
        if let open = trimmed.firstIndex(of: "("), trimmed.hasSuffix(")") {
            let before = trimmed[..<open]
            let pieces = before.split(whereSeparator: { $0.isWhitespace })
            if pieces.count == 1, pieces[0].contains("@"), before.last?.isWhitespace == true {
                return RSSText.clean(String(trimmed[trimmed.index(after: open)..<trimmed.index(before: trimmed.endIndex)]), 200)
            }
        }
        return RSSText.clean(trimmed, 200)
    }

    static func rssItems(_ container: RSSXMLElement, base: String) -> [RSSParsedItem] {
        var items: [RSSParsedItem] = []
        for element in container.all("item").prefix(maxItems) {
            var item = RSSParsedItem()
            var link = RSSURL.http(text(element.child("link")), base: base)
            let guidNode = element.child("guid")
            let guid = text(guidNode)
            if link.isEmpty, !guid.isEmpty, (guidNode?.get("ispermalink") ?? "true").lowercased() != "false" {
                link = RSSURL.http(guid, base: base)
            }
            let content = element.child("content:encoded")?.inner ?? ""
            let description = element.child("description")
            let summary = description.map { $0.children.isEmpty ? $0.plain : $0.inner } ?? ""
            let about = element.get("about") ?? ""
            let rssEnclosures = element.all("enclosure")
            let imageEnclosures = rssEnclosures.filter { ($0.get("type") ?? "").lowercased().hasPrefix("image/") }
                .map { RSSURL.http($0.get("url") ?? "", base: base) }
            let extra = media(element, base: base, imageEnclosures: imageEnclosures)
            var attachments: [(String?, String?, String?)] = rssEnclosures.map { ($0.get("url"), $0.get("type"), $0.get("length")) }
            attachments += element.all("media:content").filter {
                let kind = $0.get("medium") ?? $0.get("type") ?? ""
                return kind.hasPrefix("audio") || kind.hasPrefix("video")
            }.map { ($0.get("url"), $0.get("type"), $0.get("filesize")) }
            item.guid = guid.isEmpty ? (link.isEmpty ? about : link) : guid
            item.title = rssTitle(element.child("title"))
            if item.title.isEmpty { item.title = extra.title }
            item.url = link.isEmpty ? RSSURL.http(about, base: base) : link
            item.author = author(text(element.child("dc:creator", "author", "itunes:author")))
            item.published = RSSDate.parse(text(element.child("pubdate", "dc:date", "date")))
            item.updated = RSSDate.parse(text(element.child("updated", "dc:modified")))
            item.contentHtml = content.isEmpty ? summary : content
            item.summaryHtml = content.isEmpty ? "" : summary
            if RSSText.strip(item.contentHtml).isEmpty { item.contentHtml = extra.descriptionHtml }
            item.commentsUrl = RSSURL.http(text(element.child("comments")), base: base)
            item.enclosures = enclosures(attachments)
            item.images = extra.images
            item.language = language(text(element.child("dc:language")))
            items.append(item)
        }
        return items
    }

    static func atomLink(_ element: RSSXMLElement, base: String, rel wanted: String = "alternate") -> String {
        var fallback = ""
        for link in element.all("link") {
            let href = RSSURL.http(link.get("href") ?? "", base: base)
            if href.isEmpty { continue }
            let kind = (link.get("rel") ?? "alternate").lowercased()
            guard kind == wanted else { continue }
            let type = link.get("type") ?? "text/html"
            if wanted != "alternate" || type.hasPrefix("text/html") || type.hasPrefix("application/xhtml") { return href }
            if fallback.isEmpty { fallback = href }
        }
        return fallback
    }

    static func atomFeed(_ root: RSSXMLElement, base: String) -> RSSParsedFeed {
        var feed = RSSParsedFeed()
        let site = atomLink(root, base: base)
        feed.title = atomText(root.child("title"))
        feed.siteUrl = site.isEmpty ? base : site
        feed.iconUrl = RSSURL.http(text(root.child("icon", "logo")), base: base)
        feed.description = atomText(root.child("subtitle"))
        feed.language = language(root.get("lang") ?? "")
        feed.format = "Atom " + ((root.get("version") ?? "").isEmpty ? "1.0" : RSSText.clean(root.get("version") ?? "", 10))
        let feedAuthor = text(root.child("author")?.child("name"))
        for entry in root.all("entry").prefix(maxItems) {
            var item = RSSParsedItem()
            let extra = media(entry, base: base)
            let url = atomLink(entry, base: base)
            let content = atomText(entry.child("content"), html: true)
            let summary = atomText(entry.child("summary"), html: true)
            let author = text(entry.child("author")?.child("name"))
            let attachments: [(String?, String?, String?)] = entry.all("link")
                .filter { ($0.get("rel") ?? "").lowercased() == "enclosure" }
                .map { ($0.get("href"), $0.get("type"), $0.get("length")) }
            let id = text(entry.child("id"))
            item.guid = id.isEmpty ? url : id
            item.title = atomText(entry.child("title"))
            if item.title.isEmpty { item.title = extra.title }
            item.url = url
            item.author = RSSText.clean(author.isEmpty ? feedAuthor : author, 200)
            item.published = RSSDate.parse(text(entry.child("published", "issued", "created")))
            item.updated = RSSDate.parse(text(entry.child("updated", "modified")))
            item.contentHtml = content.isEmpty ? summary : content
            item.summaryHtml = content.isEmpty ? "" : summary
            if RSSText.strip(item.contentHtml).isEmpty { item.contentHtml = extra.descriptionHtml }
            item.commentsUrl = atomLink(entry, base: base, rel: "replies")
            item.enclosures = enclosures(attachments)
            item.images = extra.images
            item.language = language(entry.get("lang") ?? "")
            feed.items.append(item)
        }
        return feed
    }

    static func rssFeed(_ root: RSSXMLElement, base: String) throws -> RSSParsedFeed {
        guard let channel = root.child("channel") else { throw RSSFeedError.notAFeed }
        var feed = RSSParsedFeed()
        let link = RSSURL.http(text(channel.child("link")), base: base)
        let site = link.isEmpty ? base : link
        var image = channel.child("image")
        var icon = image.map { RSSURL.http(text($0.child("url")), base: base) } ?? ""
        if icon.isEmpty, let itunes = channel.child("itunes:image") {
            icon = RSSURL.http(itunes.get("href") ?? "", base: base)
        }
        let container = channel.all("item").isEmpty ? root : channel
        if root.tag == "rdf" && image == nil {
            image = root.child("image")
            if icon.isEmpty { icon = image.map { RSSURL.http(text($0.child("url")), base: base) } ?? "" }
        }
        feed.title = rssTitle(channel.child("title"))
        feed.siteUrl = site
        feed.iconUrl = icon
        feed.description = RSSHTML.plainText(text(channel.child("description")), limit: 500)
        feed.language = language(text(channel.child("language", "dc:language")))
        if root.tag == "rdf" {
            feed.format = "RSS 1.0"
        } else {
            let version = RSSText.clean(root.get("version") ?? "", 10)
            feed.format = "RSS " + (version.isEmpty ? "2.0" : version)
        }
        feed.items = rssItems(container, base: site)
        return feed
    }

    static func jsonAuthor(_ value: JSONValue?) -> String {
        guard let value else { return "" }
        if let object = value.objectValue { return RSSText.clean(object.str("name"), 200) }
        if let list = value.arrayValue {
            return list.prefix(4).map { jsonAuthor($0) }.filter { !$0.isEmpty }.joined(separator: ", ")
        }
        return ""
    }

    static func jsonString(_ value: JSONValue?) -> String {
        guard let value, !value.isNull else { return "" }
        switch value {
        case .bool(let flag): return flag ? "True" : ""
        case .int(let number): return number == 0 ? "" : String(number)
        case .double(let number): return number == 0 ? "" : value.text
        case .array(let items): return items.isEmpty ? "" : value.text
        case .object(let object): return object.isEmpty ? "" : value.text
        default: return value.text
        }
    }

    static func jsonFeed(_ data: JSONObject, base: String) -> RSSParsedFeed {
        var feed = RSSParsedFeed()
        let home = RSSURL.http(jsonString(data["home_page_url"]), base: base)
        let site = home.isEmpty ? base : home
        for value in data.list("items").prefix(maxItems) {
            guard let entry = value.objectValue else { continue }
            var item = RSSParsedItem()
            var content = jsonString(entry["content_html"])
            let plain = jsonString(entry["content_text"])
            if content.isEmpty && !plain.isEmpty {
                content = plain.components(separatedBy: "\n\n")
                    .filter { !RSSText.strip($0).isEmpty }
                    .map { "<p>" + RSSText.escape($0, quote: true).replacingOccurrences(of: "\n", with: "<br>") + "</p>" }
                    .joined()
            }
            let summary = jsonString(entry["summary"])
            let url = RSSURL.http(jsonString(entry["url"]), base: site)
            let attachments: [(String?, String?, String?)] = entry.maps("attachments").map {
                (jsonString($0["url"]), jsonString($0["mime_type"]), jsonString($0["size_in_bytes"]))
            }
            let id = RSSText.clean(jsonString(entry["id"]), 600)
            item.guid = id.isEmpty ? url : id
            item.title = RSSText.clean(jsonString(entry["title"]), 1000)
            item.url = url
            item.externalUrl = RSSURL.http(jsonString(entry["external_url"]), base: site)
            var author = jsonAuthor(entry["authors"] ?? entry["author"])
            if author.isEmpty { author = jsonAuthor(data["authors"] ?? data["author"]) }
            item.author = author
            item.published = RSSDate.parse(jsonString(entry["date_published"]))
            item.updated = RSSDate.parse(jsonString(entry["date_modified"]))
            item.contentHtml = content.isEmpty ? RSSText.escape(summary, quote: true) : content
            item.summaryHtml = !content.isEmpty && !summary.isEmpty ? RSSText.escape(summary, quote: true) : ""
            item.enclosures = enclosures(attachments)
            for key in ["image", "banner_image"] {
                let image = RSSURL.http(jsonString(entry[key]), base: site)
                if !image.isEmpty && !item.images.contains(image) { item.images.append(image) }
            }
            item.language = language(jsonString(entry["language"]))
            feed.items.append(item)
        }
        feed.title = RSSText.clean(jsonString(data["title"]), 1000)
        feed.siteUrl = site
        let icon = jsonString(data["icon"])
        feed.iconUrl = RSSURL.http(icon.isEmpty ? jsonString(data["favicon"]) : icon, base: base)
        feed.description = RSSText.clean(jsonString(data["description"]), 500)
        feed.language = language(jsonString(data["language"]))
        feed.format = jsonString(data["version"]).hasSuffix("/1") ? "JSON Feed 1.0" : "JSON Feed 1.1"
        return feed
    }

    public static func parse(_ document: String, base: String = "") throws -> RSSParsedFeed {
        var text = document
        while let first = text.unicodeScalars.first, first == "\u{FEFF}" || first == " " || first == "\t" || first == "\r" || first == "\n" {
            text.unicodeScalars.removeFirst()
        }
        if text.isEmpty { throw RSSFeedError.empty }
        if text.hasPrefix("{") {
            guard let value = try? JSON.parse(text), let object = value.objectValue,
                  jsonString(object["version"]).contains("jsonfeed.org")
            else { throw RSSFeedError.notAFeed }
            return jsonFeed(object, base: base)
        }
        guard text.hasPrefix("<") else { throw RSSFeedError.notAFeed }
        let head = RSSText.prefix(text, 2000).lowercased()
        if head.contains("<html") && !head.contains("<rss") && !head.contains("<feed") && !head.contains("<rdf") {
            throw RSSFeedError.notAFeed
        }
        let root: RSSXMLElement
        do {
            root = try RSSXML.parse(text)
        } catch {
            throw RSSFeedError.unreadable
        }
        if root.tag == "feed" { return atomFeed(root, base: base) }
        if root.tag == "rss" || root.tag == "rdf" { return try rssFeed(root, base: base) }
        throw RSSFeedError.notAFeed
    }
}

public struct RSSOPMLEntry: Equatable, Sendable {
    public var url: String
    public var title: String
    public var folder: String
    public var siteUrl: String

    public init(url: String, title: String, folder: String, siteUrl: String) {
        self.url = url
        self.title = title
        self.folder = folder
        self.siteUrl = siteUrl
    }
}

public enum RSSOPML {
    public static func parse(_ document: String) throws -> [RSSOPMLEntry] {
        guard let root = try? RSSXML.parse(document), root.tag == "opml", let body = root.child("body") else {
            throw RSSFeedError.unreadable
        }
        var entries: [RSSOPMLEntry] = []
        func walk(_ node: RSSXMLElement, _ folder: String) {
            for child in node.all("outline") {
                let url = RSSURL.normalizeFeedInput(child.get("xmlurl") ?? "")
                let title = RSSText.clean(child.get("title").flatMap { $0.isEmpty ? nil : $0 } ?? child.get("text") ?? "", 200)
                if !url.isEmpty {
                    entries.append(RSSOPMLEntry(url: url, title: title, folder: folder, siteUrl: RSSURL.http(child.get("htmlurl") ?? "")))
                } else if !child.children.isEmpty {
                    walk(child, folder.isEmpty ? (title.isEmpty ? "Imported" : title) : folder)
                }
            }
        }
        walk(body, "")
        return entries
    }

    static func rfc822(_ millis: Int64) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = TimeZone(identifier: "GMT")
        formatter.dateFormat = "EEE, dd MMM yyyy HH:mm:ss 'GMT'"
        return formatter.string(from: Date(timeIntervalSince1970: Double(millis) / 1000))
    }

    public static func build(folders: [RSSFolder], feeds: [RSSFeed], now: Int64, title: String = "RSS Reader subscriptions") -> String {
        func attr(_ value: String) -> String { RSSText.escape(value, quote: true) }
        func outline(_ feed: RSSFeed, _ indent: String) -> String {
            let home = feed.siteUrl.isEmpty ? "" : " htmlUrl=\"\(attr(feed.siteUrl))\""
            return "\(indent)<outline text=\"\(attr(feed.displayTitle))\" title=\"\(attr(feed.displayTitle))\" type=\"rss\" version=\"RSS\" xmlUrl=\"\(attr(feed.url))\"\(home)/>"
        }
        let byName: (RSSFeed, RSSFeed) -> Bool = { $0.displayTitle.localizedCaseInsensitiveCompare($1.displayTitle) == .orderedAscending }
        var lines = [
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>", "<opml version=\"1.1\">", "  <head>",
            "    <title>\(attr(title))</title>", "    <dateCreated>\(rfc822(now))</dateCreated>", "  </head>", "  <body>",
        ]
        let folderIds = Set(folders.map(\.id))
        for feed in feeds.filter({ $0.folderId.isEmpty || !folderIds.contains($0.folderId) }).sorted(by: byName) {
            lines.append(outline(feed, "    "))
        }
        for folder in folders.sorted(by: { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }) {
            lines.append("    <outline text=\"\(attr(folder.name))\" title=\"\(attr(folder.name))\">")
            for feed in feeds.filter({ $0.folderId == folder.id }).sorted(by: byName) {
                lines.append(outline(feed, "      "))
            }
            lines.append("    </outline>")
        }
        lines += ["  </body>", "</opml>", ""]
        return lines.joined(separator: "\n")
    }
}
