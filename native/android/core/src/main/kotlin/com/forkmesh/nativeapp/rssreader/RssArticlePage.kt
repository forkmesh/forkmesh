package com.forkmesh.nativeapp.rssreader

import java.time.Instant
import java.time.ZoneId
import java.time.format.DateTimeFormatter
import java.util.Locale

object RssFormat {
    private val LONG = DateTimeFormatter.ofPattern("MMM d, yyyy 'at' h:mm a", Locale.US)
    private val TIME = DateTimeFormatter.ofPattern("h:mm a", Locale.US)
    private val DAY = DateTimeFormatter.ofPattern("MMM d", Locale.US)
    private val YEAR = DateTimeFormatter.ofPattern("MMM d, yyyy", Locale.US)

    fun articleDate(ms: Long, zone: ZoneId = ZoneId.systemDefault()): String =
        if (ms <= 0) "" else LONG.format(Instant.ofEpochMilli(ms).atZone(zone)).uppercase(Locale.US)

    fun time(ms: Long, zone: ZoneId = ZoneId.systemDefault()): String = TIME.format(Instant.ofEpochMilli(ms).atZone(zone))

    fun stamp(ms: Long, zone: ZoneId = ZoneId.systemDefault()): String =
        if (ms <= 0) "Never" else LONG.format(Instant.ofEpochMilli(ms).atZone(zone))

    fun short(ms: Long, now: Long, zone: ZoneId = ZoneId.systemDefault()): String {
        if (ms <= 0) return ""
        val date = Instant.ofEpochMilli(ms).atZone(zone)
        val today = Instant.ofEpochMilli(now).atZone(zone)
        return when {
            date.toLocalDate() == today.toLocalDate() -> TIME.format(date)
            date.year == today.year -> DAY.format(date)
            else -> YEAR.format(date)
        }
    }

    fun status(refreshing: Boolean, lastRefresh: Long, zone: ZoneId = ZoneId.systemDefault()): String = when {
        refreshing -> "Updating…"
        lastRefresh > 0 -> "Updated ${time(lastRefresh, zone)}"
        else -> ""
    }

    fun plural(count: Int, word: String): String = "$count $word${if (count == 1) "" else "s"}"

    fun shareText(article: RssArticle): String =
        listOf(RssText.clean(article.title, 300), article.link.trim()).filter { it.isNotEmpty() }.distinct().joinToString("\n")
}

data class RssPageStyle(
    val theme: RssTheme,
    val font: RssFont,
    val textSize: Int,
    val dark: Boolean,
    val accent: String,
    val loadRemoteImages: Boolean,
)

object RssArticlePage {
    private fun colors(style: RssPageStyle): Triple<String, String, String> = when (style.theme) {
        RssTheme.SEPIA -> if (style.dark) Triple("#2b2620", "#e8dcc8", "#a89a84") else Triple("#f4ecd8", "#3b2f22", "#7d6b55")
        RssTheme.PAPER -> if (style.dark) Triple("#1f1f1f", "#e6e6e6", "#9a9a9a") else Triple("#fbfbf8", "#222222", "#777777")
        RssTheme.CONTRAST -> if (style.dark) Triple("#000000", "#ffffff", "#d0d0d0") else Triple("#ffffff", "#000000", "#333333")
        RssTheme.DEFAULT -> if (style.dark) Triple("#121212", "#e8e8e8", "#9c9c9c") else Triple("#ffffff", "#1a1a1a", "#7a7a7a")
    }

    private fun family(font: RssFont): String = when (font) {
        RssFont.SANS -> "-apple-system, Roboto, 'Noto Sans', sans-serif"
        RssFont.SERIF -> "'Noto Serif', Georgia, serif"
        RssFont.MONO -> "'Roboto Mono', 'Noto Sans Mono', monospace"
    }

    fun source(article: RssArticle, reader: Boolean): String =
        if (reader && article.readerHtml.isNotEmpty()) article.readerHtml else article.contentHtml.ifEmpty {
            if (article.summary.isNotEmpty()) "<p>${RssText.escape(article.summary)}</p>" else ""
        }

    fun extraImages(article: RssArticle, html: String): List<String> =
        article.images.filter { it.isNotEmpty() && it !in html && RssText.escape(it) !in html }.distinct()

    fun body(article: RssArticle, reader: Boolean, loadRemoteImages: Boolean): String {
        val content = source(article, reader)
        val extra = extraImages(article, content)
        val html = if (extra.isEmpty()) content else RssSanitizer.sanitize(extra.joinToString("") { "<p><img src=\"${RssText.escape(it)}\" alt=\"\"></p>" }) + content
        return if (loadRemoteImages) RssSanitizer.withAltCaptions(html) else RssSanitizer.withoutRemoteMedia(html)
    }

    fun linksTo(html: String, url: String): Boolean {
        val target = RssUrls.linkKey(url)
        if (target.isEmpty() || html.isEmpty()) return false
        var found = false
        val finder = object : RssHtmlHandler {
            override fun start(tag: String, attrs: List<Pair<String, String>>, selfClosing: Boolean) {
                if (found || tag != "a") return
                val href = attrs.firstOrNull { it.first == "href" }?.second ?: return
                if (RssUrls.linkKey(href) == target) found = true
            }

            override fun end(tag: String) {}

            override fun text(text: String) {}
        }
        runCatching { RssHtml.tokenize(html, finder) }
        return found
    }

    fun readingTime(article: RssArticle, reader: Boolean): String {
        val minutes = RssRules.readingMinutes(RssSanitizer.plainText(source(article, reader), Int.MAX_VALUE))
        return if (minutes > 0) "$minutes min read" else ""
    }

    fun html(
        article: RssArticle,
        feedTitle: String,
        style: RssPageStyle,
        iconDataUri: String = "",
        reader: Boolean = false,
        feedLanguage: String = "",
    ): String {
        val (background, text, muted) = colors(style)
        val accent = style.accent
        val e = { value: String -> RssText.escape(value) }
        val letter = feedTitle.trim().firstOrNull()?.uppercaseChar()?.toString() ?: "?"
        val icon = if (iconDataUri.startsWith("data:image/")) "<img class=\"icon\" src=\"${e(iconDataUri)}\" alt=\"\">" else "<div class=\"icon letter\">${e(letter)}</div>"
        val title = if (article.link.isNotEmpty()) "<a href=\"${e(article.link)}\">${e(article.title.ifEmpty { "Untitled" })}</a>" else e(article.title.ifEmpty { "Untitled" })
        val media = article.enclosures.filter { !it.type.startsWith("image/") }.joinToString("") { enclosure ->
            val src = e(enclosure.url)
            when {
                style.loadRemoteImages && enclosure.type.startsWith("audio") -> "<p><audio controls preload=\"none\" src=\"$src\"></audio></p>"
                style.loadRemoteImages && enclosure.type.startsWith("video") -> "<p><video controls preload=\"none\" src=\"$src\"></video></p>"
                else -> "<p><a href=\"$src\">${e(if (enclosure.type.isNotEmpty()) "Download ${enclosure.type.substringBefore('/')} (${enclosure.type})" else "Download attachment")}</a></p>"
            }
        }
        val content = body(article, reader, style.loadRemoteImages)
        val comments = if (article.commentsUrl.isNotEmpty() && !linksTo(content, article.commentsUrl)) {
            "<p class=\"comments\"><a href=\"${e(article.commentsUrl)}\">Comments</a></p>"
        } else {
            ""
        }
        val author = if (article.author.isNotEmpty()) "<div class=\"author\">${e(article.author)}</div>" else ""
        val css = """
            :root { color-scheme: ${if (style.dark) "dark" else "light"}; }
            html, body { margin: 0; padding: 0; background: $background; color: $text; max-width: 100%; overflow-x: hidden; overscroll-behavior-x: none; touch-action: pan-y; }
            body { font-family: ${family(style.font)}; font-size: ${style.textSize}px; line-height: 1.55; padding: 16px 20px 48px; word-wrap: break-word; overflow-wrap: anywhere; }
            a { color: $accent; }
            .head { display: flex; align-items: flex-start; gap: 12px; }
            .who { flex: 1; min-width: 0; }
            .feed { color: $accent; font-weight: 600; font-size: 0.9em; }
            .author { color: $accent; font-size: 0.9em; }
            .icon { width: 48px; height: 48px; border-radius: 10px; object-fit: cover; flex: none; }
            .letter { display: flex; align-items: center; justify-content: center; background: $accent; color: #fff; font-weight: 700; font-size: 22px; }
            hr.rule { border: 0; border-top: 1px solid ${muted}55; margin: 12px 0 14px; }
            h1.title { font-size: 1.55em; line-height: 1.2; margin: 0 0 8px; font-weight: 700; }
            h1.title a { color: $text; text-decoration: none; }
            .date { color: $muted; font-size: 0.72em; letter-spacing: 0.04em; text-transform: uppercase; margin-bottom: 18px; }
            .content, .content * { max-width: 100%; box-sizing: border-box; }
            img, video, iframe, embed, object, svg, canvas { max-width: 100%; height: auto; }
            audio { width: 100%; }
            pre, code { font-family: ${family(RssFont.MONO)}; font-size: 0.88em; }
            code { white-space: pre-wrap; }
            pre { white-space: pre-wrap; overflow-x: hidden; overflow-wrap: anywhere; padding: 10px; background: ${muted}22; border-radius: 6px; }
            pre code { white-space: pre-wrap; font-size: 1em; }
            .alt-caption { display: block; color: $muted; font-size: 0.8em; line-height: 1.35; margin: 4px 0 10px; }
            blockquote { margin: 0 0 0 4px; padding-left: 12px; border-left: 3px solid ${muted}66; color: $muted; }
            table { border-collapse: collapse; width: 100%; max-width: 100%; table-layout: fixed; }
            td, th { border: 1px solid ${muted}44; padding: 4px 6px; overflow-wrap: anywhere; }
            figure { margin: 1em 0; }
            figcaption { color: $muted; font-size: 0.85em; }
        """.trimIndent()
        return buildString {
            val language = article.language.ifEmpty { feedLanguage }
            append("<!DOCTYPE html><html")
            if (language.isNotEmpty()) append(" lang=\"").append(e(language)).append('"')
            append(" dir=\"").append(RssRules.direction(language)).append("\"><head><meta charset=\"utf-8\">")
            append("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no\">")
            val remote = if (style.loadRemoteImages) "https: http: data:" else "data:"
            val mediaSrc = if (style.loadRemoteImages) "https: http:" else "'none'"
            append("<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; img-src $remote; media-src $mediaSrc; style-src 'unsafe-inline'\">")
            append("<style>").append(css).append("</style></head><body>")
            append("<div class=\"head\"><div class=\"who\"><div class=\"feed\">").append(e(feedTitle)).append("</div>").append(author).append("</div>").append(icon).append("</div>")
            append("<hr class=\"rule\">")
            append("<h1 class=\"title\">").append(title).append("</h1>")
            val dateLine = listOf(RssFormat.articleDate(article.published), readingTime(article, reader)).filter { it.isNotEmpty() }.joinToString(" · ")
            append("<div class=\"date\">").append(e(dateLine)).append("</div>")
            append("<div class=\"content\">").append(content).append("</div>")
            append(media).append(comments)
            append("</body></html>")
        }
    }
}
