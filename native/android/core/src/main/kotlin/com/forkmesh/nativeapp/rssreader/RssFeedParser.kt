package com.forkmesh.nativeapp.rssreader

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.JsonMap
import com.forkmesh.nativeapp.json.asJsonMap
import com.forkmesh.nativeapp.json.jsonLong
import com.forkmesh.nativeapp.json.jsonText

data class RssEnclosure(val url: String, val type: String = "", val length: Long = 0)

data class RssParsedItem(
    val guid: String = "",
    val title: String = "",
    val url: String = "",
    val externalUrl: String = "",
    val author: String = "",
    val published: Long = 0,
    val updated: Long = 0,
    val contentHtml: String = "",
    val summaryHtml: String = "",
    val commentsUrl: String = "",
    val enclosures: List<RssEnclosure> = emptyList(),
)

data class RssParsedFeed(
    val title: String = "",
    val siteUrl: String = "",
    val iconUrl: String = "",
    val description: String = "",
    val items: List<RssParsedItem> = emptyList(),
)

class RssFeedException(val code: String) : Exception(code)

object RssGuid {
    const val MAX = 600

    fun of(item: RssParsedItem): String {
        val own = RssText.clean(item.guid, MAX)
        val link = RssText.clean(item.url, MAX)
        val value = when {
            own.isNotEmpty() -> own
            link.isNotEmpty() -> link
            else -> RssText.clean(RssText.clean(item.title, 300) + "|" + (if (item.published > 0) item.published.toString() else ""), MAX)
        }
        return RssText.trimPipes(value)
    }
}

object RssFeedParser {
    const val MAX_ITEMS = 200
    private val AUTHOR_EMAIL = Regex("^\\S+@\\S+\\s+\\((.*)\\)$")
    private val TITLE_MARKUP = Regex("</[a-zA-Z][^>]*>|<br\\s*/?>", RegexOption.IGNORE_CASE)

    fun parse(document: String, base: String = ""): RssParsedFeed {
        val text = document.trimStart('﻿', ' ', '\t', '\r', '\n')
        if (text.isEmpty()) throw RssFeedException("empty_feed")
        if (text.startsWith("{")) {
            val data = runCatching { Json.parse(text) }.getOrNull() as? Map<*, *> ?: throw RssFeedException("not_a_feed")
            val map = data.asJsonMap()
            if ("jsonfeed.org" !in jsonText(map["version"])) throw RssFeedException("not_a_feed")
            return jsonFeed(map, base)
        }
        if (!text.startsWith("<")) throw RssFeedException("not_a_feed")
        val head = text.take(2000).lowercase()
        if ("<html" in head && "<rss" !in head && "<feed" !in head && "<rdf" !in head) throw RssFeedException("not_a_feed")
        val root = try {
            RssXml.parse(text)
        } catch (_: RssXmlException) {
            throw RssFeedException("feed_unreadable")
        }
        return when (root.tag) {
            "feed" -> atomFeed(root, base)
            "rss", "rdf" -> rssFeed(root, base)
            else -> throw RssFeedException("not_a_feed")
        }
    }

    private fun text(element: RssElement?): String = element?.text().orEmpty()

    private fun enclosures(items: List<Triple<String, String, String>>): List<RssEnclosure> {
        val result = ArrayList<RssEnclosure>()
        for ((url, kind, length) in items) {
            val clean = RssUrls.httpUrl(url)
            if (clean.isNotEmpty() && result.none { it.url == clean }) {
                result.add(RssEnclosure(clean, RssText.clean(kind, 80).lowercase(), maxOf(0L, jsonLong(length.trim()))))
            }
        }
        return result.take(4)
    }

    private fun rssTitle(element: RssElement?): String {
        val value = if (element != null && element.elements.isNotEmpty()) element.inner() else text(element)
        if ('<' in value && TITLE_MARKUP.containsMatchIn(value)) return RssSanitizer.plainText(value, 1000)
        return RssText.clean(RssText.unescape(value), 1000)
    }

    private fun rssItems(container: RssElement, base: String): List<RssParsedItem> =
        container.all("item").take(MAX_ITEMS).map { item ->
            var link = RssUrls.httpUrl(text(item.child("link")), base)
            val guidNode = item.child("guid")
            val guid = text(guidNode)
            if (link.isEmpty() && guid.isNotEmpty() && guidNode!!.attr("ispermalink").ifEmpty { "true" }.lowercase() != "false") {
                link = RssUrls.httpUrl(guid, base)
            }
            val content = item.child("content:encoded")?.inner().orEmpty()
            val description = item.child("description")
            val summaryHtml = if (description != null && description.elements.isNotEmpty()) description.inner() else text(description)
            val author = text(item.child("dc:creator", "author", "itunes:author"))
            val media = item.all("enclosure").map { Triple(it.attr("url"), it.attr("type"), it.attr("length")) } +
                item.all("media:content").filter {
                    val kind = it.attrs["medium"]?.ifEmpty { null } ?: it.attr("type")
                    kind.startsWith("audio") || kind.startsWith("video")
                }.map { Triple(it.attr("url"), it.attr("type"), it.attr("filesize")) }
            RssParsedItem(
                guid = guid.ifEmpty { link }.ifEmpty { item.attr("about") },
                title = rssTitle(item.child("title")),
                url = link.ifEmpty { RssUrls.httpUrl(item.attr("about"), base) },
                author = RssText.clean(AUTHOR_EMAIL.replace(author, "$1"), 200),
                published = RssDates.parse(text(item.child("pubdate", "dc:date", "date"))),
                updated = RssDates.parse(text(item.child("updated", "dc:modified"))),
                contentHtml = content.ifEmpty { summaryHtml },
                summaryHtml = if (content.isNotEmpty()) summaryHtml else "",
                commentsUrl = RssUrls.httpUrl(text(item.child("comments")), base),
                enclosures = enclosures(media),
            )
        }

    private fun rssFeed(root: RssElement, base: String): RssParsedFeed {
        val channel = root.child("channel") ?: throw RssFeedException("not_a_feed")
        val site = RssUrls.httpUrl(text(channel.child("link")), base).ifEmpty { base }
        var image = channel.child("image")
        var icon = image?.let { RssUrls.httpUrl(text(it.child("url")), base) }.orEmpty()
        if (icon.isEmpty()) icon = channel.child("itunes:image")?.let { RssUrls.httpUrl(it.attr("href"), base) }.orEmpty()
        val container = if (channel.all("item").isNotEmpty()) channel else root
        if (root.tag == "rdf" && image == null) {
            image = root.child("image")
            if (icon.isEmpty()) icon = image?.let { RssUrls.httpUrl(text(it.child("url")), base) }.orEmpty()
        }
        return RssParsedFeed(
            title = rssTitle(channel.child("title")),
            siteUrl = site,
            iconUrl = icon,
            description = RssSanitizer.plainText(text(channel.child("description")), 500),
            items = rssItems(container, site),
        )
    }

    private fun atomText(element: RssElement?, asHtml: Boolean = false): String {
        if (element == null) return ""
        val kind = element.attr("type").ifEmpty { "text" }.lowercase()
        if (kind == "xhtml" || kind == "application/xhtml+xml") {
            val holder = element.child("div") ?: element
            val content = holder.inner()
            return if (asHtml) content else RssSanitizer.plainText(content, 1000)
        }
        if (kind == "html" || kind == "text/html") {
            val content = if (element.elements.isNotEmpty()) element.inner() else element.leadingText()
            return if (asHtml) content.trim() else RssSanitizer.plainText(content, 1000)
        }
        val value = text(element)
        return if (asHtml) RssText.escape(value).replace("\n\n", "<br><br>") else RssText.clean(value, 1000)
    }

    private fun atomLink(element: RssElement, base: String, rel: String = "alternate"): String {
        var fallback = ""
        for (link in element.all("link")) {
            val href = RssUrls.httpUrl(link.attr("href"), base)
            if (href.isEmpty()) continue
            val kind = link.attr("rel").ifEmpty { "alternate" }.lowercase()
            if (kind == rel) {
                val type = link.attr("type").ifEmpty { "text/html" }
                if (rel != "alternate" || type.startsWith("text/html") || type.startsWith("application/xhtml")) return href
                if (fallback.isEmpty()) fallback = href
            }
        }
        return fallback
    }

    private fun atomFeed(root: RssElement, base: String): RssParsedFeed {
        val site = atomLink(root, base).ifEmpty { base }
        val feedAuthor = root.child("author")?.let { text(it.child("name")) }.orEmpty()
        val items = root.all("entry").take(MAX_ITEMS).map { entry ->
            val url = atomLink(entry, base)
            val content = atomText(entry.child("content"), true)
            val summary = atomText(entry.child("summary"), true)
            val author = entry.child("author")?.let { text(it.child("name")) }.orEmpty()
            val media = entry.all("link").filter { it.attr("rel").lowercase() == "enclosure" }.map { Triple(it.attr("href"), it.attr("type"), it.attr("length")) }
            RssParsedItem(
                guid = text(entry.child("id")).ifEmpty { url },
                title = atomText(entry.child("title")),
                url = url,
                author = RssText.clean(author.ifEmpty { feedAuthor }, 200),
                published = RssDates.parse(text(entry.child("published", "issued", "created"))),
                updated = RssDates.parse(text(entry.child("updated", "modified"))),
                contentHtml = content.ifEmpty { summary },
                summaryHtml = if (content.isNotEmpty()) summary else "",
                commentsUrl = atomLink(entry, base, "replies"),
                enclosures = enclosures(media),
            )
        }
        return RssParsedFeed(
            title = atomText(root.child("title")),
            siteUrl = site,
            iconUrl = RssUrls.httpUrl(text(root.child("icon", "logo")), base),
            description = atomText(root.child("subtitle")),
            items = items,
        )
    }

    private fun jsonAuthor(value: Any?): String = when (value) {
        is Map<*, *> -> RssText.clean(jsonText(value["name"]), 200)
        is List<*> -> value.take(4).map(::jsonAuthor).filter { it.isNotEmpty() }.joinToString(", ")
        else -> ""
    }

    private fun jsonFeed(data: JsonMap, base: String): RssParsedFeed {
        val site = RssUrls.httpUrl(jsonText(data["home_page_url"]), base).ifEmpty { base }
        val items = (data["items"] as? List<*>).orEmpty().take(MAX_ITEMS).mapNotNull { raw ->
            val item = (raw as? Map<*, *>)?.asJsonMap() ?: return@mapNotNull null
            var content = jsonText(item["content_html"])
            val plain = jsonText(item["content_text"])
            if (content.isEmpty() && plain.isNotEmpty()) {
                content = plain.split("\n\n").filter { it.isNotBlank() }.joinToString("") { "<p>" + RssText.escape(it).replace("\n", "<br>") + "</p>" }
            }
            val summary = jsonText(item["summary"])
            val url = RssUrls.httpUrl(jsonText(item["url"]), site)
            val attachments = (item["attachments"] as? List<*>).orEmpty().mapNotNull { it as? Map<*, *> }
                .map { Triple(jsonText(it["url"]), jsonText(it["mime_type"]), jsonText(it["size_in_bytes"])) }
            RssParsedItem(
                guid = RssText.clean(jsonText(item["id"]), RssGuid.MAX).ifEmpty { url },
                title = RssText.clean(jsonText(item["title"]), 1000),
                url = url,
                externalUrl = RssUrls.httpUrl(jsonText(item["external_url"]), site),
                author = jsonAuthor(item["authors"] ?: item["author"]).ifEmpty { jsonAuthor(data["authors"] ?: data["author"]) },
                published = RssDates.parse(jsonText(item["date_published"])),
                updated = RssDates.parse(jsonText(item["date_modified"])),
                contentHtml = content.ifEmpty { RssText.escape(summary) },
                summaryHtml = if (content.isNotEmpty() && summary.isNotEmpty()) RssText.escape(summary) else "",
                enclosures = enclosures(attachments),
            )
        }
        return RssParsedFeed(
            title = RssText.clean(jsonText(data["title"]), 1000),
            siteUrl = site,
            iconUrl = RssUrls.httpUrl(jsonText(data["icon"]).ifEmpty { jsonText(data["favicon"]) }, base),
            description = RssText.clean(jsonText(data["description"]), 500),
            items = items,
        )
    }
}
