package com.forkmesh.nativeapp.ui.theme

import android.content.Context
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.size
import androidx.compose.material3.LocalContentColor
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.PlatformTextStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.font.Font
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.R

object FmIcons {
    const val NOTIFICATION_LINE = "mingcute:notification_line"

    val materialFamily = FontFamily(Font(R.font.material_icons_regular))
    val mingCuteFamily = FontFamily(Font(R.font.mingcute_notification))

    @Volatile
    private var codepoints: Map<String, Int>? = null

    private fun codepoints(context: Context): Map<String, Int> {
        codepoints?.let { return it }
        synchronized(this) {
            codepoints?.let { return it }
            val loaded = HashMap<String, Int>(9000)
            runCatching {
                context.assets.open("fonts/MaterialIcons.codepoints").bufferedReader().useLines { lines ->
                    for (line in lines) {
                        val space = line.indexOf(' ')
                        if (space <= 0) continue
                        line.substring(space + 1).trim().toIntOrNull(16)?.let { loaded[line.substring(0, space)] = it }
                    }
                }
            }
            codepoints = loaded
            return loaded
        }
    }

    fun has(context: Context, name: String): Boolean =
        name == NOTIFICATION_LINE || codepoints(context).containsKey(name)

    fun glyph(context: Context, name: String): String {
        if (name == NOTIFICATION_LINE) return String(Character.toChars(0xf127))
        val table = codepoints(context)
        val code = table[name] ?: table["${name}_baseline"]
            ?: table["help_outline"] ?: table["help_outline_baseline"] ?: return "?"
        return String(Character.toChars(code))
    }

    fun family(name: String): FontFamily = if (name == NOTIFICATION_LINE) mingCuteFamily else materialFamily
}

@Composable
fun FmIcon(
    name: String,
    modifier: Modifier = Modifier,
    size: Dp = 24.dp,
    tint: Color = LocalContentColor.current,
    contentDescription: String? = null,
) {
    val context = LocalContext.current
    val glyph = remember(name) { FmIcons.glyph(context, name) }
    val fontSize = with(LocalDensity.current) { size.toSp() }
    Box(
        modifier = modifier
            .size(size)
            .semantics { if (contentDescription != null) this.contentDescription = contentDescription },
        contentAlignment = Alignment.Center,
    ) {
        Text(
            text = glyph,
            color = tint,
            style = TextStyle(
                fontFamily = FmIcons.family(name),
                fontSize = fontSize,
                lineHeight = fontSize,
                platformStyle = PlatformTextStyle(includeFontPadding = false),
            ),
        )
    }
}
