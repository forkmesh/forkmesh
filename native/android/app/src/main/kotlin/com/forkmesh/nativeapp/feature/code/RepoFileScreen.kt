package com.forkmesh.nativeapp.feature.code

import android.graphics.BitmapFactory
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.DisableSelection
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawBehind
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.code.CodeHighlighter
import com.forkmesh.nativeapp.code.CodeTokenKind
import com.forkmesh.nativeapp.code.PreviewKind
import com.forkmesh.nativeapp.code.RepoExplorer
import com.forkmesh.nativeapp.code.codeLangForPath
import com.forkmesh.nativeapp.model.RepoBlob
import com.forkmesh.nativeapp.model.Repository
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.ui.kit.FmBottomBar
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.rememberFmStatus
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmPalette
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType

internal class RepoFileScreen(val repo: Repository, val path: String, val ref: String = "", val servedBlobSha: String = "") : Screen() {
    private val blob = AsyncValue<RepoBlob>().apply { reload { repoApi.blob(repo.owner, repo.name, path, ref = ref) } }
    private val image = AsyncValue<ImageBitmap>()
    private var document: CodeDocument? = null

    private var activeLine by mutableIntStateOf(0)

    override val title: String get() = RepoExplorer.fileName(path)

    @Composable
    override fun Content(navigator: Navigator) {
        blob.Bind()
        val c = Fm.colors
        val rawUrl = remember { repoApi.rawUri(repo.owner, repo.name, path, ref = ref) }
        val copyStatus = rememberFmStatus()
        val copy = rememberCopyText(copyStatus)
        DetailScaffold(
            title = title,
            navigator = navigator,
            status = copyStatus,
            actions = {
                if (RepoExplorer.previewKind(path) == PreviewKind.Text) {
                    FmIconButton(
                        "edit_outlined",
                        "Edit this file",
                        onPressed = { navigator.push(QuickEditScreen(repo, path, ref, servedBlobSha)) },
                        color = c.textPrimary,
                        iconSize = 24.dp,
                    )
                }
                if (RepoExplorer.previewKind(path) == PreviewKind.Text) {
                    FmIconButton(
                        "manage_search",
                        "Find and outline",
                        onPressed = { navigator.push(com.forkmesh.nativeapp.feature.code.review.FileToolsScreen(repo, path, ref)) },
                        color = c.textPrimary,
                        iconSize = 24.dp,
                    )
                }
                FmIconButton("copy_all_outlined", "Copy path", onPressed = { copy("Path", path) }, color = c.textPrimary, iconSize = 24.dp)
                FmIconButton("link", "Copy raw URL", onPressed = { copy("Raw URL", rawUrl) }, color = c.textPrimary, iconSize = 24.dp)
            },
        ) {
            val error = blob.error
            when {
                blob.waiting -> FmSkeletonList(rows = 10, hasLeading = false, label = "Loading file")
                error != null -> Box(Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                    Text("$error", Modifier.padding(24.dp), textAlign = TextAlign.Center)
                }
                else -> FilePreview(blob.data ?: RepoBlob(path = path, content = ""))
            }
        }
    }

    @Composable
    private fun FilePreview(blob: RepoBlob) {
        val path = blob.path
        val rawUrl = remember(path) { repoApi.rawUri(repo.owner, repo.name, path, ref = ref) }
        when (RepoExplorer.previewKind(path)) {
            PreviewKind.Image -> Column(
                Modifier
                    .fillMaxSize()
                    .background(Fm.colors.bgBase),
            ) {
                FilePreviewHeader(path, blob.source)
                Box(
                    Modifier
                        .weight(1f)
                        .fillMaxWidth(),
                    contentAlignment = Alignment.Center,
                ) {
                    ImagePreview(path, rawUrl, blob.source)
                }
            }
            PreviewKind.Video -> UnsupportedPreview(
                path = path,
                message = "Video preview is not bundled in the mobile app yet. Use the raw file URL below to open or download it.",
                rawUrl = rawUrl,
                source = blob.source,
            )
            PreviewKind.Text -> {
                val decoded = remember(blob) { RepoExplorer.decodedText(blob.content, blob.encoding) }
                if (decoded == null) {
                    UnsupportedPreview(
                        path = path,
                        message = "This looks like a binary file, so ForkMesh is not showing it as text.",
                        rawUrl = rawUrl,
                        source = blob.source,
                    )
                } else {
                    val content = decoded.ifEmpty { "Empty file." }
                    val doc = remember(path, content) {
                        document?.takeIf { it.path == path && it.content == content }
                            ?: CodeDocument(path, content).also { document = it }
                    }
                    CodePreview(doc, blob.source, activeLine) { activeLine = it }
                }
            }
        }
    }

    @Composable
    private fun ImagePreview(path: String, rawUrl: String, source: String) {
        LaunchedEffect(rawUrl) {
            if (!image.started) image.reload { fetchImage(rawUrl) }
        }
        image.Bind()
        val bitmap = image.data
        val error = image.error
        when {
            error != null -> UnsupportedPreview(
                path = path,
                message = "Could not render this image preview. $error",
                rawUrl = rawUrl,
                source = source,
            )
            bitmap != null -> {
                var scale by remember { mutableFloatStateOf(1f) }
                var offset by remember { mutableStateOf(Offset.Zero) }
                Image(
                    bitmap = bitmap,
                    contentDescription = RepoExplorer.fileName(path),
                    modifier = Modifier
                        .fillMaxSize()
                        .pointerInput(Unit) {
                            detectTransformGestures { _, pan, zoom, _ ->
                                scale = (scale * zoom).coerceIn(.5f, 5f)
                                offset += pan
                            }
                        }
                        .graphicsLayer {
                            scaleX = scale
                            scaleY = scale
                            translationX = offset.x
                            translationY = offset.y
                        },
                    contentScale = ContentScale.Inside,
                )
            }
        }
    }
}

private class ImageLoadException(message: String) : Exception(message) {
    override fun toString(): String = message ?: ""
}

private fun fetchImage(url: String): ImageBitmap {
    val response = HttpClient.Request.get(url).send()
    if (!response.ok) throw ImageLoadException("HTTP request failed, statusCode: ${response.status}, $url")
    val bytes = response.body
    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
    var sample = 1
    while (bounds.outWidth / sample > 4096 || bounds.outHeight / sample > 4096) sample *= 2
    val bitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.size, BitmapFactory.Options().apply { inSampleSize = sample })
        ?: throw ImageLoadException("Exception: Invalid image data")
    return bitmap.asImageBitmap()
}

@Composable
private fun UnsupportedPreview(path: String, message: String, rawUrl: String = "", source: String = "") {
    val c = Fm.colors
    Box(
        Modifier
            .fillMaxSize()
            .background(c.bgBase),
    ) {
        Column(
            Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .windowInsetsPadding(fmSafeAreaInsets(top = false))
                .padding(FmSpace.x5),
        ) {
            InfoCard(RepoExplorer.fileName(path)) {
                if (RepoExplorer.sourceLabel(source) != null) {
                    SourceChip(source)
                    Spacer(Modifier.height(FmSpace.x3))
                }
                Text(message, color = c.textSecondary)
                Spacer(Modifier.height(FmSpace.x3))
                PreviewValue("Path", path)
                if (rawUrl.isNotEmpty()) {
                    Spacer(Modifier.height(FmSpace.x3))
                    PreviewValue("Raw URL", rawUrl, accent = true)
                }
            }
        }
    }
}

@Composable
private fun PreviewValue(label: String, value: String, accent: Boolean = false) {
    val c = Fm.colors
    val shape = RoundedCornerShape(FmRadius.sm)
    Column {
        Text(label, style = Fm.text.bodySmall.copy(fontSize = FmType.micro, fontWeight = FmType.emphasis, color = c.textTertiary))
        Spacer(Modifier.height(FmSpace.x1))
        Box(
            Modifier
                .fillMaxWidth()
                .background(c.bgBase, shape)
                .border(1.dp, c.border, shape)
                .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
        ) {
            SelectionContainer {
                Text(
                    value,
                    style = Fm.text.bodySmall.copy(
                        color = if (accent) c.accent else c.textSecondary,
                        fontFamily = if (accent) null else FmType.mono,
                    ),
                )
            }
        }
    }
}

@Composable
private fun FilePreviewHeader(path: String, source: String) {
    val c = Fm.colors
    Row(
        Modifier
            .fillMaxWidth()
            .heightIn(min = 42.dp)
            .background(c.editorSurface)
            .drawBehind {
                val rule = 1.dp.toPx()
                drawLine(c.border, Offset(0f, size.height - rule / 2), Offset(size.width, size.height - rule / 2), rule)
            }
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Row(
            Modifier
                .weight(1f, fill = false)
                .background(c.bgOverlay, RoundedCornerShape(FmRadius.md))
                .padding(horizontal = FmSpace.x3, vertical = 6.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            FmIcon(RepoExplorer.fileGlyph(RepoExplorer.fileName(path)), size = 15.dp, tint = c.accent)
            Spacer(Modifier.width(FmSpace.x2))
            Text(
                path,
                modifier = Modifier.weight(1f, fill = false),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = c.textPrimary),
            )
        }
        if (RepoExplorer.sourceLabel(source) != null) {
            Spacer(Modifier.width(FmSpace.x2))
            Box(Modifier.weight(1f, fill = false)) { SourceChip(source) }
        }
    }
}

@Stable
private class CodeDocument(val path: String, val content: String) {
    val lines: List<String> = RepoExplorer.codeLines(content)
    val highlighter = CodeHighlighter(codeLangForPath(path))
    private val startsInBlockComment = highlighter.blockCommentStarts(lines)
    val longestLine: Int = lines.maxOfOrNull { it.length } ?: 0
    val byteCount: Long = content.toByteArray(Charsets.UTF_8).size.toLong()
    private val spans = arrayOfNulls<AnnotatedString>(lines.size)
    private var spansDark: Boolean? = null

    fun spansFor(index: Int, colors: FmPalette): AnnotatedString {
        if (colors.dark != spansDark) {
            spans.fill(null)
            spansDark = colors.dark
        }
        spans[index]?.let { return it }
        val tokens = highlighter.highlight(lines[index], startsInBlockComment[index])
        val built = buildAnnotatedString {
            for (token in tokens) withStyle(spanStyle(token.kind, colors)) { append(token.text) }
        }
        spans[index] = built
        return built
    }
}

internal fun codeBaseColor(colors: FmPalette): Color = if (colors.dark) Color(0xFFD7D9E0) else colors.textPrimary

internal fun spanStyle(kind: CodeTokenKind, colors: FmPalette): SpanStyle = when (kind) {
    CodeTokenKind.Base -> SpanStyle(color = codeBaseColor(colors))
    CodeTokenKind.Keyword -> SpanStyle(color = colors.codeKeyword)
    CodeTokenKind.Heading -> SpanStyle(color = colors.codeKeyword, fontWeight = FmType.emphasis)
    CodeTokenKind.Type -> SpanStyle(color = colors.codeType)
    CodeTokenKind.String -> SpanStyle(color = colors.codeString)
    CodeTokenKind.Comment -> SpanStyle(color = colors.codeComment, fontStyle = FontStyle.Italic)
    CodeTokenKind.Number -> SpanStyle(color = colors.codeNumber)
    CodeTokenKind.Function -> SpanStyle(color = colors.codeFunction)
}

@Composable
private fun CodePreview(doc: CodeDocument, source: String, activeLine: Int, onActiveLine: (Int) -> Unit) {
    val c = Fm.colors
    val editorBg = c.editorSurface
    val gutterBg = c.editorGutter
    val activeBg = c.editorActiveLine
    val gutter = RepoExplorer.gutterWidth(doc.lines.size)
    val estimated = RepoExplorer.estimatedWidth(doc.longestLine, gutter).dp
    Column(
        Modifier
            .fillMaxSize()
            .background(editorBg),
    ) {
        FilePreviewHeader(doc.path, source)
        BoxWithConstraints(
            Modifier
                .weight(1f)
                .fillMaxWidth(),
        ) {
            val width = if (estimated < maxWidth) maxWidth else estimated
            val horizontal = rememberScrollState()
            SelectionContainer {
                LazyColumn(
                    modifier = Modifier
                        .fillMaxHeight()
                        .horizontalScroll(horizontal)
                        .width(width),
                    contentPadding = PaddingValues(bottom = FmSpace.x5),
                ) {
                    items(doc.lines.size) { index ->
                        CodeLine(
                            lineNumber = index + 1,
                            spans = doc.spansFor(index, c),
                            gutterWidth = gutter.dp,
                            gutterBg = gutterBg,
                            editorBg = editorBg,
                            activeBg = activeBg,
                            active = index == activeLine,
                            onTap = { onActiveLine(index) },
                        )
                    }
                }
            }
        }
        EditorStatusBar(activeLine + 1, doc.lines.size, doc.byteCount, doc.highlighter.lang.label)
    }
}

@Composable
private fun EditorStatusBar(activeLine: Int, lineCount: Int, byteCount: Long, language: String) {
    val style = Fm.text.bodySmall.copy(fontWeight = FmType.emphasis, color = Fm.colors.textTertiary)
    FmBottomBar(
        height = 40.dp,
        leading = {
            Text(
                RepoExplorer.statusBarText(activeLine, lineCount, byteCount),
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
                style = style,
            )
        },
        actions = { Text(language, style = style) },
    )
}

@Composable
private fun CodeLine(
    lineNumber: Int,
    spans: AnnotatedString,
    gutterWidth: Dp,
    gutterBg: Color,
    editorBg: Color,
    activeBg: Color,
    active: Boolean,
    onTap: () -> Unit,
) {
    val c = Fm.colors
    Row(
        Modifier
            .fillMaxWidth()
            .height(IntrinsicSize.Min)
            .background(if (active) activeBg else editorBg)
            .pointerInput(lineNumber) { detectTapGestures(onTap = { onTap() }) },
    ) {
        DisableSelection {
            Box(
                Modifier
                    .width(gutterWidth)
                    .fillMaxHeight()
                    .background(if (active) activeBg else gutterBg)
                    .padding(end = FmSpace.x3, top = 2.dp),
            ) {
                Text(
                    "$lineNumber",
                    modifier = Modifier.fillMaxWidth(),
                    textAlign = TextAlign.End,
                    style = TextStyle(
                        color = if (active) c.textSecondary else c.textDisabled,
                        fontFamily = FmType.mono,
                        fontSize = FmType.meta,
                        lineHeight = FmType.meta * 1.62f,
                        fontWeight = if (active) FmType.emphasis else FmType.regular,
                    ),
                )
            }
        }
        Text(
            spans,
            modifier = Modifier
                .weight(1f)
                .padding(start = FmSpace.x3, top = 2.dp, end = FmSpace.x4, bottom = 2.dp),
            maxLines = 1,
            softWrap = false,
            style = Fm.text.bodyMedium.copy(
                color = codeBaseColor(c),
                fontFamily = FmType.mono,
                lineHeight = FmType.small * 1.5f,
                letterSpacing = 0.sp,
            ),
        )
    }
}
