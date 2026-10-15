package com.forkmesh.nativeapp.rssreader

interface RssHtmlHandler {
    fun start(tag: String, attrs: List<Pair<String, String>>, selfClosing: Boolean)
    fun end(tag: String)
    fun text(text: String)
}

object RssHtml {
    private val RAW = setOf("script", "style")

    fun tokenize(source: String, handler: RssHtmlHandler) {
        val text = source
        var index = 0
        val buffer = StringBuilder()
        fun flush() {
            if (buffer.isNotEmpty()) {
                handler.text(RssText.unescape(buffer.toString()))
                buffer.setLength(0)
            }
        }
        while (index < text.length) {
            val lt = text.indexOf('<', index)
            if (lt < 0) {
                buffer.append(text, index, text.length)
                break
            }
            buffer.append(text, index, lt)
            index = lt
            val next = text.getOrNull(index + 1)
            when {
                text.startsWith("<!--", index) -> {
                    flush()
                    val end = text.indexOf("-->", index + 4)
                    index = if (end < 0) text.length else end + 3
                }
                next == '!' || next == '?' -> {
                    flush()
                    val end = text.indexOf('>', index)
                    index = if (end < 0) text.length else end + 1
                }
                next == '/' && text.getOrNull(index + 2)?.isLetter() == true -> {
                    flush()
                    val end = text.indexOf('>', index)
                    val raw = if (end < 0) text.substring(index + 2) else text.substring(index + 2, end)
                    handler.end(raw.trim().substringBefore(' ').lowercase())
                    index = if (end < 0) text.length else end + 1
                }
                next != null && next.isLetter() -> {
                    flush()
                    val tag = readTag(text, index)
                    index = tag.end
                    handler.start(tag.name, tag.attrs, tag.selfClosing)
                    if (tag.name in RAW && !tag.selfClosing) {
                        val close = indexOfIgnoreCase(text, "</${tag.name}", index)
                        val body = if (close < 0) text.substring(index) else text.substring(index, close)
                        if (body.isNotEmpty()) handler.text(body)
                        if (close < 0) {
                            index = text.length
                        } else {
                            val end = text.indexOf('>', close)
                            index = if (end < 0) text.length else end + 1
                            handler.end(tag.name)
                        }
                    }
                }
                else -> {
                    buffer.append('<')
                    index++
                }
            }
        }
        flush()
    }

    private fun indexOfIgnoreCase(text: String, needle: String, from: Int): Int {
        var index = from
        while (true) {
            index = text.indexOf('<', index)
            if (index < 0) return -1
            if (text.regionMatches(index, needle, 0, needle.length, ignoreCase = true)) return index
            index++
        }
    }

    private class Tag(val name: String, val attrs: List<Pair<String, String>>, val selfClosing: Boolean, val end: Int)

    private fun readTag(text: String, start: Int): Tag {
        var index = start + 1
        val nameStart = index
        while (index < text.length && !text[index].isWhitespace() && text[index] != '>' && text[index] != '/') index++
        val name = text.substring(nameStart, index).lowercase()
        val attrs = ArrayList<Pair<String, String>>()
        while (index < text.length) {
            while (index < text.length && (text[index].isWhitespace())) index++
            if (index >= text.length) break
            if (text[index] == '>') return Tag(name, attrs, false, index + 1)
            if (text.startsWith("/>", index)) return Tag(name, attrs, true, index + 2)
            if (text[index] == '/') {
                index++
                continue
            }
            val attrStart = index
            while (index < text.length && !text[index].isWhitespace() && text[index] != '=' && text[index] != '>' && !text.startsWith("/>", index)) index++
            if (index == attrStart) {
                index++
                continue
            }
            val attrName = text.substring(attrStart, index).lowercase()
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
            attrs.add(attrName to RssText.unescape(value))
        }
        return Tag(name, attrs, false, text.length)
    }
}
