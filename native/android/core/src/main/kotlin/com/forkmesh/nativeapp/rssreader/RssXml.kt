package com.forkmesh.nativeapp.rssreader

class RssElement(val tag: String, val attrs: Map<String, String>, val parent: RssElement?) {
    val children = ArrayList<Any>()

    val elements: List<RssElement> get() = children.filterIsInstance<RssElement>()

    fun attr(name: String): String = attrs[name].orEmpty()

    fun child(vararg names: String): RssElement? {
        for (name in names) for (item in children) if (item is RssElement && item.tag == name) return item
        return null
    }

    fun all(name: String): List<RssElement> = children.filterIsInstance<RssElement>().filter { it.tag == name }

    fun text(): String {
        val out = StringBuilder()
        fun walk(node: RssElement) {
            for (item in node.children) if (item is String) out.append(item) else walk(item as RssElement)
        }
        walk(this)
        return out.toString().trim()
    }

    fun inner(): String {
        val out = StringBuilder()
        var leading = true
        for (item in children) {
            if (item is String) {
                out.append(if (leading) item else RssText.escape(item, false))
            } else {
                leading = false
                (item as RssElement).html(out)
            }
        }
        return out.toString().trim()
    }

    fun leadingText(): String = children.takeWhile { it is String }.joinToString("") { it as String }

    fun html(out: StringBuilder) {
        out.append('<').append(tag)
        for ((name, value) in attrs) out.append(' ').append(name).append("=\"").append(RssText.escape(value)).append('"')
        out.append('>')
        if (tag in RssXml.VOID) return
        for (item in children) if (item is String) out.append(RssText.escape(item, false)) else (item as RssElement).html(out)
        out.append("</").append(tag).append('>')
    }
}

class RssXmlException(message: String) : Exception(message)

object RssXml {
    val VOID = setOf("area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr")
    private const val MAX_DEPTH = 256
    private val KNOWN = setOf("dc", "content", "media", "itunes")

    private fun prefixFor(uri: String): String = when {
        "purl.org/dc" in uri -> "dc:"
        "purl.org/rss/1.0/modules/content" in uri -> "content:"
        "search.yahoo.com/mrss" in uri -> "media:"
        "itunes.com" in uri -> "itunes:"
        else -> ""
    }

    private fun local(name: String): String = name.substringAfter(':').lowercase()

    fun parse(source: String): RssElement {
        val text = source.trimStart('﻿', ' ', '\t', '\r', '\n')
        val root = RssElement("#document", emptyMap(), null)
        var current = root
        val scopes = ArrayList<Map<String, String>>()
        scopes.add(mapOf("xml" to "http://www.w3.org/XML/1998/namespace"))
        val rawNames = ArrayList<String>()
        var index = 0
        var sawElement = false
        fun lookup(prefix: String): String {
            for (i in scopes.indices.reversed()) scopes[i][prefix]?.let { return it }
            return ""
        }
        while (index < text.length) {
            val lt = text.indexOf('<', index)
            if (lt < 0) {
                if (current !== root) current.children.add(decode(text.substring(index)))
                break
            }
            if (lt > index && current !== root) current.children.add(decode(text.substring(index, lt)))
            index = lt
            when {
                text.startsWith("<!--", index) -> {
                    val end = text.indexOf("-->", index + 4)
                    index = if (end < 0) text.length else end + 3
                }
                text.startsWith("<![CDATA[", index) -> {
                    val end = text.indexOf("]]>", index + 9)
                    val body = if (end < 0) text.substring(index + 9) else text.substring(index + 9, end)
                    if (current !== root) current.children.add(body)
                    index = if (end < 0) text.length else end + 3
                }
                text.startsWith("<?", index) -> {
                    val end = text.indexOf("?>", index + 2)
                    index = if (end < 0) text.length else end + 2
                }
                text.startsWith("<!", index) -> index = skipDeclaration(text, index)
                text.startsWith("</", index) -> {
                    val end = text.indexOf('>', index)
                    val name = (if (end < 0) text.substring(index + 2) else text.substring(index + 2, end)).trim()
                    index = if (end < 0) text.length else end + 1
                    val depth = rawNames.lastIndexOf(name)
                    if (depth >= 0) {
                        while (rawNames.size > depth) {
                            rawNames.removeAt(rawNames.lastIndex)
                            scopes.removeAt(scopes.lastIndex)
                            current = current.parent ?: root
                        }
                    }
                }
                else -> {
                    val tag = readTag(text, index)
                    if (tag == null) {
                        if (current !== root) current.children.add("<")
                        index++
                        continue
                    }
                    index = tag.end
                    if (rawNames.size >= MAX_DEPTH) continue
                    val declared = HashMap<String, String>()
                    for ((name, value) in tag.attrs) {
                        if (name == "xmlns") declared[""] = value
                        else if (name.startsWith("xmlns:")) declared[name.substring(6)] = value
                    }
                    scopes.add(declared)
                    val prefix = if (tag.name.contains(':')) tag.name.substringBefore(':') else ""
                    val uri = lookup(prefix)
                    val name = if (uri.isNotEmpty()) prefixFor(uri) + local(tag.name) else if (prefix.lowercase() in KNOWN) prefix.lowercase() + ":" + local(tag.name) else tag.name.lowercase()
                    val attrs = LinkedHashMap<String, String>()
                    for ((attrName, value) in tag.attrs) {
                        if (attrName == "xmlns" || attrName.startsWith("xmlns:")) continue
                        attrs[local(attrName)] = value
                    }
                    val element = RssElement(name, attrs, current)
                    if (current === root && sawElement) {
                        scopes.removeAt(scopes.lastIndex)
                        continue
                    }
                    sawElement = true
                    current.children.add(element)
                    if (tag.selfClosing) {
                        scopes.removeAt(scopes.lastIndex)
                    } else {
                        rawNames.add(tag.name)
                        current = element
                    }
                }
            }
        }
        return root.elements.firstOrNull() ?: throw RssXmlException("feed_unreadable")
    }

    private fun skipDeclaration(text: String, start: Int): Int {
        var index = start + 2
        var bracket = 0
        while (index < text.length) {
            when (text[index]) {
                '[' -> bracket++
                ']' -> bracket--
                '>' -> if (bracket <= 0) return index + 1
            }
            index++
        }
        return text.length
    }

    private class Tag(val name: String, val attrs: List<Pair<String, String>>, val selfClosing: Boolean, val end: Int)

    private fun readTag(text: String, start: Int): Tag? {
        var index = start + 1
        if (index >= text.length || !(text[index].isLetter() || text[index] == '_')) return null
        val nameStart = index
        while (index < text.length && !text[index].isWhitespace() && text[index] != '>' && text[index] != '/') index++
        val name = text.substring(nameStart, index)
        val attrs = ArrayList<Pair<String, String>>()
        while (index < text.length) {
            while (index < text.length && text[index].isWhitespace()) index++
            if (index >= text.length) return Tag(name, attrs, false, text.length)
            if (text[index] == '>') return Tag(name, attrs, false, index + 1)
            if (text.startsWith("/>", index)) return Tag(name, attrs, true, index + 2)
            if (text[index] == '/') {
                index++
                continue
            }
            val attrStart = index
            while (index < text.length && !text[index].isWhitespace() && text[index] != '=' && text[index] != '>' && !text.startsWith("/>", index)) index++
            val attrName = text.substring(attrStart, index)
            while (index < text.length && text[index].isWhitespace()) index++
            var value = ""
            if (index < text.length && text[index] == '=') {
                index++
                while (index < text.length && text[index].isWhitespace()) index++
                if (index < text.length && (text[index] == '"' || text[index] == '\'')) {
                    val quote = text[index]
                    val close = text.indexOf(quote, index + 1)
                    value = if (close < 0) text.substring(index + 1) else text.substring(index + 1, close)
                    index = if (close < 0) text.length else close + 1
                } else {
                    val valueStart = index
                    while (index < text.length && !text[index].isWhitespace() && text[index] != '>') index++
                    value = text.substring(valueStart, index)
                }
            }
            if (attrName.isNotEmpty()) attrs.add(attrName to decode(value))
        }
        return Tag(name, attrs, false, text.length)
    }

    fun decode(value: String): String {
        if (value.indexOf('&') < 0 && value.indexOf('\u0000') < 0) return value
        val out = StringBuilder(value.length)
        var index = 0
        while (index < value.length) {
            val ch = value[index]
            if (ch == '&') {
                val decoded = RssText.decodeAt(value, index)
                if (decoded != null) {
                    out.appendCodePoint(decoded.first)
                    index = decoded.second
                    continue
                }
            }
            if (ch.code in 0..8 || ch.code == 11 || ch.code == 12 || ch.code in 14..31) {
                index++
                continue
            }
            out.append(ch)
            index++
        }
        return out.toString()
    }
}
