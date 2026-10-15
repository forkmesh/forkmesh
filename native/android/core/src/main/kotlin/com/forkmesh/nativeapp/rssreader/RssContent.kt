package com.forkmesh.nativeapp.rssreader

data class RssDiscoveredFeed(val url: String, val title: String, val type: String)

data class RssDiscovery(val feeds: List<RssDiscoveredFeed>, val icon: String, val title: String)

data class RssReadable(val title: String, val contentHtml: String)

object RssSanitizer {
    const val MAX_CONTENT_CHARS = 200_000
    const val MAX_SUMMARY_CHARS = 300

    val VOID_TAGS = setOf("br", "hr", "img", "source", "wbr")
    val ALLOWED_TAGS = setOf(
        "a", "abbr", "b", "blockquote", "br", "caption", "cite", "code", "dd", "del",
        "details", "dfn", "div", "dl", "dt", "em", "figcaption", "figure", "h1", "h2",
        "h3", "h4", "h5", "h6", "hr", "i", "img", "ins", "kbd", "li", "mark", "ol", "p",
        "pre", "q", "s", "samp", "section", "small", "span", "strike", "strong", "sub",
        "summary", "sup", "table", "tbody", "td", "tfoot", "th", "thead", "time", "tr",
        "u", "ul", "audio", "video", "source", "wbr", "article", "aside", "header",
        "footer", "main",
    )
    val DROPPED_TAGS = setOf(
        "script", "style", "template", "iframe", "object", "embed", "svg", "math",
        "head", "title", "textarea", "select", "button", "noscript", "canvas", "frame",
        "frameset", "applet", "noembed", "noframes", "dialog", "nav",
    )
    val ALLOWED_ATTRS: Map<String, List<String>> = mapOf(
        "a" to listOf("href", "title"),
        "img" to listOf("src", "alt", "title", "width", "height"),
        "td" to listOf("colspan", "rowspan"),
        "th" to listOf("colspan", "rowspan", "scope"),
        "ol" to listOf("start", "reversed"),
        "time" to listOf("datetime"),
        "abbr" to listOf("title"),
        "audio" to listOf("src", "controls"),
        "video" to listOf("src", "controls", "poster", "width", "height"),
        "source" to listOf("src", "type"),
        "details" to listOf("open"),
    )
    private val URL_ATTRS = setOf("href", "src", "poster")
    private val NUMERIC_ATTRS = setOf("width", "height", "colspan", "rowspan", "start")
    private val FLAG_ATTRS = setOf("controls", "reversed", "open")
    private val LAZY_SRC = listOf("data-src", "data-lazy-src", "data-original", "data-orig-file")
    private val SELF_VOID = setOf("embed", "frame")

    private class Sanitizer(val base: String) : RssHtmlHandler {
        val out = StringBuilder()
        val stack = ArrayList<String>()
        val drop = ArrayList<String>()

        fun emit(text: String) {
            if (out.length < MAX_CONTENT_CHARS) out.append(text)
        }

        fun attrs(tag: String, raw: List<Pair<String, String>>): List<Pair<String, String>> {
            val values = LinkedHashMap<String, String>()
            for ((name, value) in raw) if (name !in values) values[name] = value
            if (tag == "img") {
                val current = values["src"].orEmpty()
                if (current.isEmpty() || current.startsWith("data:")) {
                    LAZY_SRC.firstOrNull { !values[it].isNullOrEmpty() }?.let { values["src"] = values.getValue(it) }
                }
            }
            val kept = ArrayList<Pair<String, String>>()
            for (name in ALLOWED_ATTRS[tag].orEmpty()) {
                var value = values[name] ?: continue
                when (name) {
                    in URL_ATTRS -> {
                        value = if (name == "href" && value.trim().lowercase().startsWith("mailto:")) value.trim() else RssUrls.httpUrl(value, base)
                        if (value.isEmpty()) continue
                    }
                    in NUMERIC_ATTRS -> {
                        val trimmed = value.trim()
                        if (trimmed.isEmpty() || !trimmed.all { it in '0'..'9' }) continue
                        value = trimmed.take(6)
                    }
                    in FLAG_ATTRS -> value = ""
                    else -> value = RssText.clean(value, 300)
                }
                kept.add(name to value)
            }
            if (tag == "a") {
                kept.add("rel" to "noopener noreferrer")
                kept.add("target" to "_blank")
            }
            if (tag == "img") {
                kept.add("loading" to "lazy")
                kept.add("referrerpolicy" to "no-referrer")
            }
            if ((tag == "audio" || tag == "video") && kept.none { it.first == "controls" }) kept.add("controls" to "")
            return kept
        }

        override fun start(tag: String, attrs: List<Pair<String, String>>, selfClosing: Boolean) {
            if (drop.isNotEmpty()) {
                if (tag == drop.last() && tag !in VOID_TAGS && !selfClosing) drop.add(tag)
                return
            }
            if (tag in DROPPED_TAGS) {
                if (tag == "iframe") {
                    val src = RssUrls.httpUrl(attrs.firstOrNull { it.first == "src" }?.second, base)
                    if (src.isNotEmpty()) {
                        emit("<p><a href=\"${RssText.escape(src)}\" rel=\"noopener noreferrer\" target=\"_blank\">${RssText.escape("Open embedded content")}</a></p>")
                    }
                }
                if (!selfClosing && tag !in SELF_VOID) drop.add(tag)
                return
            }
            if (tag !in ALLOWED_TAGS) return
            val kept = attrs(tag, attrs)
            if ((tag == "img" || tag == "source") && kept.none { it.first == "src" }) return
            val rendered = StringBuilder()
            for ((name, value) in kept) {
                if (value.isNotEmpty() || name == "alt") rendered.append(' ').append(name).append("=\"").append(RssText.escape(value)).append('"')
                else rendered.append(' ').append(name)
            }
            emit("<$tag$rendered>")
            if (tag !in VOID_TAGS) {
                stack.add(tag)
                if (selfClosing && stack.lastOrNull() == tag) end(tag)
            }
        }

        override fun end(tag: String) {
            if (drop.isNotEmpty()) {
                if (tag == drop.last()) drop.removeAt(drop.lastIndex)
                return
            }
            if (tag !in stack) return
            while (stack.isNotEmpty()) {
                val open = stack.removeAt(stack.lastIndex)
                emit("</$open>")
                if (open == tag) break
            }
        }

        override fun text(text: String) {
            if (drop.isEmpty()) emit(RssText.escape(text, false))
        }

        fun result(): String {
            while (stack.isNotEmpty()) emit("</${stack.removeAt(stack.lastIndex)}>")
            return out.toString().trim()
        }
    }

    fun sanitize(value: String?, base: String = ""): String {
        val text = value.orEmpty()
        if (text.isBlank()) return ""
        return try {
            val sanitizer = Sanitizer(base)
            RssHtml.tokenize(text.take(MAX_CONTENT_CHARS * 2), sanitizer)
            sanitizer.result().take(MAX_CONTENT_CHARS + 2000)
        } catch (_: Exception) {
            RssText.escape(plainText(text, MAX_CONTENT_CHARS))
        }
    }

    private class Collector : RssHtmlHandler {
        val parts = StringBuilder()
        var drop = 0

        override fun start(tag: String, attrs: List<Pair<String, String>>, selfClosing: Boolean) {
            if (tag in DROPPED_TAGS) {
                if (!selfClosing && tag !in SELF_VOID) drop++
            } else if (tag in BLOCKS) {
                parts.append(' ')
            }
        }

        override fun end(tag: String) {
            if (tag in DROPPED_TAGS && drop > 0) drop-- else if (tag in BLOCKS) parts.append(' ')
        }

        override fun text(text: String) {
            if (drop == 0) parts.append(text)
        }

        companion object {
            val BLOCKS = setOf("p", "div", "br", "li", "h1", "h2", "h3", "h4", "h5", "h6", "tr", "blockquote", "pre", "section", "article")
        }
    }

    fun plainText(value: String?, limit: Int = MAX_SUMMARY_CHARS): String {
        var text = value.orEmpty()
        if ('<' in text || '&' in text) {
            text = runCatching {
                val collector = Collector()
                RssHtml.tokenize(text.take(400_000), collector)
                collector.parts.toString()
            }.getOrElse { text.replace(Regex("<[^>]*>"), " ") }
        }
        text = RssText.collapse(text)
        if (text.codePointCount(0, text.length) <= limit) return text
        val head = RssText.cut(text, limit)
        val space = head.lastIndexOf(' ')
        val cut = if (space > 0) head.substring(0, space) else head
        return cut + "…"
    }

    fun firstImage(content: String): String {
        val match = Regex("<img [^>]*src=\"([^\"]+)\"").find(content) ?: return ""
        return RssText.unescape(match.groupValues[1])
    }

    private class MediaLinks : RssHtmlHandler {
        val out = StringBuilder()
        var media: String? = null
        var mediaSrc = ""

        override fun start(tag: String, attrs: List<Pair<String, String>>, selfClosing: Boolean) {
            val src = attrs.firstOrNull { it.first == "src" }?.second.orEmpty()
            if (media != null) {
                if (tag == "source" && mediaSrc.isEmpty()) mediaSrc = src
                return
            }
            when (tag) {
                "img" -> {
                    val alt = attrs.firstOrNull { it.first == "alt" }?.second.orEmpty()
                    if (src.isNotEmpty()) out.append("<p><a href=\"").append(RssText.escape(src)).append("\">")
                        .append(RssText.escape(if (alt.isEmpty()) "Image" else "Image: $alt")).append("</a></p>")
                }
                "video", "audio" -> {
                    media = tag
                    mediaSrc = src
                    if (selfClosing) end(tag)
                }
                "source" -> {}
                else -> {
                    out.append('<').append(tag)
                    for ((name, value) in attrs) {
                        out.append(' ').append(name)
                        if (value.isNotEmpty() || name == "alt") out.append("=\"").append(RssText.escape(value)).append('"')
                    }
                    out.append('>')
                }
            }
        }

        override fun end(tag: String) {
            val open = media
            if (open != null) {
                if (tag == open) {
                    if (mediaSrc.isNotEmpty()) out.append("<p><a href=\"").append(RssText.escape(mediaSrc)).append("\">Open $open</a></p>")
                    media = null
                    mediaSrc = ""
                }
                return
            }
            if (tag in VOID_TAGS) return
            out.append("</").append(tag).append('>')
        }

        override fun text(text: String) {
            if (media == null) out.append(RssText.escape(text, false))
        }
    }

    fun withoutRemoteMedia(sanitized: String): String {
        if (sanitized.isEmpty()) return sanitized
        val links = MediaLinks()
        RssHtml.tokenize(sanitized, links)
        return links.out.toString()
    }
}

object RssPages {
    val FEED_TYPES = listOf(
        "application/rss+xml", "application/atom+xml", "application/feed+json",
        "application/json", "application/rdf+xml", "application/xml", "text/xml",
    )
    private val ORDER = mapOf(
        "application/rss+xml" to 0, "application/atom+xml" to 1, "application/feed+json" to 2,
        "application/json" to 3, "application/rdf+xml" to 4,
    )

    private class LinkFinder : RssHtmlHandler {
        val feeds = ArrayList<Triple<String, String, String>>()
        val icons = ArrayList<Pair<Boolean, String>>()
        var title = ""
        var inTitle = false
        var base = ""

        override fun start(tag: String, attrs: List<Pair<String, String>>, selfClosing: Boolean) {
            val values = attrs.associate { it.first to it.second }
            if (tag == "base" && !values["href"].isNullOrEmpty() && base.isEmpty()) base = values.getValue("href")
            if (tag == "title" && title.isEmpty() && !selfClosing) inTitle = true
            if (tag != "link") return
            val rel = values["rel"].orEmpty().lowercase().split(' ').filter { it.isNotEmpty() }
            val kind = values["type"].orEmpty().lowercase().substringBefore(';').trim()
            val href = values["href"].orEmpty()
            if (href.isEmpty()) return
            if ("alternate" in rel && kind in FEED_TYPES) feeds.add(Triple(kind, href, RssText.clean(values["title"], 200)))
            else if ("icon" in rel || "apple-touch-icon" in rel) icons.add(("apple-touch-icon" in rel) to href)
        }

        override fun end(tag: String) {
            if (tag == "title") inTitle = false
        }

        override fun text(text: String) {
            if (inTitle) title = RssText.clean(title + text, 200)
        }
    }

    fun discover(document: String, base: String): RssDiscovery {
        val finder = LinkFinder()
        runCatching { RssHtml.tokenize(document.take(600_000), finder) }
        val root = RssUrls.httpUrl(finder.base, base).ifEmpty { base }
        val feeds = ArrayList<RssDiscoveredFeed>()
        for ((kind, href, title) in finder.feeds.sortedBy { ORDER[it.first] ?: 5 }) {
            val url = RssUrls.httpUrl(href, root)
            if (url.isNotEmpty() && feeds.none { it.url == url }) feeds.add(RssDiscoveredFeed(url, title, kind))
        }
        val icon = finder.icons.sortedBy { !it.first }.map { RssUrls.httpUrl(it.second, root) }.firstOrNull { it.isNotEmpty() }.orEmpty()
        return RssDiscovery(feeds, icon, finder.title)
    }

    private class Node(val tag: String, val attrs: Map<String, String>, val parent: Node?) {
        val children = ArrayList<Any>()
    }

    private class TreeBuilder : RssHtmlHandler {
        val root = Node("root", emptyMap(), null)
        var current = root
        val skip = ArrayList<String>()
        val meta = HashMap<String, String>()
        var title = ""
        var inTitle = false

        override fun start(tag: String, attrs: List<Pair<String, String>>, selfClosing: Boolean) {
            val values = LinkedHashMap<String, String>()
            for ((name, value) in attrs) if (name !in values) values[name] = value
            if (tag == "meta") {
                val key = (values["property"]?.ifEmpty { null } ?: values["name"]).orEmpty().lowercase()
                if (key.isNotEmpty() && !values["content"].isNullOrEmpty() && key !in meta) meta[key] = values.getValue("content")
                return
            }
            if (tag == "title" && !selfClosing) inTitle = true
            if (skip.isNotEmpty()) {
                if (tag == skip.last() && tag !in VOID && !selfClosing) skip.add(tag)
                return
            }
            if (tag in SKIP) {
                if (!selfClosing && tag !in VOID) skip.add(tag)
                return
            }
            if ((tag == "p" || tag == "li") && (current.tag == "p" || current.tag == "li") && tag in AUTO_CLOSE[current.tag].orEmpty()) {
                current = current.parent ?: root
            }
            val node = Node(tag, values, current)
            current.children.add(node)
            if (tag !in VOID && !selfClosing) current = node
        }

        override fun end(tag: String) {
            if (tag == "title") inTitle = false
            if (skip.isNotEmpty()) {
                if (tag == skip.last()) skip.removeAt(skip.lastIndex)
                return
            }
            var node: Node? = current
            while (node != null && node.tag != tag) node = node.parent
            if (node?.parent != null) current = node.parent
        }

        override fun text(text: String) {
            if (inTitle) {
                title = RssText.clean(title + text, 300)
                return
            }
            if (skip.isEmpty() && text.isNotEmpty()) current.children.add(text)
        }

        companion object {
            val SKIP = setOf(
                "script", "style", "noscript", "template", "svg", "nav", "header", "footer", "aside", "form",
                "button", "select", "textarea", "iframe", "head",
            )
            val VOID = RssXml.VOID
            val AUTO_CLOSE = mapOf(
                "p" to setOf("p", "div", "ul", "ol", "table", "h1", "h2", "h3", "h4", "blockquote"),
                "li" to setOf("li"),
            )
        }
    }

    private fun nodeText(node: Node): String {
        val out = StringBuilder()
        fun walk(item: Any) {
            if (item is String) out.append(item) else (item as Node).children.forEach(::walk)
        }
        walk(node)
        return RssText.collapse(out.toString())
    }

    private fun serialize(node: Node): String {
        val out = StringBuilder()
        fun walk(item: Any) {
            if (item is String) {
                out.append(RssText.escape(item, false))
                return
            }
            val element = item as Node
            out.append('<').append(element.tag)
            for ((name, value) in element.attrs) out.append(' ').append(name).append("=\"").append(RssText.escape(value)).append('"')
            out.append('>')
            if (element.tag in TreeBuilder.VOID) return
            element.children.forEach(::walk)
            out.append("</").append(element.tag).append('>')
        }
        node.children.forEach(::walk)
        return out.toString()
    }

    private val SCORED = setOf("p", "pre", "blockquote", "li", "h2", "h3", "figure")

    private fun score(node: Node): Int {
        var total = 0
        for (child in node.children) {
            if (child is Node && child.tag in SCORED) {
                val length = nodeText(child).length
                total += length + if (child.tag == "p" && length > 80) 25 else 0
            }
        }
        return total
    }

    fun readable(document: String, base: String): RssReadable {
        val builder = TreeBuilder()
        try {
            RssHtml.tokenize(document.take(1_500_000), builder)
        } catch (_: Exception) {
            return RssReadable("", "")
        }
        var best: Node? = null
        var bestScore = 0
        val articles = ArrayList<Node>()
        val stack = ArrayDeque<Node>()
        stack.add(builder.root)
        while (stack.isNotEmpty()) {
            val node = stack.removeLast()
            if (node.tag == "article" || node.tag == "main" || "articlebody" in node.attrs["itemprop"].orEmpty().lowercase()) articles.add(node)
            val value = score(node)
            if (value > bestScore) {
                best = node
                bestScore = value
            }
            node.children.filterIsInstance<Node>().forEach(stack::addLast)
        }
        if (articles.isNotEmpty()) {
            val richest = articles.maxBy { nodeText(it).length }
            if (nodeText(richest).length >= bestScore * 0.6) best = richest
        }
        val title = RssText.clean(RssText.unescape(builder.meta["og:title"] ?: builder.title), 300)
        val chosen = best
        if (chosen == null || nodeText(chosen).length < 140) return RssReadable(title, "")
        return RssReadable(title, RssSanitizer.sanitize(serialize(chosen), base))
    }
}
