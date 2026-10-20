package com.forkmesh.nativeapp.feature.docs

import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.IntrinsicSize
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.foundation.lazy.rememberLazyListState
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.AnnotatedString
import androidx.compose.ui.text.LinkAnnotation
import androidx.compose.ui.text.SpanStyle
import androidx.compose.ui.text.TextLinkStyles
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.buildAnnotatedString
import androidx.compose.ui.text.font.FontStyle
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextDecoration
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.text.withLink
import androidx.compose.ui.text.withStyle
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.docs.DocsAlign
import com.forkmesh.nativeapp.docs.DocsBlock
import com.forkmesh.nativeapp.docs.DocsDocument
import com.forkmesh.nativeapp.docs.DocsLibrary
import com.forkmesh.nativeapp.docs.DocsMarkdown
import com.forkmesh.nativeapp.docs.DocsPage
import com.forkmesh.nativeapp.docs.DocsSpan
import com.forkmesh.nativeapp.docs.DocsTarget
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmSkeletonList
import com.forkmesh.nativeapp.ui.kit.rememberLinkOpener
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.navigation.Screen
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

class DocsPageScreen(private val path: String, private val fragment: String = "") : Screen() {
    override val title: String get() = DocsFeature.current?.page(path)?.title ?: "Docs"

    private var document by mutableStateOf<DocsDocument?>(null)
    private var landed = false

    @Composable
    override fun Content(navigator: Navigator) {
        val open = rememberLinkOpener()
        DocsFrame(
            navigator,
            title = { library -> library?.page(path)?.title ?: "Docs" },
            actions = { library ->
                val url = library.page(path)?.url.orEmpty()
                if (url.isNotEmpty()) {
                    FmIconButton("open_in_new", "Open on web", onPressed = { open(url) }, color = Fm.colors.textPrimary, iconSize = 22.dp)
                }
            },
        ) { library ->
            val page = library.page(path)
            if (page == null) {
                FmEmptyState("description", "Page not found", message = "The documentation library has no page at $path.")
            } else {
                LaunchedEffect(page.path) {
                    if (document == null) document = withContext(Dispatchers.Default) { DocsMarkdown.parse(page.markdown) }
                }
                val parsed = document
                if (parsed == null) {
                    FmSkeletonList(Modifier.fillMaxWidth().padding(FmSpace.x4), rows = 8, hasLeading = false, label = "Loading ${page.title}")
                } else {
                    Reader(library, page, parsed, navigator, open)
                }
            }
        }
    }

    @Composable
    private fun Reader(library: DocsLibrary, page: DocsPage, parsed: DocsDocument, navigator: Navigator, open: (String) -> Unit) {
        val listState = rememberLazyListState()
        val scope = rememberCoroutineScope()
        fun scrollTo(target: String) {
            val index = parsed.anchorIndex(target)
            if (index >= 0) scope.launch { listState.animateScrollToItem(index) }
        }
        val onLink: (String) -> Unit = { href ->
            when (val target = library.resolve(page.path, href)) {
                is DocsTarget.Anchor -> scrollTo(target.fragment)
                is DocsTarget.Page ->
                    if (target.path == page.path) scrollTo(target.fragment)
                    else navigator.push(DocsPageScreen(target.path, target.fragment))
                is DocsTarget.External -> open(target.url)
                null -> Unit
            }
        }
        LaunchedEffect(parsed) {
            if (!landed) {
                landed = true
                val index = parsed.anchorIndex(fragment)
                if (index >= 0) listState.scrollToItem(index)
            }
        }
        LazyColumn(
            Modifier.fillMaxSize(),
            state = listState,
            contentPadding = PaddingValues(start = FmSpace.x4, end = FmSpace.x4, top = FmSpace.x2, bottom = FmSpace.x6),
        ) {
            itemsIndexed(parsed.blocks) { _, block -> DocsBlockView(block, parsed.references, onLink) }
            item(key = "footer") { Footer(library, page, navigator) }
        }
    }

    @Composable
    private fun Footer(library: DocsLibrary, page: DocsPage, navigator: Navigator) {
        val previous = library.previous(page.path)
        val next = library.next(page.path)
        if (previous == null && next == null) return
        Column(Modifier.fillMaxWidth().padding(top = FmSpace.x5)) {
            HorizontalDivider(thickness = 1.dp, color = Fm.colors.border)
            Row(Modifier.fillMaxWidth().padding(top = FmSpace.x3), horizontalArrangement = Arrangement.spacedBy(FmSpace.x3)) {
                Box(Modifier.weight(1f)) {
                    if (previous != null) {
                        PageStep("Previous", previous.title, "chevron_left", end = false) { navigator.replace(DocsPageScreen(previous.path)) }
                    }
                }
                Box(Modifier.weight(1f)) {
                    if (next != null) {
                        PageStep("Next", next.title, "chevron_right", end = true) { navigator.replace(DocsPageScreen(next.path)) }
                    }
                }
            }
        }
    }
}

@Composable
private fun PageStep(label: String, title: String, icon: String, end: Boolean, onClick: () -> Unit) {
    Row(
        Modifier
            .fillMaxWidth()
            .clip(RoundedCornerShape(FmRadius.md))
            .border(1.dp, Fm.colors.border, RoundedCornerShape(FmRadius.md))
            .clickable(onClick = onClick)
            .padding(FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (!end) FmIcon(icon, size = 20.dp, tint = Fm.colors.textTertiary)
        Column(Modifier.weight(1f).padding(horizontal = FmSpace.x1), horizontalAlignment = if (end) Alignment.End else Alignment.Start) {
            Text(label, style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary))
            Text(
                title,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis,
                textAlign = if (end) TextAlign.End else TextAlign.Start,
                style = Fm.text.titleSmall.copy(color = Fm.colors.accent),
            )
        }
        if (end) FmIcon(icon, size = 20.dp, tint = Fm.colors.textTertiary)
    }
}

@Composable
internal fun DocsBlockView(block: DocsBlock, references: Map<String, String>, onLink: (String) -> Unit) {
    val body = Fm.text.bodyLarge.copy(color = Fm.colors.textPrimary)
    when (block) {
        is DocsBlock.Heading -> {
            val style = when (block.level) {
                1 -> Fm.text.headlineSmall.copy(fontWeight = FmType.emphasis)
                2 -> Fm.text.titleLarge
                3 -> Fm.text.titleMedium.copy(fontWeight = FmType.emphasis)
                else -> Fm.text.titleSmall
            }
            DocsText(
                block.text,
                references,
                onLink,
                style.copy(color = Fm.colors.textPrimary),
                Modifier
                    .padding(top = if (block.level <= 2) FmSpace.x5 else FmSpace.x4, bottom = FmSpace.x2)
                    .semantics { heading() },
            )
        }
        is DocsBlock.Paragraph -> DocsText(block.text, references, onLink, body, Modifier.padding(vertical = FmSpace.x2))
        is DocsBlock.ListItem -> Row(Modifier.fillMaxWidth().padding(start = 20.dp * block.depth, top = 3.dp, bottom = 3.dp)) {
            Box(Modifier.width(if (block.ordered) 30.dp else 22.dp).padding(top = 1.dp)) {
                val checked = block.checked
                when {
                    checked != null -> FmIcon(
                        if (checked) "check_box" else "check_box_outline_blank",
                        size = 18.dp,
                        tint = if (checked) Fm.colors.accent else Fm.colors.textTertiary,
                    )
                    block.continuation -> Unit
                    else -> Text(block.marker, style = body.copy(color = Fm.colors.textSecondary))
                }
            }
            DocsText(block.text, references, onLink, body, Modifier.weight(1f))
        }
        is DocsBlock.Quote -> Row(Modifier.fillMaxWidth().padding(vertical = FmSpace.x2).height(IntrinsicSize.Min)) {
            Box(Modifier.width(3.dp).fillMaxHeight().background(Fm.colors.accent.copy(alpha = 0.6f), RoundedCornerShape(2.dp)))
            Column(Modifier.weight(1f).padding(start = FmSpace.x3)) {
                for (child in block.blocks) DocsBlockView(child, references, onLink)
            }
        }
        is DocsBlock.Code -> Box(
            Modifier
                .fillMaxWidth()
                .padding(vertical = FmSpace.x2)
                .clip(RoundedCornerShape(FmRadius.md))
                .background(Fm.colors.bgRaised)
                .border(1.dp, Fm.colors.border, RoundedCornerShape(FmRadius.md))
                .horizontalScroll(rememberScrollState())
                .padding(FmSpace.x3),
        ) {
            Text(
                block.text,
                softWrap = false,
                style = TextStyle(fontFamily = FmType.mono, fontSize = 13.sp, lineHeight = 19.sp, color = Fm.colors.textPrimary),
            )
        }
        is DocsBlock.Table -> DocsTable(block, references, onLink)
        is DocsBlock.Anchor -> Spacer(Modifier.height(1.dp))
        DocsBlock.Rule -> HorizontalDivider(Modifier.padding(vertical = FmSpace.x3), thickness = 1.dp, color = Fm.colors.border)
    }
}

@Composable
private fun DocsText(text: String, references: Map<String, String>, onLink: (String) -> Unit, style: TextStyle, modifier: Modifier = Modifier) {
    val accent = Fm.colors.accent
    val codeBackground = Fm.colors.bgRaised
    val latest by rememberUpdatedState(onLink)
    val annotated = remember(text, references, accent, codeBackground) {
        docsAnnotated(DocsMarkdown.inline(text, references), accent, codeBackground) { latest(it) }
    }
    Text(annotated, modifier = modifier, style = style)
}

internal fun docsAnnotated(spans: List<DocsSpan>, accent: Color, codeBackground: Color, onLink: (String) -> Unit): AnnotatedString =
    buildAnnotatedString {
        val links = TextLinkStyles(style = SpanStyle(color = accent, textDecoration = TextDecoration.Underline))
        for (span in spans) {
            val style = SpanStyle(
                fontWeight = if (span.bold) FontWeight.SemiBold else null,
                fontStyle = if (span.italic) FontStyle.Italic else null,
                fontFamily = if (span.code) FmType.mono else null,
                background = if (span.code) codeBackground else Color.Unspecified,
                textDecoration = if (span.strike) TextDecoration.LineThrough else null,
            )
            val href = span.href
            if (href == null) {
                withStyle(style) { append(span.text) }
            } else {
                withLink(LinkAnnotation.Clickable(href, links) { onLink(href) }) { withStyle(style) { append(span.text) } }
            }
        }
    }

@Composable
private fun DocsTable(table: DocsBlock.Table, references: Map<String, String>, onLink: (String) -> Unit) {
    val widths = remember(table, references) {
        List(table.header.size) { column ->
            val longest = (listOf(table.header) + table.rows).maxOf { row -> DocsMarkdown.plain(row.getOrElse(column) { "" }, references).length }
            (longest * 7 + 24).coerceIn(72, 280).dp
        }
    }
    val shape = RoundedCornerShape(FmRadius.md)
    Box(
        Modifier
            .fillMaxWidth()
            .padding(vertical = FmSpace.x2)
            .clip(shape)
            .border(1.dp, Fm.colors.border, shape)
            .horizontalScroll(rememberScrollState()),
    ) {
        Column {
            TableRow(table.header, table.align, widths, references, onLink, header = true)
            for (row in table.rows) {
                HorizontalDivider(Modifier.width(widths.fold(0.dp) { sum, width -> sum + width }), thickness = 1.dp, color = Fm.colors.border)
                TableRow(row, table.align, widths, references, onLink, header = false)
            }
        }
    }
}

@Composable
private fun TableRow(
    cells: List<String>,
    align: List<DocsAlign>,
    widths: List<Dp>,
    references: Map<String, String>,
    onLink: (String) -> Unit,
    header: Boolean,
) {
    val base = Fm.text.bodyMedium.copy(color = Fm.colors.textPrimary, fontWeight = if (header) FmType.emphasis else null)
    Row(Modifier.height(IntrinsicSize.Min).background(if (header) Fm.colors.bgRaised else Color.Transparent)) {
        for ((column, width) in widths.withIndex()) {
            val textAlign = when (align.getOrElse(column) { DocsAlign.Start }) {
                DocsAlign.Start -> TextAlign.Start
                DocsAlign.Center -> TextAlign.Center
                DocsAlign.End -> TextAlign.End
            }
            DocsText(
                cells.getOrElse(column) { "" },
                references,
                onLink,
                base.copy(textAlign = textAlign),
                Modifier.width(width).fillMaxHeight().padding(horizontal = FmSpace.x2, vertical = FmSpace.x2),
            )
        }
    }
}
