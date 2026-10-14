package com.forkmesh.nativeapp.market

import java.net.URI

object FaceChangerPage {
    const val PACKAGE_ID = "face-changer"
    const val PATH = "/dashboard/face-changer"

    fun url(origin: String): String = origin.trimEnd('/') + PATH

    fun host(url: String?): String? =
        runCatching { URI(url ?: return null).host?.lowercase() }.getOrNull()?.takeIf { it.isNotEmpty() }

    fun isPageHost(url: String?, origin: String): Boolean {
        val expected = host(origin) ?: return false
        return host(url) == expected
    }

    fun staysInView(url: String?, origin: String): Boolean {
        val uri = runCatching { URI(url ?: return false) }.getOrNull() ?: return false
        val secure = uri.scheme == "https" || (uri.scheme == "http" && runCatching { URI(origin).scheme }.getOrNull() == "http")
        return secure && isPageHost(url, origin)
    }
}
