package com.forkmesh.nativeapp.feature.code

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.OffsetMapping
import androidx.compose.ui.text.input.TextFieldValue
import androidx.compose.ui.text.input.TransformedText
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.code.CodeCaret
import com.forkmesh.nativeapp.code.CodeEdit
import com.forkmesh.nativeapp.code.CodeHighlighter
import com.forkmesh.nativeapp.code.CodeIndent
import com.forkmesh.nativeapp.code.CodeKeys
import com.forkmesh.nativeapp.code.CodeLang
import com.forkmesh.nativeapp.code.codeCaretOf
import com.forkmesh.nativeapp.code.detectIndentUnit
import com.forkmesh.nativeapp.code.editorStatusText
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmSafeInput
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmPalette
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType

@Stable
internal class CodeEditorState(val lang: CodeLang, initial: String = "") {
    var value by mutableStateOf(TextFieldValue(initial))
        private set
    var indentUnit by mutableStateOf(detectIndentUnit(initial))
        private set
    var focused by mutableStateOf(false)
    var canUndo by mutableStateOf(false)
        private set
    var canRedo by mutableStateOf(false)
        private set
    val focusRequester = FocusRequester()

    private val undoStack = ArrayDeque<TextFieldValue>()
    private val redoStack = ArrayDeque<TextFieldValue>()
    private var lastPush = 0L

    val text: String get() = value.text

    val caret: CodeCaret get() = codeCaretOf(value.text, value.selection.end)

    fun reset(text: String) {
        value = TextFieldValue(text)
        indentUnit = detectIndentUnit(text)
        undoStack.clear()
        redoStack.clear()
        syncHistory()
    }

    fun onEdit(next: TextFieldValue) {
        val old = value
        if (next.text == old.text) {
            value = next
            return
        }
        var result = next
        if (next.composition == null) {
            val formatted = CodeIndent.formatEdit(
                CodeEdit(old.text, old.selection.min, old.selection.max),
                CodeEdit(next.text, next.selection.start, next.selection.end),
                indentUnit,
                CodeIndent.colonOpensBlock(lang),
            )
            if (formatted.text != next.text) {
                result = TextFieldValue(formatted.text, TextRange(formatted.selectionStart, formatted.selectionEnd))
            }
        }
        record(old, coalesce = result.text.count { it == '\n' } == old.text.count { it == '\n' })
        value = result
    }

    fun insert(inserted: String) {
        val edit = CodeIndent.insert(CodeEdit(value.text, value.selection.min, value.selection.max), inserted)
        record(value, coalesce = false)
        value = TextFieldValue(edit.text, TextRange(edit.selectionStart, edit.selectionEnd))
    }

    fun moveToLine(line: Int) {
        value = value.copy(selection = TextRange(CodeIndent.lineStartOffset(value.text, line)))
    }

    fun undo() {
        val previous = undoStack.removeLastOrNull() ?: return
        redoStack.addLast(value)
        value = previous
        lastPush = 0
        syncHistory()
    }

    fun redo() {
        val next = redoStack.removeLastOrNull() ?: return
        undoStack.addLast(value)
        value = next
        lastPush = 0
        syncHistory()
    }

    private fun record(old: TextFieldValue, coalesce: Boolean) {
        val now = System.currentTimeMillis()
        if (!coalesce || now - lastPush > 800 || undoStack.isEmpty()) {
            undoStack.addLast(old)
            if (undoStack.size > 200) undoStack.removeFirst()
        }
        lastPush = now
        redoStack.clear()
        syncHistory()
    }

    private fun syncHistory() {
        canUndo = undoStack.isNotEmpty()
        canRedo = redoStack.isNotEmpty()
    }
}

private class CodeHighlightTransformation(lang: CodeLang, private val colors: FmPalette) : VisualTransformation {
    private val highlighter = CodeHighlighter(lang)
    private val plain = lang == CodeLang.Plain
    private var cache = HashMap<String, AnnotatedString>()

    override fun filter(text: AnnotatedString): TransformedText {
        if (plain) return TransformedText(text, OffsetMapping.Identity)
        val lines = text.text.split('\n')
        val inBlock = highlighter.blockCommentStarts(lines)
        val next = HashMap<String, AnnotatedString>(lines.size)
        val built = buildAnnotatedString {
            for (i in lines.indices) {
                if (i > 0) append('\n')
                val line = lines[i]
                if (line.isEmpty()) continue
                val key = (if (inBlock[i]) "1" else "0") + line
                val spans = next[key] ?: cache[key] ?: buildAnnotatedString {
                    for (token in highlighter.highlight(line, inBlock[i])) {
                        withStyle(spanStyle(token.kind, colors)) { append(token.text) }
                    }
                }
                next[key] = spans
                append(spans)
            }
        }
        cache = next
        return TransformedText(built, OffsetMapping.Identity)
    }
}

internal object CodeEditorMetrics {
    val fontSize = FmType.small
    const val lineHeightFactor = 1.55f
    val padTop = FmSpace.x2
    val padBottom = FmSpace.x6
    val gutterPad = FmSpace.x3
}

@Composable
internal fun FmCodeEditor(state: CodeEditorState, modifier: Modifier = Modifier) {
    val c = Fm.colors
    val density = LocalDensity.current
    val lineHeight = CodeEditorMetrics.fontSize * CodeEditorMetrics.lineHeightFactor
    val lineHeightPx = with(density) { lineHeight.toPx() }
    val codeStyle = TextStyle(
        fontFamily = FmType.mono,
        fontSize = CodeEditorMetrics.fontSize,
        lineHeight = lineHeight,
        letterSpacing = 0.sp,
        color = codeBaseColor(c),
    )
    val transformation = remember(state.lang, c) { CodeHighlightTransformation(state.lang, c) }
    val caret = state.caret
    val showActive = state.focused
    val activeIndex = caret.line - 1
    val digits = maxOf(2, caret.lineCount.toString().length)
    val charWidth = CodeEditorMetrics.fontSize.value * 0.6f
    val gutterWidth = (digits * charWidth).dp + CodeEditorMetrics.gutterPad * 2
    val gutterText = remember(caret.lineCount) { (1..caret.lineCount).joinToString("\n") }
    val vertical = rememberScrollState()
    val horizontal = rememberScrollState()
    val padTopPx = with(density) { CodeEditorMetrics.padTop.toPx() }
    BoxWithConstraints(
        modifier
            .fillMaxSize()
            .background(c.editorSurface),
    ) {
        val viewportHeight = maxHeight
        val viewportWidth = maxWidth
        Row(
            Modifier
                .fillMaxWidth()
                .heightIn(min = viewportHeight)
                .verticalScroll(vertical)
                .drawBehind {
                    drawRect(c.editorGutter, size = Size(gutterWidth.toPx(), size.height))
                    drawRect(c.border, topLeft = Offset(gutterWidth.toPx() - 1.dp.toPx(), 0f), size = Size(1.dp.toPx(), size.height))
                    if (showActive) {
                        drawRect(
                            c.editorActiveLine,
                            topLeft = Offset(0f, padTopPx + activeIndex * lineHeightPx),
                            size = Size(size.width, lineHeightPx),
                        )
                    }
                },
        ) {
            Box(
                Modifier
                    .width(gutterWidth)
                    .pointerInput(caret.lineCount) {
                        detectTapGestures { position ->
                            val line = ((position.y - padTopPx) / lineHeightPx).toInt().coerceIn(0, caret.lineCount - 1)
                            state.moveToLine(line)
                            state.focusRequester.requestFocus()
                        }
                    }
                    .padding(top = CodeEditorMetrics.padTop, end = CodeEditorMetrics.gutterPad, bottom = CodeEditorMetrics.padBottom)
                    .clearAndSetSemantics {},
            ) {
                val numbers = remember(gutterText, activeIndex, showActive, c) {
                    buildAnnotatedString {
                        val lines = gutterText.split('\n')
                        lines.forEachIndexed { index, number ->
                            if (index > 0) append('\n')
                            if (showActive && index == activeIndex) {
                                withStyle(androidx.compose.ui.text.SpanStyle(color = c.textSecondary)) { append(number) }
                            } else {
                                append(number)
                            }
                        }
                    }
                }
                Text(
                    numbers,
                    modifier = Modifier.fillMaxWidth(),
                    textAlign = TextAlign.End,
                    softWrap = false,
                    style = codeStyle.copy(color = c.textDisabled),
                )
            }
            Box(
                Modifier
                    .weight(1f)
                    .horizontalScroll(horizontal),
            ) {
                BasicTextField(
                    value = state.value,
                    onValueChange = FmSafeInput.typing(state::onEdit),
                    modifier = Modifier
                        .widthIn(min = viewportWidth - gutterWidth)
                        .heightIn(min = viewportHeight)
                        .focusRequester(state.focusRequester)
                        .onFocusChanged { state.focused = it.isFocused }
                        .padding(
                            start = CodeEditorMetrics.gutterPad,
                            top = CodeEditorMetrics.padTop,
                            end = FmSpace.x6,
                            bottom = CodeEditorMetrics.padBottom,
                        ),
                    textStyle = codeStyle,
                    cursorBrush = SolidColor(c.accent),
                    visualTransformation = transformation,
                    keyboardOptions = KeyboardOptions(
                        capitalization = KeyboardCapitalization.None,
                        autoCorrectEnabled = false,
                        keyboardType = KeyboardType.Ascii,
                    ),
                )
            }
        }
    }
}

@Composable
internal fun CodeKeyBar(state: CodeEditorState) {
    val c = Fm.colors
    val focusManager = LocalFocusManager.current
    Row(
        Modifier
            .fillMaxWidth()
            .height(44.dp)
            .background(c.bgRaised)
            .drawBehind { drawRect(c.border, size = Size(size.width, 1.dp.toPx())) }
            .focusProperties { canFocus = false },
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Spacer(Modifier.width(FmSpace.x1))
        CodeKey(label = "Tab", icon = "keyboard_tab", description = "Indent") { state.insert(state.indentUnit) }
        LazyRow(
            Modifier.weight(1f),
            contentPadding = androidx.compose.foundation.layout.PaddingValues(horizontal = FmSpace.x1),
        ) {
            items(CodeKeys.symbols) { symbol -> CodeKey(label = symbol) { state.insert(symbol) } }
        }
        FmIconButton("undo", "Undo", onPressed = if (state.canUndo) state::undo else null, iconSize = 20.dp)
        FmIconButton("redo", "Redo", onPressed = if (state.canRedo) state::redo else null, iconSize = 20.dp)
        FmIconButton("keyboard_hide_outlined", "Hide keyboard", onPressed = { focusManager.clearFocus() }, iconSize = 20.dp)
    }
}

@Composable
private fun CodeKey(label: String, icon: String? = null, description: String = label, onTap: () -> Unit) {
    val c = Fm.colors
    Box(
        Modifier
            .padding(horizontal = 2.dp, vertical = 6.dp)
            .height(32.dp)
            .widthIn(min = 34.dp)
            .background(c.bgOverlay, RoundedCornerShape(FmRadius.sm))
            .clickable(role = Role.Button, onClickLabel = description, onClick = onTap)
            .focusProperties { canFocus = false }
            .semantics { contentDescription = description }
            .padding(horizontal = FmSpace.x2),
        contentAlignment = Alignment.Center,
    ) {
        if (icon == null) {
            Text(label, style = TextStyle(fontFamily = FmType.mono, fontSize = FmType.body, color = c.textPrimary))
        } else {
            Row(verticalAlignment = Alignment.CenterVertically) {
                FmIcon(icon, size = 16.dp, tint = c.textPrimary)
                Spacer(Modifier.width(FmSpace.x1))
                Text(label, style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.textPrimary))
            }
        }
    }
}

@Composable
internal fun EditorStatusLine(caret: CodeCaret, focused: Boolean, dirty: Boolean, trailing: String) {
    val c = Fm.colors
    val style = Fm.text.bodySmall.copy(
        fontWeight = FmType.emphasis,
        color = c.textTertiary,
        fontFeatureSettings = FmType.tabular,
    )
    Row(
        Modifier
            .fillMaxWidth()
            .height(32.dp)
            .drawBehind { drawRect(c.border, size = Size(size.width, 1.dp.toPx())) }
            .padding(horizontal = FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (dirty) {
            Box(
                Modifier
                    .size(7.dp)
                    .background(c.warning, CircleShape),
            )
            Spacer(Modifier.width(FmSpace.x2))
        }
        Text(
            editorStatusText(caret, focused),
            modifier = Modifier.weight(1f),
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            style = style,
        )
        Text(trailing, style = style)
    }
}
