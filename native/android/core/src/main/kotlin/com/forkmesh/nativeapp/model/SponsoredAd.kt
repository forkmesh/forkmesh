package com.forkmesh.nativeapp.model

import java.net.URI

data class SponsoredAd(
    val id: String,
    val headline: String,
    val url: String,
    val body: String = "",
    val cta: String = DEFAULT_CTA,
    val sponsor: String = "",
    val categoryLabel: String = "",
) {
    val host: String get() = hostOf(url)

    companion object {
        const val MAX_HEADLINE = 60
        const val MAX_BODY = 160
        const val MAX_CTA = 24
        const val MAX_SPONSOR = 48
        const val MAX_CATEGORY_LABEL = 64
        const val MAX_URL_LENGTH = 2048
        const val DEFAULT_CTA = "Learn more"

        private val ID_PATTERN = Regex("^[0-9a-f]{32}$")
        private val WHITESPACE = Regex("\\s+")

        fun isValidId(id: String): Boolean = ID_PATTERN.matches(id)

        fun fromJson(json: Any?): SponsoredAd? {
            if (json !is Map<*, *>) return null
            val id = string(json["id"])
            if (!isValidId(id)) return null
            val headline = clamp(json["headline"], MAX_HEADLINE)
            if (headline.isEmpty()) return null
            val url = string(json["url"])
            if (!isSafeUrl(url)) return null
            val cta = clamp(json["cta"], MAX_CTA)
            return SponsoredAd(
                id = id,
                headline = headline,
                url = url,
                body = clamp(json["body"], MAX_BODY),
                cta = cta.ifEmpty { DEFAULT_CTA },
                sponsor = clamp(json["sponsor"], MAX_SPONSOR),
                categoryLabel = clamp(json["categoryLabel"], MAX_CATEGORY_LABEL),
            )
        }

        fun isSafeUrl(value: String): Boolean {
            if (value.isEmpty() || value.length > MAX_URL_LENGTH) return false
            if (WHITESPACE.containsMatchIn(value) || value.contains('\\')) return false
            if (!value.startsWith("https://")) return false
            val uri = try {
                URI(value)
            } catch (_: Exception) {
                return false
            }
            if (uri.scheme != "https" || uri.host.isNullOrEmpty()) return false
            if (uri.rawUserInfo != null) return false
            return true
        }

        private fun hostOf(url: String): String = try {
            URI(url).host.orEmpty()
        } catch (_: Exception) {
            ""
        }

        private fun string(value: Any?): String = (value as? String)?.trim().orEmpty()

        private fun clamp(value: Any?, max: Int): String {
            val clean = string(value).replace(WHITESPACE, " ")
            if (clean.length <= max) return clean
            val points = clean.codePointCount(0, clean.length)
            if (points <= max) return clean
            return clean.substring(0, clean.offsetByCodePoints(0, max)).trimEnd()
        }
    }
}
