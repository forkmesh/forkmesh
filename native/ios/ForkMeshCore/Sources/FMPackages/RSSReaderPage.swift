import FMBase
import Foundation

public enum RSSReaderText {
    public static let title = "RSS Reader"
    public static let feedsTitle = "Feeds"
    public static let smartFeeds = "Smart Feeds"
    public static let onThisIPhone = "On This iPhone"
    public static let netNewsWireURL = "https://netnewswire.com/"
    public static let netNewsWireSourceURL = "https://github.com/Ranchero-Software/NetNewsWire"
    public static let netNewsWireRequestsURL = "https://github.com/Ranchero-Software/NetNewsWire/issues"
    public static let credit =
        "Inspired by NetNewsWire, the free and open source RSS reader created by Brent Simmons and developed by Ranchero Software and its contributors (MIT licensed). RSS Reader shares no code with NetNewsWire and is not affiliated with or endorsed by it."
    public static let featureCredit =
        "RSS Reader's smart feeds, timeline, article view and settings follow NetNewsWire's design, and its custom smart feeds, Read Later, content filters, timeline thumbnails, Media RSS images and other recent features were chosen from the feature requests NetNewsWire's users have filed on GitHub. Thanks to Brent Simmons and the NetNewsWire community."
    public static let about =
        "RSS Reader follows blogs, news sites and podcasts through their RSS, Atom and JSON feeds. Your subscriptions, articles and read and starred state stay on this iPhone; it only contacts the sites you follow: their feeds, their icons and images (unless you turn images off), and an article's page when you ask for reader view."
    public static let caughtUp = "You're all caught up."
    public static let readerFailed = "Reader view could not find the article text on this page."

    public static func feedError(_ code: String) -> String {
        if code.hasPrefix("network: ") { return "Could not reach the site: \(code.dropFirst(9))" }
        if code.hasPrefix("http_") {
            let status = code.dropFirst(5)
            switch status {
            case "404": return "The feed was not found (404)."
            case "410": return "The feed is gone (410)."
            case "401", "403": return "The site refused the request (\(status))."
            case "429": return "The site asked to slow down (429)."
            default: return "The site answered \(status)."
            }
        }
        switch code {
        case "invalid_url": return "The address is not a valid web address."
        case "insecure_http":
            return "This feed is only served over plain HTTP, which iOS does not allow. Ask the site for an https:// address."
        case "too_large": return "The feed is larger than 2 MB."
        case "too_many_redirects": return "The address redirects too many times."
        case "timeout": return "The site took too long to answer."
        case "not_a_feed": return "The address does not point to a feed."
        case "feed_unreadable": return "The feed could not be read."
        case "empty_feed": return "The feed is empty."
        case "no_feed_found": return "No feed was found at that address."
        case "no_readable_text": return readerFailed
        default: return code.isEmpty ? "" : code.replacingOccurrences(of: "_", with: " ")
        }
    }

    public static func formatter(_ date: DateFormatter.Style, _ time: DateFormatter.Style, timeZone: TimeZone, locale: Locale) -> DateFormatter {
        let formatter = DateFormatter()
        formatter.locale = locale
        formatter.timeZone = timeZone
        formatter.dateStyle = date
        formatter.timeStyle = time
        formatter.doesRelativeDateFormatting = false
        return formatter
    }

    public static func articleDate(_ millis: Int64, timeZone: TimeZone = .current, locale: Locale = .current) -> String {
        guard millis > 0 else { return "" }
        return formatter(.medium, .short, timeZone: timeZone, locale: locale)
            .string(from: Date(timeIntervalSince1970: Double(millis) / 1000)).uppercased(with: locale)
    }

    public static func timelineDate(_ millis: Int64, now: Int64, timeZone: TimeZone = .current, locale: Locale = .current) -> String {
        guard millis > 0 else { return "" }
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        let date = Date(timeIntervalSince1970: Double(millis) / 1000)
        let today = Date(timeIntervalSince1970: Double(now) / 1000)
        if calendar.isDate(date, inSameDayAs: today) {
            return formatter(.none, .short, timeZone: timeZone, locale: locale).string(from: date)
        }
        return formatter(.short, .none, timeZone: timeZone, locale: locale).string(from: date)
    }

    public static func stamp(_ millis: Int64, timeZone: TimeZone = .current, locale: Locale = .current) -> String {
        guard millis > 0 else { return "Never" }
        return formatter(.medium, .short, timeZone: timeZone, locale: locale).string(from: Date(timeIntervalSince1970: Double(millis) / 1000))
    }

    public static func updated(_ millis: Int64, now: Int64, timeZone: TimeZone = .current, locale: Locale = .current) -> String {
        guard millis > 0 else { return "" }
        let date = Date(timeIntervalSince1970: Double(millis) / 1000)
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = timeZone
        if calendar.isDate(date, inSameDayAs: Date(timeIntervalSince1970: Double(now) / 1000)) {
            return "Updated " + formatter(.none, .short, timeZone: timeZone, locale: locale).string(from: date)
        }
        return "Updated " + formatter(.short, .short, timeZone: timeZone, locale: locale).string(from: date)
    }

    public static func plural(_ count: Int, _ word: String) -> String {
        "\(count.formatted()) \(word)\(count == 1 ? "" : "s")"
    }

    public static func markAllPrompt(_ count: Int, scope: String) -> String {
        "Mark \(plural(count, "article")) in \u{201C}\(scope)\u{201D} as read?"
    }

    public static func markOlderPrompt(_ count: Int, _ choice: RSSCatchUp, scope: String?) -> String {
        guard let scope else { return "Mark \(plural(count, "article")) \(choice.phrase) as read?" }
        return "Mark \(plural(count, "article")) \(choice.phrase) in \u{201C}\(scope)\u{201D} as read?"
    }

    public static func time(_ millis: Int64, timeZone: TimeZone = .current, locale: Locale = .current) -> String {
        formatter(.none, .short, timeZone: timeZone, locale: locale).string(from: Date(timeIntervalSince1970: Double(millis) / 1000))
    }

    public static func checkResult(format: String, items: Int, at millis: Int64, timeZone: TimeZone = .current, locale: Locale = .current) -> String {
        let parts = [format, plural(items, "item"), "checked " + time(millis, timeZone: timeZone, locale: locale)]
        return parts.filter { !$0.isEmpty }.joined(separator: " \u{00B7} ")
    }

    public static func notModified(at millis: Int64, timeZone: TimeZone = .current, locale: Locale = .current) -> String {
        "Not modified \u{00B7} checked " + time(millis, timeZone: timeZone, locale: locale)
    }

    public static func lastCheckFailed(_ code: String) -> String { "Last check failed: \(feedError(code))" }

    public static let shareMessageLimit = 4_000

    public static func shareMessage(title: String, link: String, feed: String = "", comment: String = "") -> String {
        let note = RSSText.strip(comment)
        let headline = RSSText.clean(title, 300)
        let source = RSSText.clean(feed, 120)
        var heading = headline.isEmpty ? "Untitled" : headline
        if !source.isEmpty && source != headline { heading += " \u{2014} \(source)" }
        let lines = [note, heading, RSSURL.http(link)].filter { !$0.isEmpty }
        return RSSText.prefix(lines.joined(separator: "\n"), shareMessageLimit)
    }

    static func isCJK(_ scalar: Unicode.Scalar) -> Bool {
        switch scalar.value {
        case 0x1100...0x11FF, 0x2E80...0x2FDF, 0x3041...0x309F, 0x30A1...0x30FA, 0x3130...0x318F, 0x31F0...0x31FF,
             0x3400...0x4DBF, 0x4E00...0x9FFF, 0xAC00...0xD7AF, 0xF900...0xFAFF, 0xFF66...0xFF9F, 0xFFA0...0xFFDC, 0x20000...0x3FFFF:
            return true
        default:
            return false
        }
    }

    public static func readingMinutes(_ text: String) -> Int {
        var words = 0
        var cjk = 0
        var inWord = false
        for scalar in text.unicodeScalars {
            if RSSText.isSpace(scalar) {
                inWord = false
            } else if isCJK(scalar) {
                cjk += 1
            } else if !inWord {
                inWord = true
                words += 1
            }
        }
        if words == 0 && cjk == 0 { return 0 }
        return max(1, Int((Double(words) / 230 + Double(cjk) / 500).rounded(.up)))
    }

    public static func readingTime(_ text: String) -> String {
        let minutes = readingMinutes(text)
        return minutes == 0 ? "" : "\(minutes) min read"
    }
}

public enum RSSLanguage {
    public static let rightToLeft: Set<String> = ["ar", "he", "iw", "fa", "ur", "yi", "ps", "sd", "ug", "dv", "ckb", "syr"]

    public static func primary(_ tag: String) -> String {
        String(RSSText.strip(tag).lowercased().split(whereSeparator: { $0 == "-" || $0 == "_" }).first ?? "")
    }

    public static func direction(_ tag: String) -> String { rightToLeft.contains(primary(tag)) ? "rtl" : "auto" }

    public static func tag(_ value: String) -> String {
        let clean = RSSText.strip(value).replacingOccurrences(of: "_", with: "-")
        guard !clean.isEmpty, clean.count <= 35, clean.allSatisfy({ $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "-") }) else { return "" }
        return clean
    }
}

public struct RSSPageStyle: Equatable, Sendable {
    public var theme: RSSArticleTheme
    public var font: RSSArticleFont
    public var size: Int
    public var palette: RSSColorPalette
    public var remoteImages: Bool

    public init(settings: RSSReaderSettings) {
        theme = settings.articleTheme
        font = settings.articleFont
        size = settings.articleSize
        palette = settings.palette
        remoteImages = settings.loadRemoteImages
    }

    struct Colors {
        var background: String
        var text: String
        var secondary: String
        var accent: String
        var rule: String
        var code: String
    }

    func colors(dark: Bool) -> Colors {
        switch (theme, dark) {
        case (.sepia, false): return Colors(background: "#F4ECD8", text: "#433422", secondary: "#7B6A55", accent: "#A0522D", rule: "#D9CBB0", code: "#EADFC6")
        case (.sepia, true): return Colors(background: "#2B2419", text: "#E8DCC4", secondary: "#B3A48A", accent: "#E0A060", rule: "#4A3F2E", code: "#3A3122")
        case (.paper, false): return Colors(background: "#FBFBF8", text: "#222222", secondary: "#6E6E73", accent: "#0A64C8", rule: "#E2E2DC", code: "#F0F0EA")
        case (.paper, true): return Colors(background: "#1E1E1C", text: "#E6E6E1", secondary: "#9C9C96", accent: "#5AA9FF", rule: "#3A3A36", code: "#2A2A27")
        case (.contrast, false): return Colors(background: "#FFFFFF", text: "#000000", secondary: "#000000", accent: "#0040DD", rule: "#000000", code: "#EEEEEE")
        case (.contrast, true): return Colors(background: "#000000", text: "#FFFFFF", secondary: "#FFFFFF", accent: "#66B2FF", rule: "#FFFFFF", code: "#222222")
        case (.standard, false): return Colors(background: "#FFFFFF", text: "#1C1C1E", secondary: "#8E8E93", accent: "#007AFF", rule: "#D1D1D6", code: "#F2F2F7")
        case (.standard, true): return Colors(background: "#000000", text: "#F2F2F7", secondary: "#8E8E93", accent: "#0A84FF", rule: "#38383A", code: "#1C1C1E")
        }
    }

    func variables(_ colors: Colors) -> String {
        "--bg:\(colors.background);--fg:\(colors.text);--sub:\(colors.secondary);--accent:\(colors.accent);--rule:\(colors.rule);--code:\(colors.code);"
    }

    var css: String {
        var out = ""
        switch palette {
        case .light: out += ":root{\(variables(colors(dark: false)))color-scheme:light;}"
        case .dark: out += ":root{\(variables(colors(dark: true)))color-scheme:dark;}"
        case .automatic:
            out += ":root{\(variables(colors(dark: false)))color-scheme:light dark;}"
            out += "@media (prefers-color-scheme: dark){:root{\(variables(colors(dark: true)))}}"
        }
        out += """
        html{-webkit-text-size-adjust:100%;overflow-x:hidden;}
        body{margin:0;padding:16px 20px 120px;background:var(--bg);color:var(--fg);font-family:\(font.css);font-size:\(size)px;line-height:1.55;word-wrap:break-word;overflow-wrap:anywhere;overflow-x:clip;}
        a{color:var(--accent);text-decoration:none;}
        .byline{display:flex;align-items:flex-start;justify-content:space-between;gap:12px;}
        .byline-text{display:flex;flex-direction:column;min-width:0;font-family:-apple-system,system-ui,sans-serif;font-size:15px;line-height:1.3;}
        .feed{color:var(--accent);font-weight:600;}
        .author{color:var(--accent);}
        .tile{flex:none;width:48px;height:48px;border-radius:11px;overflow:hidden;background:var(--code);display:flex;align-items:center;justify-content:center;font:600 22px -apple-system,system-ui,sans-serif;color:var(--sub);}
        .tile img{width:48px;height:48px;object-fit:cover;}
        hr.rule{border:0;border-top:0.5px solid var(--rule);margin:12px 0 14px;}
        h1.title{font-family:-apple-system,system-ui,sans-serif;font-size:1.45em;line-height:1.2;font-weight:700;margin:0 0 6px;}
        h1.title a{color:var(--fg);}
        .date{font-family:-apple-system,system-ui,sans-serif;font-size:12px;letter-spacing:0.04em;color:var(--sub);margin:0 0 18px;text-transform:uppercase;}
        img,video{max-width:100%;height:auto;}
        audio{width:100%;}
        figure{margin:1em 0;}
        figcaption{font-size:0.85em;color:var(--sub);}
        pre,code{font-family:ui-monospace,Menlo,monospace;font-size:0.88em;background:var(--code);border-radius:6px;}
        pre{padding:12px;overflow-x:auto;white-space:pre;word-wrap:normal;overflow-wrap:normal;}
        code{padding:1px 4px;}
        pre code{padding:0;background:none;white-space:inherit;}
        .alt-caption{display:block;margin:4px 0 12px;font-size:0.8em;line-height:1.35;color:var(--sub);}
        blockquote{margin:1em 0;padding-left:14px;border-left:3px solid var(--rule);color:var(--sub);}
        table{border-collapse:collapse;max-width:100%;display:block;overflow-x:auto;}
        td,th{border:0.5px solid var(--rule);padding:4px 8px;}
        .note{color:var(--sub);font-size:0.9em;}
        .enclosures{margin:0 0 18px;}
        .enclosures p{margin:6px 0;}
        .comments{margin-top:24px;font-family:-apple-system,system-ui,sans-serif;font-size:15px;}
        """
        return out
    }

    var policy: String {
        let images = remoteImages ? "https: http: data:" : "data:"
        let media = remoteImages ? "https: http:" : "'none'"
        return "default-src 'none'; style-src 'unsafe-inline'; img-src \(images); media-src \(media); script-src 'none'; form-action 'none'; frame-src 'none'"
    }
}

public enum RSSArticlePage {
    public static func enclosures(_ list: [RSSEnclosure], remote: Bool) -> String {
        let shown = list.filter { !$0.isImage }
        guard !shown.isEmpty else { return "" }
        var out = "<div class=\"enclosures\">"
        for item in shown {
            let url = RSSText.escape(item.url, quote: true)
            let player = RSSText.escape(RSSURL.upgraded(item.url), quote: true)
            if remote && item.isAudio {
                out += "<p><audio controls preload=\"none\" src=\"\(player)\"></audio></p>"
            } else if remote && item.isVideo {
                out += "<p><video controls preload=\"none\" src=\"\(player)\"></video></p>"
            }
            let label = item.isAudio ? "Audio" : (item.isVideo ? "Video" : "Attachment")
            let type = item.type.isEmpty ? "" : " (\(RSSText.escape(item.type, quote: false)))"
            out += "<p><a href=\"\(url)\">\(label)\(type)</a></p>"
        }
        return out + "</div>"
    }

    public static func leadImages(_ images: [String], content: String) -> String {
        var out = ""
        for image in images where !image.isEmpty {
            if content.contains(image) || content.contains(RSSText.escape(image, quote: true)) { continue }
            out += "<p><img src=\"\(RSSText.escape(image, quote: true))\" alt=\"\"></p>"
        }
        return out
    }

    public static func sameLink(_ text: String) -> String {
        var parts = RSSURL.split(RSSText.strip(RSSEntities.decode(RSSEntities.decode(text))))
        parts.scheme = parts.scheme.lowercased()
        if parts.scheme == "http" { parts.scheme = "https" }
        let host = parts.host
        var port = ""
        if case .some(.some(let number)) = parts.port, number != 80, number != 443 { port = ":\(number)" }
        var path = parts.path
        while path.hasSuffix("/") { path.removeLast() }
        let query = (parts.query ?? "").isEmpty ? "" : "?" + (parts.query ?? "")
        let fragment = (parts.fragment ?? "").isEmpty ? "" : "#" + (parts.fragment ?? "")
        return "\(parts.scheme)://\(host)\(port)\(path)\(query)\(fragment)"
    }

    public static func linksTo(_ html: String, _ target: String) -> Bool {
        let wanted = sameLink(target)
        for token in RSSHTMLTokenizer.tokens(html) {
            guard case let .start(name, attributes, _) = token, name == "a",
                  let href = attributes.first(where: { $0.name == "href" })?.value, !href.isEmpty else { continue }
            if sameLink(href) == wanted { return true }
        }
        return false
    }

    public static func html(
        article: RSSArticle, body: RSSArticleBody, feed: RSSFeed?, settings: RSSReaderSettings, reader: Bool,
        icon: Data? = nil, timeZone: TimeZone = .current, locale: Locale = .current
    ) -> String {
        let style = RSSPageStyle(settings: settings)
        let link = RSSURL.http(article.link)
        let site = RSSURL.http(feed?.siteUrl ?? "")
        let base = link.isEmpty ? site : link
        let feedName = feed?.displayTitle ?? "Feed"
        let original = reader && !body.readerHtml.isEmpty ? body.readerHtml : body.contentHtml
        let source = leadImages(body.images, content: original) + original
        var content = RSSHTML.sanitize(source, base: base, remoteMedia: style.remoteImages, upgradeMedia: true, altCaptions: true)
        let minutes = RSSReaderText.readingTime(RSSHTML.plainText(original, limit: Int.max))
        if content.isEmpty {
            let summary = article.summary.isEmpty ? "This article has no text in its feed." : article.summary
            content = "<p class=\"note\">\(RSSText.escape(summary, quote: false))</p>"
            if !link.isEmpty { content += "<p class=\"note\">Use reader view or open the original to read it.</p>" }
        }
        let feedLabel = RSSText.escape(feedName, quote: false)
        let feedMarkup = site.isEmpty
            ? "<span class=\"feed\">\(feedLabel)</span>"
            : "<a class=\"feed\" href=\"\(RSSText.escape(site, quote: true))\">\(feedLabel)</a>"
        let author = article.author.isEmpty ? "" : "<span class=\"author\">\(RSSText.escape(article.author, quote: false))</span>"
        let inlined = RSSFeedIcons.inline(icon)
        let iconSource = inlined.isEmpty ? RSSURL.upgraded(RSSURL.http(feed?.iconAddress ?? "")) : inlined
        let tile = style.remoteImages && !iconSource.isEmpty
            ? "<div class=\"tile\"><img src=\"\(RSSText.escape(iconSource, quote: true))\" alt=\"\" referrerpolicy=\"no-referrer\"></div>"
            : "<div class=\"tile\">\(RSSText.escape(RSSText.letter(feedName), quote: false))</div>"
        let titleText = RSSText.escape(article.title.isEmpty ? "Untitled" : article.title, quote: false)
        let title = link.isEmpty ? titleText : "<a href=\"\(RSSText.escape(link, quote: true))\">\(titleText)</a>"
        let date = RSSReaderText.articleDate(article.published, timeZone: timeZone, locale: locale)
        let dateLine = [date, minutes].filter { !$0.isEmpty }.joined(separator: " \u{00B7} ")
        let language = RSSLanguage.tag(article.language.isEmpty ? feed?.language ?? "" : article.language)
        let langAttribute = language.isEmpty ? "" : " lang=\"\(RSSText.escape(language, quote: true))\""
        let comments = RSSURL.http(body.commentsUrl)
        let commentsMarkup = comments.isEmpty || linksTo(content, comments) ? "" : "<p class=\"comments\"><a href=\"\(RSSText.escape(comments, quote: true))\">Comments</a></p>"
        return """
        <!DOCTYPE html><html\(langAttribute) dir="\(RSSLanguage.direction(language))"><head><meta charset="utf-8">\
        <meta http-equiv="Content-Security-Policy" content="\(style.policy)">\
        <meta name="viewport" content="width=device-width, initial-scale=1">\
        <meta name="referrer" content="no-referrer">\
        <style>\(style.css)</style></head><body>\
        <div class="byline"><div class="byline-text">\(feedMarkup)\(author)</div>\(tile)</div>\
        <hr class="rule"><h1 class="title">\(title)</h1>\
        <p class="date">\(RSSText.escape(dateLine, quote: false))</p>\
        \(enclosures(body.enclosures, remote: style.remoteImages))\
        <div class="body">\(content)</div>\(commentsMarkup)</body></html>
        """
    }
}
