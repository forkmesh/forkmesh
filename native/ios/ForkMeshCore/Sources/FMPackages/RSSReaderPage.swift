import FMBase
import Foundation

public enum RSSReaderText {
    public static let title = "RSS Reader"
    public static let feedsTitle = "Feeds"
    public static let smartFeeds = "Smart Feeds"
    public static let onThisIPhone = "On This iPhone"
    public static let netNewsWireURL = "https://netnewswire.com/"
    public static let netNewsWireSourceURL = "https://github.com/Ranchero-Software/NetNewsWire"
    public static let credit =
        "Inspired by NetNewsWire, the free and open source RSS reader created by Brent Simmons and developed by Ranchero Software and its contributors (MIT licensed). RSS Reader shares no code with NetNewsWire and is not affiliated with or endorsed by it."
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
        html{-webkit-text-size-adjust:100%;}
        body{margin:0;padding:16px 20px 120px;background:var(--bg);color:var(--fg);font-family:\(font.css);font-size:\(size)px;line-height:1.55;word-wrap:break-word;overflow-wrap:anywhere;}
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
        pre{padding:12px;overflow-x:auto;white-space:pre-wrap;}
        code{padding:1px 4px;}
        pre code{padding:0;background:none;}
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
        guard !list.isEmpty else { return "" }
        var out = "<div class=\"enclosures\">"
        for item in list {
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

    public static func html(
        article: RSSArticle, body: RSSArticleBody, feed: RSSFeed?, settings: RSSReaderSettings, reader: Bool,
        timeZone: TimeZone = .current, locale: Locale = .current
    ) -> String {
        let style = RSSPageStyle(settings: settings)
        let link = RSSURL.http(article.link)
        let site = RSSURL.http(feed?.siteUrl ?? "")
        let base = link.isEmpty ? site : link
        let feedName = feed?.displayTitle ?? "Feed"
        let source = reader && !body.readerHtml.isEmpty ? body.readerHtml : body.contentHtml
        var content = RSSHTML.sanitize(source, base: base, remoteMedia: style.remoteImages, upgradeMedia: true)
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
        let icon = RSSURL.upgraded(RSSURL.http(feed?.iconAddress ?? ""))
        let tile = style.remoteImages && !icon.isEmpty
            ? "<div class=\"tile\"><img src=\"\(RSSText.escape(icon, quote: true))\" alt=\"\" referrerpolicy=\"no-referrer\"></div>"
            : "<div class=\"tile\">\(RSSText.escape(RSSText.letter(feedName), quote: false))</div>"
        let titleText = RSSText.escape(article.title.isEmpty ? "Untitled" : article.title, quote: false)
        let title = link.isEmpty ? titleText : "<a href=\"\(RSSText.escape(link, quote: true))\">\(titleText)</a>"
        let comments = RSSURL.http(body.commentsUrl)
        let commentsMarkup = comments.isEmpty ? "" : "<p class=\"comments\"><a href=\"\(RSSText.escape(comments, quote: true))\">Comments</a></p>"
        return """
        <!DOCTYPE html><html><head><meta charset="utf-8">\
        <meta http-equiv="Content-Security-Policy" content="\(style.policy)">\
        <meta name="viewport" content="width=device-width, initial-scale=1">\
        <meta name="referrer" content="no-referrer">\
        <style>\(style.css)</style></head><body>\
        <div class="byline"><div class="byline-text">\(feedMarkup)\(author)</div>\(tile)</div>\
        <hr class="rule"><h1 class="title">\(title)</h1>\
        <p class="date">\(RSSText.escape(RSSReaderText.articleDate(article.published, timeZone: timeZone, locale: locale), quote: false))</p>\
        \(enclosures(body.enclosures, remote: style.remoteImages))\
        <div class="body">\(content)</div>\(commentsMarkup)</body></html>
        """
    }
}
