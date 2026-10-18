package com.forkmesh.nativeapp.feature.securedrive

import android.graphics.Typeface
import android.text.Editable
import android.text.Html
import android.text.InputType
import android.text.Spannable
import android.text.SpannableStringBuilder
import android.text.Spanned
import android.text.TextWatcher
import android.text.style.BulletSpan
import android.text.style.CharacterStyle
import android.text.style.ParagraphStyle
import android.text.style.RelativeSizeSpan
import android.text.style.StyleSpan
import android.text.style.UnderlineSpan
import android.view.Gravity
import android.view.View
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.Stroke as DrawStroke
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.graphics.toArgb
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.forkmesh.nativeapp.securedrive.Drawing
import com.forkmesh.nativeapp.securedrive.Point
import com.forkmesh.nativeapp.securedrive.Sheet
import com.forkmesh.nativeapp.securedrive.Stroke
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin

internal object SdHtml {
    private val WRAPPER = Regex("^<p[^>]*>(.*)</p>$", RegexOption.DOT_MATCHES_ALL)

    fun fromHtml(html: String): SpannableStringBuilder {
        val built = SpannableStringBuilder(Html.fromHtml(html, Html.FROM_HTML_MODE_COMPACT))
        while (built.isNotEmpty() && built[built.length - 1] == '\n') built.delete(built.length - 1, built.length)
        return built
    }

    fun toHtml(text: Spanned): String {
        val out = StringBuilder("<html><body>")
        var inList = false
        var start = 0
        while (true) {
            val newline = text.indexOf('\n', start)
            val end = if (newline < 0) text.length else newline
            val probeEnd = max(end, min(start + 1, text.length))
            val bullet = text.getSpans(start, probeEnd, BulletSpan::class.java).isNotEmpty()
            val heading = end > start && text.getSpans(start, end, RelativeSizeSpan::class.java).any {
                it.sizeChange > 1.1f && text.getSpanStart(it) <= start && text.getSpanEnd(it) >= end
            }
            val paragraph = SpannableStringBuilder(text, start, end)
            for (span in paragraph.getSpans(0, paragraph.length, Any::class.java)) {
                if (span is ParagraphStyle || span is RelativeSizeSpan || span !is CharacterStyle) paragraph.removeSpan(span)
            }
            val inner = inline(paragraph)
            if (bullet && !inList) {
                out.append("<ul>")
                inList = true
            }
            if (!bullet && inList) {
                out.append("</ul>")
                inList = false
            }
            when {
                bullet -> out.append("<li>").append(inner).append("</li>")
                heading -> out.append("<h2>").append(inner).append("</h2>")
                else -> out.append("<p>").append(inner.ifEmpty { "<br>" }).append("</p>")
            }
            if (newline < 0) break
            start = newline + 1
        }
        if (inList) out.append("</ul>")
        out.append("</body></html>")
        return out.toString()
    }

    private fun inline(text: Spanned): String {
        if (text.isEmpty()) return ""
        val html = Html.toHtml(text, Html.TO_HTML_PARAGRAPH_LINES_CONSECUTIVE).trim()
        return WRAPPER.find(html)?.groupValues?.get(1)?.trim() ?: html
    }

    private fun paragraphBounds(text: Spannable, from: Int, to: Int): IntRange {
        var start = from
        while (start > 0 && text[start - 1] != '\n') start--
        var end = to
        while (end < text.length && text[end] != '\n') end++
        return start..end
    }

    fun toggleStyle(edit: EditText, style: Int?) {
        val text = edit.text
        val s = min(edit.selectionStart, edit.selectionEnd)
        val e = max(edit.selectionStart, edit.selectionEnd)
        if (s < 0 || s == e) return
        val existing: List<CharacterStyle> = if (style == null) {
            text.getSpans(s, e, UnderlineSpan::class.java).toList()
        } else {
            text.getSpans(s, e, StyleSpan::class.java).filter { it.style == style }
        }
        if (existing.isEmpty()) {
            text.setSpan(if (style == null) UnderlineSpan() else StyleSpan(style), s, e, Spanned.SPAN_EXCLUSIVE_INCLUSIVE)
            return
        }
        for (span in existing) {
            val spanStart = text.getSpanStart(span)
            val spanEnd = text.getSpanEnd(span)
            text.removeSpan(span)
            if (spanStart < s) text.setSpan(if (style == null) UnderlineSpan() else StyleSpan(style), spanStart, s, Spanned.SPAN_EXCLUSIVE_INCLUSIVE)
            if (spanEnd > e) text.setSpan(if (style == null) UnderlineSpan() else StyleSpan(style), e, spanEnd, Spanned.SPAN_EXCLUSIVE_INCLUSIVE)
        }
    }

    fun toggleHeading(edit: EditText) {
        val text = edit.text
        val range = paragraphBounds(text, max(0, edit.selectionStart), max(0, edit.selectionEnd))
        val sizes = text.getSpans(range.first, range.last, RelativeSizeSpan::class.java)
        if (sizes.isNotEmpty()) {
            sizes.forEach(text::removeSpan)
            text.getSpans(range.first, range.last, StyleSpan::class.java)
                .filter { it.style == Typeface.BOLD && text.getSpanStart(it) == range.first && text.getSpanEnd(it) == range.last }
                .forEach(text::removeSpan)
            return
        }
        if (range.last <= range.first) return
        text.setSpan(RelativeSizeSpan(1.5f), range.first, range.last, Spanned.SPAN_EXCLUSIVE_INCLUSIVE)
        text.setSpan(StyleSpan(Typeface.BOLD), range.first, range.last, Spanned.SPAN_EXCLUSIVE_INCLUSIVE)
    }

    fun toggleList(edit: EditText) {
        val text = edit.text
        val range = paragraphBounds(text, max(0, edit.selectionStart), max(0, edit.selectionEnd))
        val bullets = text.getSpans(range.first, max(range.last, min(range.first + 1, text.length)), BulletSpan::class.java)
        if (bullets.isNotEmpty()) {
            bullets.forEach(text::removeSpan)
            return
        }
        var start = range.first
        while (start <= range.last) {
            var end = start
            while (end < text.length && text[end] != '\n') end++
            val spanEnd = if (end < text.length) end + 1 else end
            if (spanEnd > start) text.setSpan(BulletSpan(24), start, spanEnd, Spanned.SPAN_INCLUSIVE_EXCLUSIVE)
            start = end + 1
        }
    }
}

@Composable
internal fun SdDocumentEditor(bytes: ByteArray, editor: SdEditorState, readOnly: Boolean) {
    val c = Fm.colors
    val textColor = c.textPrimary.toArgb()
    val hintColor = c.textTertiary.toArgb()
    var field by remember { mutableStateOf<EditText?>(null) }
    editor.serialize = { field?.let { SdHtml.toHtml(it.text).toByteArray(Charsets.UTF_8) } ?: bytes.copyOf() }
    Column(Modifier.fillMaxSize().imePadding()) {
        if (!readOnly) {
            Row(
                Modifier.fillMaxWidth().background(c.bgRaised).horizontalScroll(rememberScrollState()).padding(horizontal = FmSpace.x2),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                ToolText("B", "Bold", weight = FontWeight.Bold) { field?.let { SdHtml.toggleStyle(it, Typeface.BOLD); editor.dirty = true } }
                ToolText("I", "Italic", italic = true) { field?.let { SdHtml.toggleStyle(it, Typeface.ITALIC); editor.dirty = true } }
                ToolText("U", "Underline", underline = true) { field?.let { SdHtml.toggleStyle(it, null); editor.dirty = true } }
                FmIconButton("title", "Heading", onPressed = { field?.let { SdHtml.toggleHeading(it); editor.dirty = true } })
                FmIconButton("format_list_bulleted", "List", onPressed = { field?.let { SdHtml.toggleList(it); editor.dirty = true } })
            }
            HorizontalDivider(color = c.border)
        }
        AndroidView(
            factory = { context ->
                EditText(context).apply {
                    background = null
                    gravity = Gravity.TOP or Gravity.START
                    setTextColor(textColor)
                    setHintTextColor(hintColor)
                    hint = "Start writing"
                    textSize = 16f
                    importantForAutofill = View.IMPORTANT_FOR_AUTOFILL_NO
                    imeOptions = EditorInfo.IME_FLAG_NO_PERSONALIZED_LEARNING
                    inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE or InputType.TYPE_TEXT_FLAG_CAP_SENTENCES
                    setPadding(48, 32, 48, 32)
                    setText(SdHtml.fromHtml(String(bytes, Charsets.UTF_8)), android.widget.TextView.BufferType.SPANNABLE)
                    if (readOnly) {
                        keyListener = null
                        setTextIsSelectable(true)
                    }
                    addTextChangedListener(object : TextWatcher {
                        override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
                        override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
                        override fun afterTextChanged(s: Editable?) {
                            if (!readOnly) editor.dirty = true
                            SecureDriveFeature.service.touch()
                        }
                    })
                    field = this
                }
            },
            modifier = Modifier.fillMaxSize(),
        )
    }
}

@Composable
private fun ToolText(
    label: String,
    description: String,
    weight: FontWeight = FontWeight.Normal,
    italic: Boolean = false,
    underline: Boolean = false,
    onClick: () -> Unit,
) {
    FmTextButton(onClick = onClick, minHeight = 40.dp, modifier = Modifier.semantics { contentDescription = description }) {
        Text(
            label,
            style = Fm.text.titleMedium.copy(
                fontWeight = weight,
                fontStyle = if (italic) FontStyle.Italic else FontStyle.Normal,
                textDecoration = if (underline) TextDecoration.Underline else TextDecoration.None,
                color = Fm.colors.textPrimary,
            ),
            modifier = Modifier.padding(horizontal = FmSpace.x1),
        )
    }
}

@Composable
internal fun SdSheetEditor(bytes: ByteArray, editor: SdEditorState, readOnly: Boolean) {
    val sheet = remember { sdParseSheet(bytes) }
    var revision by remember { mutableIntStateOf(0) }
    var row by remember { mutableIntStateOf(0) }
    var column by remember { mutableIntStateOf(0) }
    var formula by remember { mutableStateOf(sheet.raw(0, 0)) }
    editor.serialize = { sheet.toCsv() }
    val c = Fm.colors
    val rows = min(Sheet.MAX_ROWS, max(sheet.rows + 20, 50))
    val columns = min(Sheet.MAX_COLUMNS, max(sheet.columns + 4, 10))
    val scroll = rememberScrollState()
    val cellWidth = 104.dp
    val cellHeight = 36.dp
    Column(Modifier.fillMaxSize().imePadding()) {
        Row(
            Modifier.fillMaxWidth().background(c.bgRaised).padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        ) {
            Text("${Sheet.columnName(column)}${row + 1}", Modifier.width(56.dp), style = Fm.text.labelLarge)
            Box(Modifier.weight(1f)) {
                FmTextField(
                    value = formula,
                    onValueChange = {
                        formula = it
                        sheet.setRaw(row, column, it)
                        revision++
                        editor.dirty = true
                    },
                    readOnly = readOnly,
                    hint = if (readOnly) null else "Value or =formula, for example =SUM(B2:B9)",
                )
            }
        }
        HorizontalDivider(color = c.border)
        Row(Modifier.horizontalScroll(scroll)) {
            Box(Modifier.size(48.dp, cellHeight))
            for (col in 0 until columns) {
                Box(Modifier.size(cellWidth, cellHeight).background(c.bgOverlay), contentAlignment = Alignment.Center) {
                    Text(Sheet.columnName(col), style = Fm.text.labelMedium.copy(color = c.textSecondary))
                }
            }
        }
        LazyColumn(Modifier.weight(1f)) {
            items(rows) { r ->
                Row(Modifier.horizontalScroll(scroll)) {
                    Box(Modifier.size(48.dp, cellHeight).background(c.bgOverlay), contentAlignment = Alignment.Center) {
                        Text("${r + 1}", style = Fm.text.labelMedium.copy(color = c.textSecondary))
                    }
                    for (col in 0 until columns) {
                        revision
                        val selected = r == row && col == column
                        val shown = sheet.display(r, col)
                        val numeric = shown.isNotEmpty() && shown.toDoubleOrNull() != null
                        Box(
                            Modifier
                                .size(cellWidth, cellHeight)
                                .border(if (selected) 2.dp else 0.5.dp, if (selected) c.accent else c.border)
                                .clickable {
                                    row = r
                                    column = col
                                    formula = sheet.raw(r, col)
                                }
                                .padding(horizontal = FmSpace.x1),
                            contentAlignment = if (numeric) Alignment.CenterEnd else Alignment.CenterStart,
                        ) {
                            Text(
                                shown,
                                maxLines = 1,
                                overflow = TextOverflow.Ellipsis,
                                style = Fm.text.bodySmall.copy(color = if (shown.startsWith("#") && shown.endsWith("!") || shown == "#NAME?") c.danger else c.textPrimary),
                            )
                        }
                    }
                }
            }
        }
    }
}

private enum class DrawTool(val label: String) { Pen("Pen"), Line("Line"), Box("Box"), Oval("Oval"), Eraser("Eraser") }

private val PALETTE = listOf("#1f2328", "#cf222e", "#bf8700", "#1a7f37", "#0969da", "#8250df", "#ffffff")

private val WIDTHS = listOf(2f to "Thin", 4f to "Medium", 8f to "Thick", 16f to "Heavy")

private fun Drawing.deepCopy(): Drawing = Drawing(width, height, background, strokes.map { Stroke(it.color, it.width, it.points.toMutableList()) }.toMutableList())

private fun colorOf(hex: String): Color = try {
    Color(android.graphics.Color.parseColor(hex))
} catch (_: IllegalArgumentException) {
    Color.Black
}

@Composable
internal fun SdDrawingEditor(bytes: ByteArray, editor: SdEditorState, readOnly: Boolean) {
    val parsed = remember { sdParseDrawing(bytes) }
    if (parsed == null) {
        editor.serialize = { bytes.copyOf() }
        Text(
            "This picture was not made with Secure Drive's drawing editor, so it cannot be edited here. Use Open in another app from the file's menu.",
            Modifier.padding(FmSpace.x4),
            style = Fm.text.bodyMedium,
        )
        return
    }
    val drawing = remember { parsed }
    val history = remember { ArrayDeque<Drawing>() }
    var revision by remember { mutableIntStateOf(0) }
    var tool by remember { mutableStateOf(DrawTool.Pen) }
    var color by remember { mutableStateOf(PALETTE[0]) }
    var width by remember { mutableFloatStateOf(4f) }
    var current by remember { mutableStateOf<Stroke?>(null) }
    editor.serialize = { drawing.toSvg() }
    val c = Fm.colors
    fun snapshot() {
        history.addLast(drawing.deepCopy())
        if (history.size > 100) history.removeFirst()
    }
    fun changed() {
        revision++
        editor.dirty = true
    }
    Column(Modifier.fillMaxSize()) {
        if (!readOnly) {
            Row(
                Modifier.fillMaxWidth().background(c.bgRaised).horizontalScroll(rememberScrollState()).padding(horizontal = FmSpace.x2, vertical = FmSpace.x1),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(FmSpace.x1),
            ) {
                for (entry in DrawTool.entries) {
                    Chip(entry.label, entry == tool) { tool = entry }
                }
                for (swatch in PALETTE) {
                    Box(
                        Modifier
                            .size(28.dp)
                            .border(if (swatch == color) 2.dp else 1.dp, if (swatch == color) c.accent else c.border, RoundedCornerShape(6.dp))
                            .padding(3.dp)
                            .background(colorOf(swatch), RoundedCornerShape(4.dp))
                            .clickable { color = swatch },
                    )
                }
                for ((value, label) in WIDTHS) {
                    Chip(label, value == width) { width = value }
                }
                FmIconButton("undo", "Undo", onPressed = {
                    val previous = history.removeLastOrNull()
                    if (previous != null) {
                        drawing.strokes.clear()
                        drawing.strokes.addAll(previous.strokes)
                        changed()
                    }
                })
                FmTextButton(onClick = {
                    if (drawing.strokes.isNotEmpty()) {
                        snapshot()
                        drawing.strokes.clear()
                        changed()
                    }
                }) { Text("Clear") }
            }
            HorizontalDivider(color = c.border)
        }
        Canvas(
            Modifier
                .fillMaxSize()
                .background(c.bgBase)
                .pointerInput(readOnly, tool, color, width) {
                    if (readOnly) return@pointerInput
                    fun page(): Rect = pageRect(Size(size.width.toFloat(), size.height.toFloat()), drawing)
                    fun toDrawing(offset: Offset): Point {
                        val rect = page()
                        val scale = rect.width / drawing.width
                        return Point(
                            ((offset.x - rect.left) / scale).toDouble().coerceIn(0.0, drawing.width.toDouble()),
                            ((offset.y - rect.top) / scale).toDouble().coerceIn(0.0, drawing.height.toDouble()),
                        )
                    }
                    fun erase(point: Point): Boolean {
                        val radius = max(8.0, width * 2.0)
                        return drawing.strokes.removeAll { stroke ->
                            stroke.points.any { hypot(it.x - point.x, it.y - point.y) <= radius + stroke.width / 2 }
                        }
                    }
                    awaitEachGesture {
                        val down = awaitFirstDown()
                        SecureDriveFeature.service.touch()
                        val start = toDrawing(down.position)
                        snapshot()
                        var erased = false
                        if (tool == DrawTool.Eraser) {
                            if (erase(start)) {
                                erased = true
                                changed()
                            }
                        } else {
                            current = Stroke(color, width.toDouble(), mutableListOf(start))
                            revision++
                        }
                        while (true) {
                            val event = awaitPointerEvent()
                            val change = event.changes.firstOrNull { it.id == down.id } ?: break
                            if (!change.pressed) break
                            change.consume()
                            val point = toDrawing(change.position)
                            if (tool == DrawTool.Eraser) {
                                if (erase(point)) {
                                    erased = true
                                    changed()
                                }
                                continue
                            }
                            val stroke = current ?: continue
                            if (tool == DrawTool.Pen) {
                                val last = stroke.points.last()
                                if (hypot(last.x - point.x, last.y - point.y) >= 1.0) stroke.points.add(point)
                            } else {
                                stroke.points.clear()
                                stroke.points.addAll(shape(tool, start, point))
                            }
                            revision++
                        }
                        if (tool == DrawTool.Eraser) {
                            if (!erased) history.removeLastOrNull()
                        } else {
                            current?.let { drawing.strokes.add(it) }
                            current = null
                            changed()
                        }
                    }
                },
        ) {
            revision
            val rect = pageRect(size, drawing)
            val scale = rect.width / drawing.width
            drawRect(colorOf(drawing.background), rect.topLeft, rect.size)
            drawRect(c.border, rect.topLeft, rect.size, style = DrawStroke(1f))
            clipRect(rect.left, rect.top, rect.right, rect.bottom) {
                val all = drawing.strokes + listOfNotNull(current)
                for (stroke in all) {
                    if (stroke.points.isEmpty()) continue
                    val strokeColor = colorOf(stroke.color)
                    val strokeWidth = max(1f, (stroke.width * scale).toFloat())
                    fun at(p: Point) = Offset(rect.left + (p.x * scale).toFloat(), rect.top + (p.y * scale).toFloat())
                    if (stroke.points.size == 1) {
                        drawCircle(strokeColor, strokeWidth / 2, at(stroke.points[0]))
                        continue
                    }
                    val path = Path()
                    path.moveTo(at(stroke.points[0]).x, at(stroke.points[0]).y)
                    for (i in 1 until stroke.points.size) path.lineTo(at(stroke.points[i]).x, at(stroke.points[i]).y)
                    drawPath(path, strokeColor, style = DrawStroke(strokeWidth, cap = StrokeCap.Round, join = StrokeJoin.Round))
                }
            }
        }
    }
}

private fun pageRect(area: Size, drawing: Drawing): Rect {
    val inset = 24f
    val width = max(1f, area.width - 2 * inset)
    val height = max(1f, area.height - 2 * inset)
    val scale = min(width / drawing.width, height / drawing.height)
    val w = drawing.width * scale
    val h = drawing.height * scale
    val left = (area.width - w) / 2
    val top = (area.height - h) / 2
    return Rect(left, top, left + w, top + h)
}

private fun shape(tool: DrawTool, a: Point, b: Point): List<Point> = when (tool) {
    DrawTool.Line -> listOf(a, b)
    DrawTool.Box -> listOf(a, Point(b.x, a.y), b, Point(a.x, b.y), a)
    else -> {
        val cx = (a.x + b.x) / 2
        val cy = (a.y + b.y) / 2
        val rx = kotlin.math.abs(b.x - a.x) / 2
        val ry = kotlin.math.abs(b.y - a.y) / 2
        (0..72).map { i ->
            val angle = i * Math.PI * 2 / 72
            Point(cx + rx * cos(angle), cy + ry * sin(angle))
        }
    }
}

@Composable
private fun Chip(label: String, selected: Boolean, onClick: () -> Unit) {
    val c = Fm.colors
    Box(
        Modifier
            .height(32.dp)
            .border(1.dp, if (selected) c.accent else c.border, RoundedCornerShape(16.dp))
            .background(if (selected) c.accentSubtle else Color.Transparent, RoundedCornerShape(16.dp))
            .clickable(onClick = onClick)
            .padding(horizontal = FmSpace.x3),
        contentAlignment = Alignment.Center,
    ) {
        Text(label, style = Fm.text.labelMedium.copy(color = if (selected) c.accent else c.textPrimary))
    }
}
