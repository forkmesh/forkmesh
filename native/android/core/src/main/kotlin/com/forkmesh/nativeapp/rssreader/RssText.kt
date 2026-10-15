package com.forkmesh.nativeapp.rssreader

object RssText {
    fun isSpace(code: Int): Boolean =
        code == ' '.code || code in 0x09..0x0d || code in 0x1c..0x1f || code == 0x85 || code == 0xa0 ||
            code == 0x1680 || code in 0x2000..0x200a || code == 0x2028 || code == 0x2029 || code == 0x202f ||
            code == 0x205f || code == 0x3000

    fun collapse(value: String?): String {
        val text = value ?: return ""
        val out = StringBuilder(text.length)
        var pending = false
        var index = 0
        while (index < text.length) {
            val code = text.codePointAt(index)
            index += Character.charCount(code)
            if (code == 0) continue
            if (isSpace(code)) {
                pending = out.isNotEmpty()
                continue
            }
            if (pending) out.append(' ')
            pending = false
            out.appendCodePoint(code)
        }
        return out.toString()
    }

    fun cut(value: String, limit: Int): String {
        if (value.length <= limit) return value
        val points = value.codePointCount(0, value.length)
        if (points <= limit) return value
        return value.substring(0, value.offsetByCodePoints(0, limit))
    }

    fun clean(value: String?, limit: Int): String = cut(collapse(value), limit)

    fun trimPipes(value: String): String = value.trim('|')

    fun escape(value: String, quote: Boolean = true): String {
        val out = StringBuilder(value.length + 16)
        for (ch in value) {
            when (ch) {
                '&' -> out.append("&amp;")
                '<' -> out.append("&lt;")
                '>' -> out.append("&gt;")
                '"' -> if (quote) out.append("&quot;") else out.append(ch)
                '\'' -> if (quote) out.append("&#x27;") else out.append(ch)
                else -> out.append(ch)
            }
        }
        return out.toString()
    }

    private val ENTITIES: Map<String, Int> = mapOf(
        "amp" to 38, "lt" to 60, "gt" to 62, "quot" to 34, "apos" to 39, "nbsp" to 160, "iexcl" to 161,
        "cent" to 162, "pound" to 163, "curren" to 164, "yen" to 165, "brvbar" to 166, "sect" to 167,
        "uml" to 168, "copy" to 169, "ordf" to 170, "laquo" to 171, "not" to 172, "shy" to 173, "reg" to 174,
        "macr" to 175, "deg" to 176, "plusmn" to 177, "sup2" to 178, "sup3" to 179, "acute" to 180,
        "micro" to 181, "para" to 182, "middot" to 183, "cedil" to 184, "sup1" to 185, "ordm" to 186,
        "raquo" to 187, "frac14" to 188, "frac12" to 189, "frac34" to 190, "iquest" to 191, "Agrave" to 192,
        "Aacute" to 193, "Acirc" to 194, "Atilde" to 195, "Auml" to 196, "Aring" to 197, "AElig" to 198,
        "Ccedil" to 199, "Egrave" to 200, "Eacute" to 201, "Ecirc" to 202, "Euml" to 203, "Igrave" to 204,
        "Iacute" to 205, "Icirc" to 206, "Iuml" to 207, "ETH" to 208, "Ntilde" to 209, "Ograve" to 210,
        "Oacute" to 211, "Ocirc" to 212, "Otilde" to 213, "Ouml" to 214, "times" to 215, "Oslash" to 216,
        "Ugrave" to 217, "Uacute" to 218, "Ucirc" to 219, "Uuml" to 220, "Yacute" to 221, "THORN" to 222,
        "szlig" to 223, "agrave" to 224, "aacute" to 225, "acirc" to 226, "atilde" to 227, "auml" to 228,
        "aring" to 229, "aelig" to 230, "ccedil" to 231, "egrave" to 232, "eacute" to 233, "ecirc" to 234,
        "euml" to 235, "igrave" to 236, "iacute" to 237, "icirc" to 238, "iuml" to 239, "eth" to 240,
        "ntilde" to 241, "ograve" to 242, "oacute" to 243, "ocirc" to 244, "otilde" to 245, "ouml" to 246,
        "divide" to 247, "oslash" to 248, "ugrave" to 249, "uacute" to 250, "ucirc" to 251, "uuml" to 252,
        "yacute" to 253, "thorn" to 254, "yuml" to 255, "OElig" to 338, "oelig" to 339, "Scaron" to 352,
        "scaron" to 353, "Yuml" to 376, "fnof" to 402, "circ" to 710, "tilde" to 732, "Alpha" to 913,
        "Beta" to 914, "Gamma" to 915, "Delta" to 916, "Omega" to 937, "alpha" to 945, "beta" to 946,
        "gamma" to 947, "delta" to 948, "epsilon" to 949, "lambda" to 955, "mu" to 956, "pi" to 960,
        "sigma" to 963, "omega" to 969, "ensp" to 8194, "emsp" to 8195, "thinsp" to 8201, "zwnj" to 8204,
        "zwj" to 8205, "lrm" to 8206, "rlm" to 8207, "ndash" to 8211, "mdash" to 8212, "lsquo" to 8216,
        "rsquo" to 8217, "sbquo" to 8218, "ldquo" to 8220, "rdquo" to 8221, "bdquo" to 8222, "dagger" to 8224,
        "Dagger" to 8225, "bull" to 8226, "hellip" to 8230, "permil" to 8240, "prime" to 8242, "Prime" to 8243,
        "lsaquo" to 8249, "rsaquo" to 8250, "oline" to 8254, "frasl" to 8260, "euro" to 8364, "trade" to 8482,
        "larr" to 8592, "uarr" to 8593, "rarr" to 8594, "darr" to 8595, "harr" to 8596, "minus" to 8722,
        "infin" to 8734, "ne" to 8800, "le" to 8804, "ge" to 8805, "loz" to 9674, "hearts" to 9829,
    )

    fun entity(name: String): Int? = ENTITIES[name]

    fun unescape(value: String): String {
        if (value.indexOf('&') < 0) return value
        val out = StringBuilder(value.length)
        var index = 0
        while (index < value.length) {
            val ch = value[index]
            if (ch != '&') {
                out.append(ch)
                index++
                continue
            }
            val decoded = decodeAt(value, index)
            if (decoded == null) {
                out.append(ch)
                index++
            } else {
                out.appendCodePoint(decoded.first)
                index = decoded.second
            }
        }
        return out.toString()
    }

    fun decodeAt(value: String, start: Int): Pair<Int, Int>? {
        var index = start + 1
        if (index >= value.length) return null
        if (value[index] == '#') {
            index++
            val hex = index < value.length && (value[index] == 'x' || value[index] == 'X')
            if (hex) index++
            val digitsStart = index
            while (index < value.length && index - digitsStart < 8 &&
                (value[index].isDigit() || (hex && value[index].lowercaseChar() in 'a'..'f'))
            ) index++
            if (index == digitsStart) return null
            val code = value.substring(digitsStart, index).toIntOrNull(if (hex) 16 else 10) ?: return null
            if (index < value.length && value[index] == ';') index++
            val safe = if (code == 0 || code > 0x10ffff || code in 0xd800..0xdfff) 0xfffd else code
            return safe to index
        }
        val nameStart = index
        while (index < value.length && index - nameStart < 32 && value[index].isLetterOrDigit()) index++
        if (index == nameStart) return null
        val name = value.substring(nameStart, index)
        val code = ENTITIES[name] ?: return null
        if (index < value.length && value[index] == ';') index++ else if (name !in setOf("amp", "lt", "gt", "quot", "nbsp", "copy", "reg")) return null
        return code to index
    }
}
