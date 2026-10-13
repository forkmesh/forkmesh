package com.forkmesh.nativeapp.feature.code

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.code.CodeHighlighter
import com.forkmesh.nativeapp.code.DiffFileFilter
import com.forkmesh.nativeapp.code.codeLangForPath
import com.forkmesh.nativeapp.code.diff.DiffFile
import com.forkmesh.nativeapp.code.diff.DiffFileStatus
import com.forkmesh.nativeapp.code.diff.DiffLine
import com.forkmesh.nativeapp.code.diff.DiffLineKind
import com.forkmesh.nativeapp.code.diff.DiffRange
import com.forkmesh.nativeapp.ui.kit.FmChangeKind
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenu
import com.forkmesh.nativeapp.ui.kit.FmDropdownMenuItem
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmPalette
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType

@Stable
internal object DiffViewSettings {
    const val minFontSize = 9f
    const val maxFontSize = 18f

    private var store: PrefStore? = null

    var split by mutableStateOf(false)
        private set
    var wrap by mutableStateOf(true)
        private set
    var syntax by mutableStateOf(true)
        private set
    var autoViewed by mutableStateOf(true)
        private set
    var findings by mutableStateOf(false)
        private set
    var fontSize by mutableFloatStateOf(12f)
        private set

    fun attach(prefs: PrefStore?) {
        if (prefs == null || prefs === store) return
        store = prefs
        split = prefs.getBool("diff/split") ?: split
        wrap = prefs.getBool("diff/wrap") ?: wrap
        syntax = prefs.getBool("diff/syntax") ?: syntax
        autoViewed = prefs.getBool("diff/autoViewed") ?: autoViewed
        findings = prefs.getBool("diff/findings") ?: findings
        prefs.getDouble("diff/fontSize")?.let { fontSize = it.toFloat().coerceIn(minFontSize, maxFontSize) }
    }

    fun toggleSplit() {
        split = !split
        store?.putBool("diff/split", split)
    }

    fun toggleWrap() {
        wrap = !wrap
        store?.putBool("diff/wrap", wrap)
    }

    fun toggleSyntax() {
        syntax = !syntax
        store?.putBool("diff/syntax", syntax)
    }

    fun toggleAutoViewed() {
        autoViewed = !autoViewed
        store?.putBool("diff/autoViewed", autoViewed)
    }

    fun toggleFindings() {
        findings = !findings
        store?.putBool("diff/findings", findings)
    }

    fun resize(delta: Float) {
        fontSize = (fontSize + delta).coerceIn(minFontSize, maxFontSize)
        store?.putDouble("diff/fontSize", fontSize.toDouble())
    }
}

internal data class DiffMenuAction(val label: String, val icon: String, val onClick: () -> Unit)

internal fun diffChangeKind(status: DiffFileStatus): FmChangeKind = when (status) {
    DiffFileStatus.Added, DiffFileStatus.Copied -> FmChangeKind.Added
    DiffFileStatus.Deleted -> FmChangeKind.Deleted
    DiffFileStatus.Renamed -> FmChangeKind.Renamed
    DiffFileStatus.Modified -> FmChangeKind.Modified
}

@Composable
internal fun DiffStatSummary(additions: Int, deletions: Int, showBar: Boolean = true) {
    val c = Fm.colors
    val style = Fm.mono(weight = FmType.emphasis)
    val (green, red) = DiffFileFilter.statBar(additions, deletions)
    Row(verticalAlignment = Alignment.CenterVertically) {
        Text("+$additions", style = style.copy(color = c.success))
        Spacer(Modifier.width(FmSpace.x1))
        Text("−$deletions", style = style.copy(color = c.danger))
        if (showBar) {
            Spacer(Modifier.width(FmSpace.x2))
            for (i in 0 until 5) {
                Box(
                    Modifier
                        .padding(start = 1.5.dp)
                        .size(7.dp)
                        .background(
                            when {
                                i < green -> c.success
                                i < green + red -> c.danger
                                else -> c.bgOverlay
                            },
                            RoundedCornerShape(1.5.dp),
                        ),
                )
            }
        }
    }
}

@Composable
internal fun OpenDiffViewerLink(onOpen: () -> Unit) {
    val c = Fm.colors
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 44.dp)
            .clickable(onClick = onOpen)
            .padding(horizontal = FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        FmIcon("open_in_full", size = 18.dp, tint = c.accent)
        Spacer(Modifier.width(FmSpace.x2))
        Text("Open in diff viewer", style = Fm.text.labelLarge.copy(color = c.accent))
    }
}

@Composable
internal fun DiffViewMenu(extra: List<DiffMenuAction> = emptyList()) {
    var open by remember { mutableStateOf(false) }
    Box {
        FmIconButton("tune", "View options", onPressed = { open = true }, color = Fm.colors.textPrimary, iconSize = 24.dp)
        FmDropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            FmDropdownMenuItem(
                text = "Side by side",
                icon = if (DiffViewSettings.split) "check" else "crop_square",
                onClick = { DiffViewSettings.toggleSplit(); open = false },
            )
            FmDropdownMenuItem(
                text = "Wrap lines",
                icon = if (DiffViewSettings.wrap) "check" else "crop_square",
                onClick = { DiffViewSettings.toggleWrap(); open = false },
            )
            FmDropdownMenuItem(
                text = "Syntax highlighting",
                icon = if (DiffViewSettings.syntax) "check" else "crop_square",
                onClick = { DiffViewSettings.toggleSyntax(); open = false },
            )
            FmDropdownMenuItem(
                text = "Smaller text",
                icon = "text_decrease",
                enabled = DiffViewSettings.fontSize > DiffViewSettings.minFontSize,
                onClick = { DiffViewSettings.resize(-1f) },
            )
            FmDropdownMenuItem(
                text = "Larger text",
                icon = "text_increase",
                enabled = DiffViewSettings.fontSize < DiffViewSettings.maxFontSize,
                onClick = { DiffViewSettings.resize(1f) },
            )
            for (action in extra) {
                FmDropdownMenuItem(text = action.label, icon = action.icon, onClick = { open = false; action.onClick() })
            }
        }
    }
}

private fun applyEmphasis(base: AnnotatedString, ranges: List<DiffRange>, emphasis: Color): AnnotatedString {
    if (ranges.isEmpty()) return base
    return buildAnnotatedString {
        append(base)
        for (range in ranges) {
            val start = range.start.coerceIn(0, base.length)
            val end = range.end.coerceIn(start, base.length)
            if (end > start) addStyle(SpanStyle(background = emphasis), start, end)
        }
    }
}

@Stable
internal class DiffFileHighlights(val file: DiffFile) {
    val highlighter = CodeHighlighter(codeLangForPath(file.path))
    private val startsInComment = HashSet<DiffLine>()
    private val cache = HashMap<DiffLine, AnnotatedString>()
    private var cacheKey: Pair<Boolean, Boolean>? = null

    init {
        file.prepare()
        for (side in listOf(DiffLineKind.Removed, DiffLineKind.Added)) {
            val lines = file.hunks.flatMap { hunk -> hunk.lines.filter { it.kind == DiffLineKind.Context || it.kind == side } }
            val starts = highlighter.blockCommentStarts(lines.map { it.text })
            for (i in lines.indices) if (starts[i]) startsInComment.add(lines[i])
        }
    }

    fun spans(line: DiffLine, colors: FmPalette, syntax: Boolean): AnnotatedString {
        val key = colors.dark to syntax
        if (key != cacheKey) {
            cache.clear()
            cacheKey = key
        }
        cache[line]?.let { return it }
        val base = if (syntax) {
            buildAnnotatedString {
                for (token in highlighter.highlight(line.text, line in startsInComment)) {
                    withStyle(spanStyle(token.kind, colors)) { append(token.text) }
                }
            }
        } else {
            buildAnnotatedString { withStyle(SpanStyle(color = codeBaseColor(colors))) { append(line.text) } }
        }
        val emphasis = if (line.kind == DiffLineKind.Added) colors.success.copy(alpha = .32f) else colors.danger.copy(alpha = .30f)
        val spans = if (line.kind == DiffLineKind.Context) base else applyEmphasis(base, line.emphasis, emphasis)
        cache[line] = spans
        return spans
    }
}
