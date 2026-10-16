package com.forkmesh.nativeapp.rssreader

import java.time.Instant
import java.time.ZoneOffset
import java.time.format.DateTimeFormatter

data class RssOpmlEntry(val url: String, val title: String, val folder: String, val siteUrl: String)

object RssOpml {
    fun parse(document: String): List<RssOpmlEntry> {
        val root = try {
            RssXml.parse(document)
        } catch (_: RssXmlException) {
            throw RssFeedException("opml_unreadable")
        }
        val body = (if (root.tag == "opml") root.child("body") else null) ?: throw RssFeedException("opml_unreadable")
        val entries = ArrayList<RssOpmlEntry>()
        fun walk(node: RssElement, folder: String) {
            for (child in node.all("outline")) {
                val url = RssUrls.normalizeFeedInput(child.attr("xmlurl"))
                val title = RssText.clean(child.attr("title").ifEmpty { child.attr("text") }, 200)
                if (url.isNotEmpty()) {
                    entries.add(RssOpmlEntry(url, title, folder, RssUrls.httpUrl(child.attr("htmlurl"))))
                } else if (child.elements.isNotEmpty()) {
                    walk(child, folder.ifEmpty { title.ifEmpty { "Imported" } })
                }
            }
        }
        walk(body, "")
        return entries
    }

    fun build(folders: List<RssFolder>, feeds: List<RssFeed>, now: Long, owner: String = "this phone"): String {
        val stamp = DateTimeFormatter.RFC_1123_DATE_TIME.format(Instant.ofEpochMilli(now).atOffset(ZoneOffset.UTC))
        val lines = arrayListOf(
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>",
            "<opml version=\"1.1\">",
            "  <head>",
            "    <title>${RssText.escape("RSS Reader subscriptions from $owner")}</title>",
            "    <dateCreated>$stamp</dateCreated>",
            "  </head>",
            "  <body>",
        )
        fun outline(feed: RssFeed, indent: String): String {
            val title = RssText.escape(feed.displayTitle)
            val home = if (feed.siteUrl.isNotEmpty()) " htmlUrl=\"${RssText.escape(feed.siteUrl)}\"" else ""
            return "$indent<outline text=\"$title\" title=\"$title\" type=\"rss\" version=\"RSS\" xmlUrl=\"${RssText.escape(feed.url)}\"$home/>"
        }
        val known = folders.map { it.id }.toSet()
        val byName = compareBy<RssFeed, String>(String.CASE_INSENSITIVE_ORDER) { it.displayTitle }
        feeds.filter { it.folderId.isEmpty() || it.folderId !in known }.sortedWith(byName).forEach { lines.add(outline(it, "    ")) }
        for (folder in folders.sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.name })) {
            val name = RssText.escape(folder.name)
            lines.add("    <outline text=\"$name\" title=\"$name\">")
            feeds.filter { it.folderId == folder.id }.sortedWith(byName).forEach { lines.add(outline(it, "      ")) }
            lines.add("    </outline>")
        }
        lines.add("  </body>")
        lines.add("</opml>")
        lines.add("")
        return lines.joinToString("\n")
    }
}
