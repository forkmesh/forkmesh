package com.forkmesh.nativeapp.rssreader

import java.util.regex.Pattern
import kotlin.math.ceil

class RssMatcher(val pattern: String, val regex: Boolean) {
    private val compiled: Pattern? =
        if (regex && pattern.isNotEmpty()) runCatching { Pattern.compile(pattern, Pattern.CASE_INSENSITIVE or Pattern.UNICODE_CASE) }.getOrNull() else null

    val valid: Boolean get() = !regex || pattern.isEmpty() || compiled != null

    fun matches(value: String): Boolean {
        if (pattern.isEmpty()) return false
        if (regex) return compiled?.matcher(value)?.find() == true
        return value.contains(pattern, ignoreCase = true)
    }

    companion object {
        const val INVALID_REGEX = "Invalid regular expression"

        fun valid(pattern: String, regex: Boolean): Boolean = RssMatcher(pattern, regex).valid
    }
}

enum class RssCatchUp(val label: String, val days: Int) {
    DAY("Older Than 1 Day", 1),
    THREE_DAYS("Older Than 3 Days", 3),
    WEEK("Older Than 1 Week", 7),
    TWO_WEEKS("Older Than 2 Weeks", 14),
    MONTH("Older Than 1 Month", 30);

    fun cutoff(now: Long): Long = now - days * 24L * 60 * 60 * 1000
}

object RssRules {
    const val MAX_TEXT = 20_000
    const val WORDS_PER_MINUTE = 230.0
    const val CJK_PER_MINUTE = 500.0
    private val RTL = setOf("ar", "he", "iw", "fa", "ur", "yi", "ps", "sd", "ug", "dv", "ckb", "syr")
    private val WHITESPACE = Regex("\\s+")

    fun articleDate(article: RssArticle): Long = if (article.published > 0) article.published else article.arrived

    fun text(article: RssArticle): String = RssSanitizer.plainText(article.contentHtml.ifEmpty { article.summary }, MAX_TEXT)

    fun filterApplies(filter: RssFilter, article: RssArticle): Boolean = filter.feedId.isEmpty() || filter.feedId == article.feedId

    fun fieldMatches(field: RssFilterField, matcher: RssMatcher, article: RssArticle, text: () -> String): Boolean = when (field) {
        RssFilterField.TITLE -> matcher.matches(article.title)
        RssFilterField.AUTHOR -> matcher.matches(article.author)
        RssFilterField.TITLE_OR_TEXT -> matcher.matches(article.title) || matcher.matches(text())
    }

    fun filterMatches(filter: RssFilter, matcher: RssMatcher, article: RssArticle, text: () -> String = { text(article) }): Boolean =
        filterApplies(filter, article) && fieldMatches(filter.field, matcher, article, text)

    fun smartMatches(
        smart: RssSmartFeed,
        matcher: RssMatcher,
        article: RssArticle,
        folderId: String,
        now: Long,
        text: () -> String = { text(article) },
    ): Boolean {
        when {
            smart.scope.startsWith("feed:") -> if (article.feedId != smart.scope.removePrefix("feed:")) return false
            smart.scope.startsWith("folder:") -> if (folderId.isEmpty() || folderId != smart.scope.removePrefix("folder:")) return false
            smart.scope.isNotEmpty() -> return false
        }
        if (smart.unreadOnly && article.read) return false
        if (smart.starredOnly && !article.starred) return false
        if (smart.maxAgeDays > 0 && articleDate(article) < now - smart.maxAgeDays * 24L * 60 * 60 * 1000) return false
        if (smart.words.isEmpty()) return true
        return fieldMatches(RssFilterField.TITLE_OR_TEXT, matcher, article, text)
    }

    fun language(raw: String?): String {
        val text = raw?.trim().orEmpty().replace('_', '-')
        if (text.isEmpty()) return ""
        return text.takeWhile { it.isLetterOrDigit() || it == '-' }.take(35).trim('-')
    }

    fun direction(language: String): String {
        val primary = language.substringBefore('-').lowercase()
        return if (primary in RTL) "rtl" else "auto"
    }

    private fun isCjk(code: Int): Boolean = when (runCatching { Character.UnicodeScript.of(code) }.getOrNull()) {
        Character.UnicodeScript.HAN, Character.UnicodeScript.HIRAGANA, Character.UnicodeScript.KATAKANA, Character.UnicodeScript.HANGUL -> true
        else -> false
    }

    fun readingMinutes(text: String): Int {
        if (text.isBlank()) return 0
        var cjk = 0
        val rest = StringBuilder(text.length)
        var index = 0
        while (index < text.length) {
            val code = text.codePointAt(index)
            index += Character.charCount(code)
            if (isCjk(code)) cjk++ else rest.appendCodePoint(code)
        }
        val words = rest.split(WHITESPACE).count { it.isNotEmpty() }
        return maxOf(1, ceil(words / WORDS_PER_MINUTE + cjk / CJK_PER_MINUTE).toInt())
    }

    fun linkIdentity(url: String): String = if (url.isEmpty()) "" else RssUrls.identity(url)
}

object RssFeedFinder {
    const val YOUTUBE_FEED = "https://www.youtube.com/feeds/videos.xml"
    private val YOUTUBE_HOSTS = setOf("youtube.com", "www.youtube.com", "m.youtube.com", "youtu.be")
    private val CHANNEL = Regex("^UC[\\w-]{22}$")
    private val JSON_CHANNEL = Regex("\"channelId\"\\s*:\\s*\"(UC[\\w-]{22})\"")
    private val META_TAG = Regex("<meta\\b[^>]*>", RegexOption.IGNORE_CASE)
    private val LINK_TAG = Regex("<link\\b[^>]*>", RegexOption.IGNORE_CASE)
    private val ITEMPROP_CHANNEL = Regex("itemprop\\s*=\\s*[\"']channelId[\"']", RegexOption.IGNORE_CASE)
    private val CONTENT_ATTR = Regex("content\\s*=\\s*[\"'](UC[\\w-]{22})[\"']")
    private val CANONICAL = Regex("rel\\s*=\\s*[\"']canonical[\"']", RegexOption.IGNORE_CASE)
    private val CHANNEL_PATH = Regex("/channel/(UC[\\w-]{22})")
    private val QUERY_SAFE = Regex("^[\\w.~%-]+$")

    fun isYouTube(url: String): Boolean {
        val parts = RssUrls.split(url) ?: return false
        return (parts.scheme == "http" || parts.scheme == "https") && parts.host in YOUTUBE_HOSTS
    }

    fun channelFeed(id: String): String = "$YOUTUBE_FEED?channel_id=$id"

    private fun segments(path: String): List<String> = path.split('/').filter { it.isNotEmpty() }

    private fun param(query: String?, name: String): String =
        query.orEmpty().split('&').firstOrNull { it.substringBefore('=') == name }?.substringAfter('=', "").orEmpty()

    fun youTubeFeed(url: String): String {
        if (!isYouTube(url)) return ""
        val parts = RssUrls.split(url) ?: return ""
        if (parts.host == "youtu.be") return ""
        val path = segments(parts.path)
        val first = path.firstOrNull().orEmpty()
        return when {
            first == "channel" && path.size >= 2 && CHANNEL.matches(path[1]) -> channelFeed(path[1])
            first == "playlist" -> param(parts.query, "list").takeIf { it.isNotEmpty() && QUERY_SAFE.matches(it) }?.let { "$YOUTUBE_FEED?playlist_id=$it" }.orEmpty()
            first == "user" && path.size >= 2 && QUERY_SAFE.matches(path[1]) -> "$YOUTUBE_FEED?user=${path[1]}"
            else -> ""
        }
    }

    fun youTubeNeedsPage(url: String): Boolean {
        if (!isYouTube(url)) return false
        val parts = RssUrls.split(url) ?: return false
        val path = segments(parts.path)
        val first = path.firstOrNull() ?: return false
        if (parts.host == "youtu.be") return true
        return when {
            first.startsWith("@") && first.length > 1 -> true
            first == "c" && path.size >= 2 -> true
            first == "watch" -> param(parts.query, "v").isNotEmpty()
            first == "shorts" && path.size >= 2 -> true
            else -> false
        }
    }

    fun youTubeChannelId(html: String): String {
        JSON_CHANNEL.find(html)?.let { return it.groupValues[1] }
        for (tag in META_TAG.findAll(html)) {
            if (ITEMPROP_CHANNEL.containsMatchIn(tag.value)) CONTENT_ATTR.find(tag.value)?.let { return it.groupValues[1] }
        }
        for (tag in LINK_TAG.findAll(html)) {
            if (CANONICAL.containsMatchIn(tag.value)) CHANNEL_PATH.find(tag.value)?.let { return it.groupValues[1] }
        }
        return ""
    }

    fun mastodonFeed(url: String): String {
        if (isYouTube(url)) return ""
        val parts = RssUrls.split(url) ?: return ""
        if ((parts.scheme != "http" && parts.scheme != "https") || parts.host.isEmpty()) return ""
        val path = parts.path.removeSuffix("/")
        if (!path.startsWith("/@") || path.length < 3 || path.indexOf('/', 1) >= 0) return ""
        if (path.endsWith(".rss")) return ""
        return parts.origin + path + ".rss"
    }

    fun blueskyFeed(url: String): String {
        val parts = RssUrls.split(url) ?: return ""
        if ((parts.scheme != "http" && parts.scheme != "https") || parts.host != "bsky.app") return ""
        val path = segments(parts.path)
        if (path.size != 2 || path[0] != "profile") return ""
        return "https://bsky.app/profile/${path[1]}/rss"
    }

    fun candidates(url: String): List<String> =
        listOf(youTubeFeed(url), mastodonFeed(url), blueskyFeed(url)).filter { it.isNotEmpty() }.distinct()
}
