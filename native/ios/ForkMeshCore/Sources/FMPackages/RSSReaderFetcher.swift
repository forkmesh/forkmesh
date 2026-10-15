import FMBase
import FMNet
import Foundation

public struct RSSHTTPAnswer: Sendable {
    public var status: Int
    public var finalUrl: String
    public var permanent: Bool
    public var etag: String
    public var lastModified: String
    public var contentType: String
    public var body: Data

    public var text: String { RSSXML.decode(body, contentType: contentType) }
}

public enum RSSFetchError: Error, Equatable, Sendable {
    case invalidAddress
    case insecure
    case http(Int)
    case tooLarge
    case tooManyRedirects
    case timedOut
    case transport(String)
    case feed(RSSFeedError)
    case noFeedFound
    case noReadableText

    public var code: String {
        switch self {
        case .invalidAddress: return "invalid_url"
        case .insecure: return "insecure_http"
        case .http(let status): return "http_\(status)"
        case .tooLarge: return "too_large"
        case .tooManyRedirects: return "too_many_redirects"
        case .timedOut: return "timeout"
        case .transport(let message): return "network: \(message)"
        case .feed(let error): return error.code
        case .noFeedFound: return "no_feed_found"
        case .noReadableText: return "no_readable_text"
        }
    }
}

public struct RSSDiscovered: Sendable {
    public var url: String
    public var feed: RSSParsedFeed
    public var answer: RSSHTTPAnswer
}

public struct RSSReaderFetcher: Sendable {
    public let http: HTTPClient
    public let upgradesInsecure: Bool
    public let userAgent: String
    public let timeout: TimeInterval

    public init(
        http: HTTPClient, upgradesInsecure: Bool = true, userAgent: String = "ForkMesh-Reader/1.0 (iPhone)",
        timeout: TimeInterval = 30
    ) {
        self.http = http
        self.upgradesInsecure = upgradesInsecure
        self.userAgent = userAgent
        self.timeout = timeout
    }

    static func header(_ headers: HTTPHeaders, _ name: String) -> String {
        if let value = headers[name] { return value }
        for field in headers where field.name.lowercased() == name.lowercased() { return field.value }
        return ""
    }

    static func url(_ text: String) -> URL? {
        if let url = URL(string: text) { return url }
        let allowed = CharacterSet.urlQueryAllowed.union(CharacterSet(charactersIn: "#%"))
        return text.addingPercentEncoding(withAllowedCharacters: allowed).flatMap { URL(string: $0) }
    }

    public func get(_ address: String, headers extra: [String: String] = [:], accept: String = RSSFeedTypes.accept) async
        -> Result<RSSHTTPAnswer, RSSFetchError>
    {
        var current = RSSURL.http(address)
        guard !current.isEmpty else { return .failure(.invalidAddress) }
        var permanent = true
        for hop in 0...RSSReaderLimits.maxRedirects {
            let wasInsecure = RSSURL.isInsecure(current)
            if upgradesInsecure && wasInsecure { current = RSSURL.upgraded(current) }
            guard let target = Self.url(current) else { return .failure(.invalidAddress) }
            var headers: HTTPHeaders = ["Accept": accept, "User-Agent": userAgent]
            for (key, value) in extra where !value.isEmpty { headers[key] = value }
            let request = HTTPRequest(
                method: "GET", url: target, headers: headers, timeout: timeout, followRedirects: false,
                maxResponseBytes: RSSReaderLimits.maxFetchBytes)
            let response: HTTPResponse
            do {
                response = try await http.send(request)
            } catch let error as HTTPError {
                switch error {
                case .responseTooLarge: return .failure(.tooLarge)
                case .timedOut: return .failure(upgradesInsecure && wasInsecure ? .insecure : .timedOut)
                case .cancelled: return .failure(.transport("cancelled"))
                default: return .failure(upgradesInsecure && wasInsecure ? .insecure : .transport(error.description))
                }
            } catch {
                return .failure(upgradesInsecure && wasInsecure ? .insecure : .transport(String(describing: error)))
            }
            if [301, 302, 303, 307, 308].contains(response.status) {
                let location = Self.header(response.headers, "Location")
                let next = RSSURL.http(location, base: current)
                guard !next.isEmpty else { return .failure(.http(response.status)) }
                if response.status != 301 && response.status != 308 { permanent = false }
                current = next
                if hop == RSSReaderLimits.maxRedirects { return .failure(.tooManyRedirects) }
                continue
            }
            if response.status == 304 || (200...299).contains(response.status) {
                return .success(RSSHTTPAnswer(
                    status: response.status, finalUrl: current, permanent: permanent,
                    etag: Self.header(response.headers, "ETag"), lastModified: Self.header(response.headers, "Last-Modified"),
                    contentType: Self.header(response.headers, "Content-Type"), body: response.body))
            }
            return .failure(.http(response.status == 0 ? 502 : response.status))
        }
        return .failure(.tooManyRedirects)
    }

    public func fetch(_ feed: RSSFeed, now: Int64) async -> RSSFetchResult {
        var headers: [String: String] = [:]
        if !feed.etag.isEmpty { headers["If-None-Match"] = feed.etag }
        if !feed.lastModified.isEmpty { headers["If-Modified-Since"] = feed.lastModified }
        let outcome: RSSFetchOutcome
        switch await get(feed.url, headers: headers) {
        case .failure(let error):
            outcome = .failed(error.code)
        case .success(let answer) where answer.status == 304:
            outcome = .notModified
        case .success(let answer):
            do {
                let parsed = try RSSFeedParser.parse(answer.text, base: answer.finalUrl)
                let sameFeed = RSSURL.identity(answer.finalUrl) == RSSURL.identity(feed.url)
                let permanent = answer.permanent || (sameFeed && answer.finalUrl != feed.url)
                let items = RSSReaderStore.prepare(parsed, feedUrl: answer.finalUrl, now: now)
                outcome = .updated(
                    feed: parsed, items: items, finalUrl: answer.finalUrl, permanent: permanent, etag: answer.etag,
                    lastModified: answer.lastModified)
            } catch let error as RSSFeedError {
                outcome = .failed(error.code)
            } catch {
                outcome = .failed(RSSFeedError.unreadable.code)
            }
        }
        return RSSFetchResult(feedId: feed.id, requestedUrl: feed.url, outcome: outcome)
    }

    public func discover(_ input: String) async -> Result<RSSDiscovered, RSSFetchError> {
        let address = RSSURL.normalizeFeedInput(input)
        guard !address.isEmpty else { return .failure(.invalidAddress) }
        let first: RSSHTTPAnswer
        switch await get(address) {
        case .failure(let error): return .failure(error)
        case .success(let answer): first = answer
        }
        let final = first.finalUrl
        let document = first.text
        if let feed = try? RSSFeedParser.parse(document, base: final) {
            return .success(RSSDiscovered(url: final, feed: feed, answer: first))
        }
        let links = RSSFeedTypes.discover(document, base: final)
        var candidates = Array(links.feeds.map(\.url).prefix(3))
        if candidates.isEmpty {
            let origin = RSSURL.origin(final)
            candidates = RSSFeedTypes.guessPaths.map { origin + $0 }
        }
        for candidate in candidates {
            guard case .success(let found) = await get(candidate), found.status != 304 else { continue }
            let target = found.finalUrl
            guard var feed = try? RSSFeedParser.parse(found.text, base: target) else { continue }
            if feed.siteUrl.isEmpty || feed.siteUrl == target { feed.siteUrl = final }
            if feed.iconUrl.isEmpty && !links.icon.isEmpty { feed.iconUrl = links.icon }
            return .success(RSSDiscovered(url: target, feed: feed, answer: found))
        }
        return .failure(.noFeedFound)
    }

    public func readable(_ address: String) async -> Result<RSSReadable.Result, RSSFetchError> {
        switch await get(address, accept: "text/html, application/xhtml+xml;q=0.9, */*;q=0.5") {
        case .failure(let error):
            return .failure(error)
        case .success(let answer):
            let extracted = RSSReadable.extract(answer.text, base: answer.finalUrl)
            return extracted.contentHtml.isEmpty ? .failure(.noReadableText) : .success(extracted)
        }
    }

    public func refresh(_ feeds: [RSSFeed], now: Int64, concurrency: Int = RSSReaderLimits.concurrency,
                        onResult: @escaping @Sendable (RSSFetchResult) async -> Void) async {
        let fetcher = self
        await withTaskGroup(of: RSSFetchResult.self) { group in
            var pending = feeds[...]
            var running = 0
            while running < max(1, concurrency), let next = pending.popFirst() {
                group.addTask { await fetcher.fetch(next, now: now) }
                running += 1
            }
            while let result = await group.next() {
                await onResult(result)
                if let next = pending.popFirst() {
                    group.addTask { await fetcher.fetch(next, now: now) }
                }
            }
        }
    }
}
