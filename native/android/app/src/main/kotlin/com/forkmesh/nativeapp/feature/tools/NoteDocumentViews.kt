package com.forkmesh.nativeapp.feature.tools

import android.content.Context
import android.graphics.Color as AndroidColor
import android.os.Bundle
import android.os.CancellationSignal
import android.os.ParcelFileDescriptor
import android.print.PageRange
import android.print.PrintAttributes
import android.print.PrintDocumentAdapter
import android.print.PrintManager
import android.view.View
import android.webkit.WebResourceRequest
import android.webkit.WebView
import android.webkit.WebViewClient
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import androidx.compose.ui.viewinterop.AndroidView
import com.forkmesh.nativeapp.docs.DocsBlock
import com.forkmesh.nativeapp.feature.docs.DocsBlockView
import com.forkmesh.nativeapp.feature.tools.vault.findActivity
import com.forkmesh.nativeapp.notes.NoteDocument
import com.forkmesh.nativeapp.notes.NoteFigure
import com.forkmesh.nativeapp.notes.NoteLayout
import com.forkmesh.nativeapp.notes.NoteMargins
import com.forkmesh.nativeapp.notes.NoteOrientation
import com.forkmesh.nativeapp.notes.NotePaper
import com.forkmesh.nativeapp.shell.Segmented
import com.forkmesh.nativeapp.ui.kit.FmCheckbox
import com.forkmesh.nativeapp.ui.kit.FmModalBottomSheet
import com.forkmesh.nativeapp.ui.kit.FmPanelHeader
import com.forkmesh.nativeapp.ui.kit.FmRowDivider
import com.forkmesh.nativeapp.ui.kit.FmSwitch
import com.forkmesh.nativeapp.ui.kit.FmTone
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.theme.Fm
import java.time.LocalDate

@Composable
internal fun NoteFigureView(figure: NoteFigure) {
    val box = remember(figure.svg) { NoteDocument.figureBox(figure.svg) }
    Column(Modifier.fillMaxWidth().padding(vertical = 8.dp), horizontalAlignment = Alignment.CenterHorizontally) {
        Box(
            Modifier
                .fillMaxWidth()
                .clip(RoundedCornerShape(12.dp))
                .background(Color.White)
                .padding(12.dp)
                .semantics { contentDescription = figure.caption.ifEmpty { "Diagram" } },
            contentAlignment = Alignment.Center,
        ) {
            BoxWithConstraints(Modifier.fillMaxWidth(), contentAlignment = Alignment.Center) {
                val column = maxWidth.value.toDouble()
                val width = minOf(box.width ?: column, column)
                val height = (width * box.aspect).coerceIn(1.0, 4000.0)
                Box(Modifier.size(width.dp, height.dp)) {
                    var failed by remember(figure.svg) { mutableStateOf(false) }
                    if (failed) {
                        NoteFigureFallback(figure)
                    } else {
                        AndroidView(
                            factory = { context ->
                                runCatching { figureWebView(context, figure) }.getOrElse {
                                    failed = true
                                    View(context)
                                }
                            },
                            modifier = Modifier.matchParentSize(),
                        )
                        Box(Modifier.matchParentSize().pointerInput(Unit) {})
                    }
                }
            }
        }
        if (figure.caption.isNotEmpty()) {
            Text(
                figure.caption,
                style = Fm.text.bodySmall.copy(color = Fm.colors.textTertiary),
                textAlign = TextAlign.Center,
                modifier = Modifier.fillMaxWidth().padding(top = 6.dp),
            )
        }
    }
}

@Composable
private fun NoteFigureFallback(figure: NoteFigure) {
    SelectionContainer {
        Text(figure.svg, style = Fm.text.bodySmall.copy(color = Color(0xFF1F2328), fontFamily = FontFamily.Monospace), maxLines = 12)
    }
}

private fun figureWebView(context: Context, figure: NoteFigure): WebView = WebView(context).apply {
    settings.javaScriptEnabled = false
    settings.allowFileAccess = false
    settings.allowContentAccess = false
    settings.blockNetworkLoads = true
    settings.loadsImagesAutomatically = true
    isVerticalScrollBarEnabled = false
    isHorizontalScrollBarEnabled = false
    overScrollMode = View.OVER_SCROLL_NEVER
    isFocusable = false
    isClickable = false
    importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO_HIDE_DESCENDANTS
    setBackgroundColor(AndroidColor.WHITE)
    webViewClient = object : WebViewClient() {
        override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean = true
    }
    loadDataWithBaseURL(null, NoteDocument.figurePreviewHtml(figure), "text/html", "utf-8", null)
}

@Composable
internal fun NotePageBreakView() {
    val line = Fm.colors.textTertiary
    Row(
        Modifier.fillMaxWidth().padding(vertical = 12.dp).semantics { contentDescription = "Page break" },
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        DashedRule(line, Modifier.weight(1f))
        Text("Page break", style = Fm.text.labelSmall.copy(color = line))
        DashedRule(line, Modifier.weight(1f))
    }
}

@Composable
private fun DashedRule(color: Color, modifier: Modifier) {
    Canvas(modifier.height(1.dp)) {
        drawLine(
            color = color,
            start = Offset(0f, size.height / 2),
            end = Offset(size.width, size.height / 2),
            strokeWidth = 1.dp.toPx(),
            pathEffect = PathEffect.dashPathEffect(floatArrayOf(6.dp.toPx(), 4.dp.toPx())),
        )
    }
}

internal sealed class NotePreviewPart {
    class Blocks(val blocks: List<DocsBlock>) : NotePreviewPart()
    class Figure(val figure: NoteFigure) : NotePreviewPart()
    data object PageBreak : NotePreviewPart()
}

@Composable
internal fun NotePreviewPartView(part: NotePreviewPart, references: Map<String, String>, onLink: (String) -> Unit) {
    when (part) {
        is NotePreviewPart.Blocks -> for (block in part.blocks) DocsBlockView(block, references, onLink)
        is NotePreviewPart.Figure -> NoteFigureView(part.figure)
        NotePreviewPart.PageBreak -> NotePageBreakView()
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun NotePdfSheet(initial: NoteLayout, canSave: Boolean, onDismiss: () -> Unit, onExport: (NoteLayout, Boolean) -> Unit) {
    var layout by remember { mutableStateOf(initial) }
    var save by remember { mutableStateOf(canSave) }
    FmModalBottomSheet(onDismissRequest = onDismiss, isScrollControlled = true) {
        Column(Modifier.verticalScroll(rememberScrollState()).padding(bottom = 16.dp)) {
            FmPanelHeader(title = NoteDocument.MENU_LABEL)
            Column(Modifier.fillMaxWidth().padding(horizontal = 16.dp)) {
                NotePdfChoice("Paper", NotePaper.entries, layout.paper, { it.label }) { layout = layout.copy(paper = it) }
                NotePdfChoice("Orientation", NoteOrientation.entries, layout.orientation, { it.label }) { layout = layout.copy(orientation = it) }
                NotePdfChoice("Margins", NoteMargins.entries, layout.margins, { it.label }) { layout = layout.copy(margins = it) }
                Spacer(Modifier.height(4.dp))
                NotePdfToggle("Cover page", layout.cover) { layout = layout.copy(cover = it) }
                NotePdfToggle("Contents", layout.contents) { layout = layout.copy(contents = it) }
                NotePdfToggle("Page numbers", layout.numbers) { layout = layout.copy(numbers = it) }
                if (canSave) {
                    FmRowDivider(Modifier.padding(vertical = 4.dp), indent = 0.dp)
                    Row(
                        Modifier
                            .fillMaxWidth()
                            .heightIn(min = 44.dp)
                            .toggleable(value = save, role = Role.Checkbox) { save = it },
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        FmCheckbox(checked = save, onCheckedChange = null)
                        Text("Save as this note's page layout", style = Fm.text.bodyMedium.copy(color = Fm.colors.textPrimary), modifier = Modifier.weight(1f))
                    }
                }
                Spacer(Modifier.height(12.dp))
                FmToneButton(
                    NoteDocument.MENU_LABEL,
                    onPressed = { onExport(layout, canSave && save) },
                    modifier = Modifier.fillMaxWidth(),
                    tone = FmTone.Accent,
                    filled = true,
                    height = 44.dp,
                )
            }
        }
    }
}

@Composable
private fun <T> NotePdfChoice(title: String, choices: List<T>, selected: T, label: (T) -> String, onPick: (T) -> Unit) {
    Column(Modifier.fillMaxWidth().padding(top = 8.dp)) {
        Text(title, style = Fm.text.labelSmall.copy(color = Fm.colors.textTertiary))
        Segmented(choices, selected, label, Modifier.fillMaxWidth(), onPick)
    }
}

@Composable
private fun NotePdfToggle(title: String, checked: Boolean, onChange: (Boolean) -> Unit) {
    Row(Modifier.fillMaxWidth().heightIn(min = 44.dp), verticalAlignment = Alignment.CenterVertically) {
        Text(title, Modifier.weight(1f), style = Fm.text.bodyLarge.copy(color = Fm.colors.textPrimary))
        Spacer(Modifier.width(8.dp))
        FmSwitch(checked = checked, onCheckedChange = onChange)
    }
}

internal object NotePdfPrinter {
    private var pending: WebView? = null

    fun print(context: Context, title: String, emoji: String, markdown: String, layout: NoteLayout): Boolean {
        val activity = context.findActivity() ?: return false
        val manager = activity.getSystemService(PrintManager::class.java) ?: return false
        val html = NoteDocument.printHtml(title, emoji, markdown, layout, LocalDate.now())
        val name = NoteDocument.pdfFileName(title)
        val view = runCatching { WebView(activity) }.getOrNull() ?: return false
        view.settings.javaScriptEnabled = false
        view.settings.allowFileAccess = false
        view.settings.allowContentAccess = false
        view.setBackgroundColor(AndroidColor.WHITE)
        var started = false
        view.webViewClient = object : WebViewClient() {
            override fun shouldOverrideUrlLoading(view: WebView, request: WebResourceRequest): Boolean = true

            override fun onPageFinished(page: WebView, url: String?) {
                if (started) return
                started = true
                val adapter = FinishingAdapter(page.createPrintDocumentAdapter(name)) {
                    if (pending === page) pending = null
                    page.destroy()
                }
                val attributes = PrintAttributes.Builder()
                    .setMediaSize(mediaSize(layout))
                    .setMinMargins(PrintAttributes.Margins.NO_MARGINS)
                    .setColorMode(PrintAttributes.COLOR_MODE_COLOR)
                    .build()
                runCatching { manager.print(name.removeSuffix(".pdf"), adapter, attributes) }.onFailure {
                    if (pending === page) pending = null
                    page.destroy()
                }
            }
        }
        pending = view
        view.loadDataWithBaseURL(null, html, "text/html", "utf-8", null)
        return true
    }

    private fun mediaSize(layout: NoteLayout): PrintAttributes.MediaSize {
        val size = when (layout.paper) {
            NotePaper.A4 -> PrintAttributes.MediaSize.ISO_A4
            NotePaper.Letter -> PrintAttributes.MediaSize.NA_LETTER
            NotePaper.Legal -> PrintAttributes.MediaSize.NA_LEGAL
        }
        return if (layout.orientation == NoteOrientation.Landscape) size.asLandscape() else size.asPortrait()
    }

    private class FinishingAdapter(private val inner: PrintDocumentAdapter, private val done: () -> Unit) : PrintDocumentAdapter() {
        override fun onStart() = inner.onStart()

        override fun onLayout(
            oldAttributes: PrintAttributes?,
            newAttributes: PrintAttributes,
            cancellationSignal: CancellationSignal?,
            callback: LayoutResultCallback,
            extras: Bundle?,
        ) = inner.onLayout(oldAttributes, newAttributes, cancellationSignal, callback, extras)

        override fun onWrite(pages: Array<out PageRange>, destination: ParcelFileDescriptor, cancellationSignal: CancellationSignal?, callback: WriteResultCallback) =
            inner.onWrite(pages, destination, cancellationSignal, callback)

        override fun onFinish() {
            inner.onFinish()
            done()
        }
    }
}
