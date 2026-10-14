import FMBase
import FMNet
import Foundation
import Observation

public enum ChatLinkPreviewState: Equatable, Sendable {
    case idle
    case loading
    case ready(ChatLinkPreview)
    case none
}

@MainActor
@Observable
public final class ChatLinkPreviewService {
    public static let enabledKey = "chat/link-previews/enabled"
    public static let maxEntries = 400
    public static let maxPageBytes = 768 * 1024
    public static let maxImageBytes = 6 * 1024 * 1024
    public static let maxRedirects = 4
    public static let freshFor: TimeInterval = 3 * 24 * 3600
    public static let missFor: TimeInterval = 6 * 3600
    public static let userAgent =
        "Mozilla/5.0 (compatible; ForkMeshBot/1.0; +https://forkmesh.com/bot) facebookexternalhit/1.1 Twitterbot/1.0"

    private var states: [String: ChatLinkPreviewState] = [:]
    @ObservationIgnored private var order: [String] = []
    @ObservationIgnored private var misses: [String: Date] = [:]
    @ObservationIgnored private var inflight: [String: Task<ChatLinkPreview?, Never>] = [:]
    @ObservationIgnored private var images: [String: Data] = [:]
    @ObservationIgnored private var imageOrder: [String] = []
    @ObservationIgnored private var imageBytes: Int = 0
    @ObservationIgnored private var imageTasks: [String: Task<Data?, Never>] = [:]
    @ObservationIgnored private let http: HTTPClient
    @ObservationIgnored private let store: KeyValueStore?
    @ObservationIgnored private let file: URL?
    @ObservationIgnored private let clock: Clock
    @ObservationIgnored private var warmed: Bool = false
    @ObservationIgnored private var saveQueued: Bool = false
    public private(set) var enabled: Bool

    public init(http: HTTPClient, store: KeyValueStore? = nil, file: URL? = nil, clock: Clock = SystemClock()) {
        self.http = http
        self.store = store
        self.file = file
        self.clock = clock
        enabled = store?.bool(forKey: ChatLinkPreviewService.enabledKey, or: true) ?? true
    }

    public nonisolated static var defaultFile: URL? {
        FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first?
            .appendingPathComponent("chat-link-previews.json")
    }

    public func setEnabled(_ on: Bool) {
        enabled = on
        store?.set(on, forKey: ChatLinkPreviewService.enabledKey)
    }

    public func state(for url: URL) -> ChatLinkPreviewState {
        states[url.absoluteString] ?? .idle
    }

    public func preview(for url: URL) -> ChatLinkPreview? {
        if case .ready(let preview) = state(for: url) { return preview }
        return nil
    }

    public func load(_ url: URL) async {
        guard enabled else { return }
        warm()
        let key = url.absoluteString
        if let cached = states[key] {
            switch cached {
            case .loading: break
            case .ready(let preview) where clock.now.timeIntervalSince(preview.fetchedAt) < ChatLinkPreviewService.freshFor:
                return
            case .none where clock.now.timeIntervalSince(misses[key] ?? .distantPast) < ChatLinkPreviewService.missFor:
                return
            default: break
            }
        }
        if let running = inflight[key] {
            _ = await running.value
            return
        }
        if states[key] == nil { states[key] = .loading }
        let http = self.http
        let now = clock.now
        let task = Task<ChatLinkPreview?, Never> { await ChatLinkPreviewService.resolve(url, http: http, now: now) }
        inflight[key] = task
        let result = await task.value
        inflight[key] = nil
        if let result, !result.isEmpty {
            remember(key, .ready(result))
        } else if case .ready = states[key] {
        } else {
            misses[key] = clock.now
            remember(key, .none)
        }
    }

    public func imageData(_ raw: String) async -> Data? {
        guard enabled, let url = ChatLinkPreviewParsing.previewable(raw) else { return nil }
        let key = url.absoluteString
        if let data = images[key] { return data }
        if let running = imageTasks[key] { return await running.value }
        let http = self.http
        let task = Task<Data?, Never> {
            guard let response = await ChatLinkPreviewService.fetch(url, http: http, accept: "image/*", limit: ChatLinkPreviewService.maxImageBytes),
                response.ok
            else { return nil }
            let type = (response.header("content-type") ?? "").lowercased()
            if !type.isEmpty && !type.hasPrefix("image/") && !type.hasPrefix("application/octet-stream") { return nil }
            return response.body.isEmpty ? nil : response.body
        }
        imageTasks[key] = task
        let data = await task.value
        imageTasks[key] = nil
        if let data { storeImage(data, for: key) }
        return data
    }

    private func storeImage(_ data: Data, for key: String) {
        images[key] = data
        imageOrder.append(key)
        imageBytes += data.count
        while imageBytes > 48 * 1024 * 1024, !imageOrder.isEmpty {
            let old = imageOrder.removeFirst()
            imageBytes -= images[old]?.count ?? 0
            images[old] = nil
        }
    }

    private func remember(_ key: String, _ state: ChatLinkPreviewState) {
        states[key] = state
        order.removeAll { $0 == key }
        order.append(key)
        while order.count > ChatLinkPreviewService.maxEntries {
            states[order.removeFirst()] = nil
        }
        if case .ready = state { scheduleSave() }
    }

    public func warm() {
        if warmed { return }
        warmed = true
        guard let file else { return }
        let saved = ChatLinkPreviewService.read(file)
        let now = clock.now
        for preview in saved where states[preview.url] == nil
            && now.timeIntervalSince(preview.fetchedAt) < ChatLinkPreviewService.freshFor
        {
            states[preview.url] = .ready(preview)
            order.append(preview.url)
        }
    }

    private func scheduleSave() {
        guard file != nil, !saveQueued else { return }
        saveQueued = true
        Task {
            try? await Task.sleep(for: .seconds(2))
            saveQueued = false
            flush()
        }
    }

    public func flush() {
        guard let file else { return }
        let snapshot: [ChatLinkPreview] = order.compactMap { key in
            if case .ready(let preview) = states[key] { return preview }
            return nil
        }
        Task.detached(priority: .utility) { ChatLinkPreviewService.write(snapshot, to: file) }
    }

    public nonisolated static func read(_ file: URL) -> [ChatLinkPreview] {
        guard let data = try? Data(contentsOf: file) else { return [] }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .secondsSince1970
        return (try? decoder.decode([ChatLinkPreview].self, from: data)) ?? []
    }

    public nonisolated static func write(_ previews: [ChatLinkPreview], to file: URL) {
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .secondsSince1970
        guard let data = try? encoder.encode(previews) else { return }
        do {
            try FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
            #if os(Linux)
            try data.write(to: file, options: [.atomic])
            #else
            try data.write(to: file, options: [.atomic, .completeFileProtectionUntilFirstUserAuthentication])
            #endif
        } catch {
            Log.warning("chat", "could not save link previews: \(error)")
        }
    }

    public nonisolated static func fetch(
        _ start: URL, http: HTTPClient, accept: String, limit: Int, truncate: Bool = false, timeout: TimeInterval = 8
    ) async -> HTTPResponse? {
        var url = start
        let began = Date()
        for _ in 0...maxRedirects {
            guard ChatLinkPreviewParsing.previewable(url.absoluteString) != nil else { return nil }
            let headers: HTTPHeaders = ["Accept": accept, "User-Agent": userAgent, "Accept-Language": "en"]
            let request = HTTPRequest(
                url: url, headers: headers, timeout: timeout, followRedirects: false, maxResponseBytes: limit)
            guard let streamed = try? await http.stream(request) else { return nil }
            if (300...399).contains(streamed.status) {
                guard let location = streamed.headers["location"],
                    let next = URL(string: location, relativeTo: url)?.absoluteURL
                else { return nil }
                url = next
                continue
            }
            var body = Data()
            do {
                for try await chunk in streamed.body {
                    body.append(chunk)
                    if body.count >= limit {
                        if !truncate { return nil }
                        body = body.prefix(limit)
                        break
                    }
                    if Date().timeIntervalSince(began) > timeout * 2 { return nil }
                }
            } catch {
                if body.isEmpty || !truncate { return nil }
            }
            var headersOut = streamed.headers
            headersOut.add("x-forkmesh-final-url", url.absoluteString)
            return HTTPResponse(status: streamed.status, headers: headersOut, body: body)
        }
        return nil
    }

    public nonisolated static func resolve(_ url: URL, http: HTTPClient, now: Date) async -> ChatLinkPreview? {
        let site = ChatLinkSites.site(for: url)
        var preview: ChatLinkPreview?
        if ChatLinkPreviewParsing.isImageLink(url) {
            preview = ChatLinkPreview(url: url.absoluteString, title: url.lastPathComponent, imageUrl: url.absoluteString, kind: .image)
        } else if let site {
            preview = await special(site, url: url, http: http)
        }
        if preview == nil || preview?.isEmpty == true {
            preview = await page(url, http: http, fallback: preview)
        }
        guard var found = preview, !found.isEmpty else { return nil }
        let landed = URL(string: found.url) ?? url
        found.url = url.absoluteString
        if let known = site ?? (found.siteKey.isEmpty ? nil : ChatLinkSites.site(key: found.siteKey)) {
            found.siteKey = known.key
            if found.siteName.isEmpty || found.siteName.count > 40 { found.siteName = known.name }
            if found.kind == .page { found.kind = known.kind }
        }
        if found.siteName.isEmpty { found.siteName = ChatLinkSites.bareHost(landed.host ?? url.host ?? "") }
        if found.title.isEmpty && !found.detail.isEmpty {
            found.title = ChatLinkPreviewParsing.truncate(found.detail, ChatLinkPreviewParsing.maxTitle)
            found.detail = ""
        }
        found.fetchedAt = now
        return found
    }

    nonisolated static func page(_ url: URL, http: HTTPClient, fallback: ChatLinkPreview?) async -> ChatLinkPreview? {
        guard let response = await fetch(url, http: http, accept: "text/html,application/xhtml+xml;q=0.9,*/*;q=0.5", limit: maxPageBytes, truncate: true),
            response.ok || response.status == 206
        else { return fallback }
        let finalUrl = response.header("x-forkmesh-final-url").flatMap { URL(string: $0) } ?? url
        let type = (response.header("content-type") ?? "").lowercased()
        if type.hasPrefix("image/") {
            return ChatLinkPreview(url: url.absoluteString, title: url.lastPathComponent, imageUrl: finalUrl.absoluteString, kind: .image)
        }
        if !type.isEmpty && !type.contains("html") && !type.contains("xml") { return fallback }
        var parsed = ChatLinkPreviewParsing.parseHTML(Utf8.decodeLossy(response.body), page: finalUrl)
        if let fallback {
            if parsed.title.isEmpty { parsed.title = fallback.title }
            if parsed.imageUrl.isEmpty { parsed.imageUrl = fallback.imageUrl }
            if parsed.author.isEmpty { parsed.author = fallback.author }
            if parsed.detail.isEmpty { parsed.detail = fallback.detail }
        }
        if let finalSite = ChatLinkSites.site(for: finalUrl), ChatLinkSites.site(for: url) == nil {
            parsed.siteKey = finalSite.key
        }
        return parsed
    }

    nonisolated static func json(_ raw: String, http: HTTPClient) async -> JSONObject? {
        guard let url = URL(string: raw),
            let response = await fetch(url, http: http, accept: "application/json", limit: 512 * 1024), response.ok
        else { return nil }
        let object = response.jsonObject
        return object.isEmpty ? nil : object
    }

    nonisolated static func encoded(_ url: URL) -> String {
        url.absoluteString.addingPercentEncoding(withAllowedCharacters: .alphanumerics.union(CharacterSet(charactersIn: "-._~"))) ?? ""
    }

    nonisolated static func special(_ site: ChatLinkSite, url: URL, http: HTTPClient) async -> ChatLinkPreview? {
        let link = url.absoluteString
        let clip = ChatLinkPreviewParsing.truncate
        switch site.resolver {
        case .page:
            return nil
        case .youtube:
            guard let id = ChatLinkPreviewParsing.youtubeId(url) else { return nil }
            let canonical = "https://www.youtube.com/watch?v=\(id)"
            let thumbnail = "https://i.ytimg.com/vi/\(id)/hqdefault.jpg"
            let object = await json(
                "https://www.youtube.com/oembed?format=json&url=\(encoded(URL(string: canonical)!))", http: http)
            return ChatLinkPreview(
                url: link, title: clip(object?.str("title") ?? "", ChatLinkPreviewParsing.maxTitle),
                author: clip(object?.str("author_name") ?? "", 80), siteName: site.name, siteKey: site.key,
                imageUrl: thumbnail, kind: .video)
        case .oembed(let endpoint):
            guard let object = await json(endpoint + encoded(url), http: http) else { return nil }
            var image = object.firstStr("thumbnail_url", "url")
            if object.str("type") != "photo" && image == object.str("url") && object.str("thumbnail_url").isEmpty { image = "" }
            return ChatLinkPreview(
                url: link, title: clip(object.str("title"), ChatLinkPreviewParsing.maxTitle),
                detail: clip(object.str("description"), ChatLinkPreviewParsing.maxDetail),
                author: clip(object.str("author_name"), 80), siteName: object.str("provider_name").isEmpty ? site.name : object.str("provider_name"),
                siteKey: site.key, imageUrl: ChatLinkPreviewParsing.resolve(image, against: url),
                kind: object.str("type") == "photo" ? .image : site.kind)
        case .tweet:
            guard url.path.contains("/status/") else { return nil }
            let canonical = "https://twitter.com" + url.path
            guard let object = await json(
                "https://publish.twitter.com/oembed?omit_script=1&dnt=true&url=\(encoded(URL(string: canonical)!))", http: http)
            else { return nil }
            return ChatLinkPreview(
                url: link, title: clip(ChatLinkPreviewParsing.tweetText(object.str("html")), ChatLinkPreviewParsing.maxDetail),
                author: clip(object.str("author_name"), 80), siteName: site.name, siteKey: site.key, kind: .social)
        case .wikipedia:
            guard let page = ChatLinkPreviewParsing.wikipediaPage(url),
                let object = await json("https://\(page.host)/api/rest_v1/page/summary/\(page.title)", http: http)
            else { return nil }
            return ChatLinkPreview(
                url: link, title: clip(object.str("title"), ChatLinkPreviewParsing.maxTitle),
                detail: clip(object.str("extract"), ChatLinkPreviewParsing.maxDetail),
                author: clip(object.str("description"), 80), siteName: site.name, siteKey: site.key,
                imageUrl: ChatLinkPreviewParsing.resolve(object.obj("thumbnail").str("source"), against: url), kind: .reference)
        case .hackerNews:
            guard let id = ChatLinkPreviewParsing.hackerNewsId(url),
                let object = await json("https://hacker-news.firebaseio.com/v0/item/\(id).json", http: http)
            else { return nil }
            let points = object.int("score")
            let comments = object.int("descendants")
            var parts: [String] = []
            if points > 0 { parts.append("\(points) \(points == 1 ? "point" : "points")") }
            if !object.str("by").isEmpty { parts.append("by \(object.str("by"))") }
            if object["descendants"] != nil { parts.append("\(comments) \(comments == 1 ? "comment" : "comments")") }
            let text = object.str("title").isEmpty ? ChatLinkPreviewParsing.stripTags(object.str("text")) : object.str("title")
            return ChatLinkPreview(
                url: link, title: clip(text, ChatLinkPreviewParsing.maxTitle), detail: parts.joined(separator: " \u{00B7} "),
                siteName: site.name, siteKey: site.key, kind: .social)
        case .npm:
            guard let name = ChatLinkPreviewParsing.packageName(url, after: "package"),
                let object = await json("https://registry.npmjs.org/\(name.replacingOccurrences(of: "/", with: "%2F"))/latest", http: http)
            else { return nil }
            return ChatLinkPreview(
                url: link, title: "\(object.str("name")) \(object.str("version"))".trimmingCharacters(in: .whitespaces),
                detail: clip(object.str("description"), ChatLinkPreviewParsing.maxDetail), siteName: site.name, siteKey: site.key,
                kind: .package)
        case .pypi:
            guard let name = ChatLinkPreviewParsing.packageName(url, after: "project"),
                let object = await json("https://pypi.org/pypi/\(name)/json", http: http)
            else { return nil }
            let info = object.obj("info")
            return ChatLinkPreview(
                url: link, title: "\(info.str("name")) \(info.str("version"))".trimmingCharacters(in: .whitespaces),
                detail: clip(info.str("summary"), ChatLinkPreviewParsing.maxDetail), author: clip(info.str("author"), 80),
                siteName: site.name, siteKey: site.key, kind: .package)
        case .crates:
            guard let name = ChatLinkPreviewParsing.packageName(url, after: "crates"),
                let object = await json("https://crates.io/api/v1/crates/\(name)", http: http)
            else { return nil }
            let crate = object.obj("crate")
            return ChatLinkPreview(
                url: link, title: "\(crate.str("name")) \(crate.firstStr("max_stable_version", "max_version"))".trimmingCharacters(in: .whitespaces),
                detail: clip(crate.str("description"), ChatLinkPreviewParsing.maxDetail), siteName: site.name, siteKey: site.key,
                kind: .package)
        case .appStore:
            guard let id = ChatLinkPreviewParsing.appStoreId(url),
                let object = await json("https://itunes.apple.com/lookup?id=\(id)", http: http),
                let result = object.maps("results").first
            else { return nil }
            return ChatLinkPreview(
                url: link, title: clip(result.firstStr("trackName", "collectionName"), ChatLinkPreviewParsing.maxTitle),
                detail: clip(result.str("description"), ChatLinkPreviewParsing.maxDetail),
                author: clip(result.firstStr("sellerName", "artistName"), 80), siteName: site.name, siteKey: site.key,
                imageUrl: ChatLinkPreviewParsing.resolve(result.firstStr("artworkUrl512", "artworkUrl100"), against: url), kind: .app)
        case .steam:
            guard let id = ChatLinkPreviewParsing.steamAppId(url),
                let object = await json("https://store.steampowered.com/api/appdetails?appids=\(id)", http: http)
            else { return nil }
            let data = object.obj(id).obj("data")
            if data.isEmpty { return nil }
            return ChatLinkPreview(
                url: link, title: clip(data.str("name"), ChatLinkPreviewParsing.maxTitle),
                detail: clip(ChatLinkPreviewParsing.decodeEntities(data.str("short_description")), ChatLinkPreviewParsing.maxDetail),
                author: clip(data.strings("developers").joined(separator: ", "), 80), siteName: site.name, siteKey: site.key,
                imageUrl: ChatLinkPreviewParsing.resolve(data.str("header_image"), against: url), kind: .app)
        case .xkcd:
            guard let number = ChatLinkPreviewParsing.xkcdNumber(url),
                let object = await json(number.isEmpty ? "https://xkcd.com/info.0.json" : "https://xkcd.com/\(number)/info.0.json", http: http)
            else { return nil }
            return ChatLinkPreview(
                url: link, title: clip(object.firstStr("safe_title", "title"), ChatLinkPreviewParsing.maxTitle),
                detail: clip(object.str("alt"), ChatLinkPreviewParsing.maxDetail), siteName: site.name, siteKey: site.key,
                imageUrl: ChatLinkPreviewParsing.resolve(object.str("img"), against: url), kind: .image)
        }
    }
}
