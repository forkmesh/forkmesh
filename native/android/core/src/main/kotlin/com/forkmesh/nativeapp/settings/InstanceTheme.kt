package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.api.HttpTransport

object InstanceTheme {
    fun instanceColors(tokens: Map<String, Long>): Map<ThemeToken, Long> {
        val colors = LinkedHashMap<ThemeToken, Long>()
        fun put(token: ThemeToken, rgb: Long?) {
            if (rgb != null) colors[token] = rgb
        }
        put(ThemeToken.bgBase, tokens["background"])
        put(ThemeToken.bgRaised, tokens["card"])
        put(ThemeToken.bgOverlay, tokens["popover"] ?: tokens["muted"])
        put(ThemeToken.bgElevated, tokens["muted"] ?: tokens["secondary"])
        put(ThemeToken.border, tokens["border"])
        put(ThemeToken.textPrimary, tokens["foreground"])
        val muted = tokens["muted-foreground"]
        if (muted != null) {
            put(ThemeToken.textSecondary, tokens["foreground"]?.let { ThemeColor.mix(muted, it, 0.35) } ?: muted)
        } else {
            put(ThemeToken.textSecondary, tokens["secondary-foreground"])
        }
        put(ThemeToken.textTertiary, muted)
        put(ThemeToken.accent, tokens["primary"])
        put(ThemeToken.onAccent, tokens["primary-foreground"])
        put(ThemeToken.success, tokens["success"])
        put(ThemeToken.danger, tokens["destructive"])
        put(ThemeToken.warning, tokens["attention"])
        return colors
    }

    fun theme(themeColors: Map<*, *>, readable: Boolean = true): AppTheme? {
        fun decode(mode: String): Map<ThemeToken, Long> {
            val entries = themeColors[mode] as? Map<*, *> ?: return emptyMap()
            val tokens = LinkedHashMap<String, Long>()
            for ((name, value) in entries) {
                val rgb = (value as? String)?.let(ThemeColor::parseHex) ?: continue
                tokens[name as? String ?: continue] = rgb
            }
            return instanceColors(tokens)
        }
        val light = decode("light")
        val dark = decode("dark")
        if (light.isEmpty() && dark.isEmpty()) return null
        return AppTheme(readable = readable, lightColors = light, darkColors = dark)
    }

    fun themeColors(body: Map<*, *>): Map<*, *> {
        (body["themeColors"] as? Map<*, *>)?.let { return it }
        val stored = body["theme"] as? Map<*, *> ?: return emptyMap<String, Any?>()
        return listOf("light", "dark").mapNotNull { mode -> stored[mode]?.let { mode to it } }.toMap()
    }

    fun colorsJson(body: Map<*, *>): String = theme(themeColors(body))?.colorsJson().orEmpty()

    fun fetch(transport: HttpTransport, origin: String, timeoutMs: Int = 8000): String? {
        if (origin.isEmpty()) return null
        val response = runCatching {
            transport.send(HttpClient.Request.get("$origin/api/instance").header("Accept", "application/json").timeouts(timeoutMs, timeoutMs))
        }.getOrNull() ?: return null
        if (response.status != 200) return null
        val body = runCatching { Json.parse(response.text()) }.getOrNull() as? Map<*, *> ?: return null
        if (body["ok"] != true && body["theme"] == null && body["themeColors"] == null) return null
        return colorsJson(body)
    }

    fun effective(own: AppTheme, matching: Boolean, instanceColors: String?): AppTheme {
        if (!matching) return own
        val colors = AppTheme.parseColors(instanceColors)
        if (colors.light.isEmpty() && colors.dark.isEmpty()) return own
        return AppTheme(readable = own.readable, lightColors = colors.light, darkColors = colors.dark)
    }
}
