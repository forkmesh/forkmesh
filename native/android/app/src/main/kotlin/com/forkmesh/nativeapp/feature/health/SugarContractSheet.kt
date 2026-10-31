package com.forkmesh.nativeapp.feature.health

import android.content.ActivityNotFoundException
import android.content.Context
import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.DashPathEffect
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.text.Layout
import android.text.StaticLayout
import android.text.TextPaint
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.DatePicker
import androidx.compose.material3.DatePickerDialog
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.SelectableDates
import androidx.compose.material3.Text
import androidx.compose.material3.rememberDatePickerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.input.KeyboardCapitalization
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.unit.dp
import androidx.core.content.FileProvider
import com.forkmesh.nativeapp.feature.composer.PhotoPicker
import com.forkmesh.nativeapp.health.HealthDates
import com.forkmesh.nativeapp.health.SugarContract
import com.forkmesh.nativeapp.health.SugarContractLine
import com.forkmesh.nativeapp.health.SugarContractText
import com.forkmesh.nativeapp.ui.kit.FmCard
import com.forkmesh.nativeapp.ui.kit.FmField
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmSoundOnShow
import com.forkmesh.nativeapp.ui.kit.FmTextButton
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.kit.fmSafeAreaInsets
import com.forkmesh.nativeapp.ui.kit.fmTapSounds
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import java.io.File
import java.time.Instant
import java.time.ZoneOffset

@Composable
internal fun SugarContractSheet(initial: SugarContract) {
    val c = Fm.colors
    val context = LocalContext.current
    var contract by remember { mutableStateOf(initial) }
    var picking by remember { mutableStateOf<String?>(null) }
    val picture = remember(contract) { SugarContractCard.render(contract) }

    Column(
        Modifier
            .fillMaxWidth()
            .verticalScroll(rememberScrollState())
            .windowInsetsPadding(fmSafeAreaInsets(top = false))
            .imePadding()
            .padding(FmSpace.x4),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x3),
    ) {
        Text(SugarContract.TITLE, style = Fm.text.titleMedium)
        Image(
            bitmap = picture.asImageBitmap(),
            contentDescription = "${SugarContract.TITLE}, ${contract.dateLine}, ${contract.signers} signature lines",
            contentScale = ContentScale.FillWidth,
            modifier = Modifier.fillMaxWidth().clip(RoundedCornerShape(FmRadius.md)),
        )
        FmCard(Modifier.fillMaxWidth()) {
            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                DateRow("From", SugarContractText.full(contract.startDay)) { picking = "start" }
                DateRow("Through", SugarContractText.full(contract.endDay)) { picking = "end" }
                Row(Modifier.fillMaxWidth().heightIn(min = 44.dp), verticalAlignment = Alignment.CenterVertically) {
                    Text(
                        if (contract.signers == 1) "1 signature" else "${contract.signers} signatures",
                        style = Fm.text.bodyLarge,
                        modifier = Modifier.weight(1f),
                    )
                    FmIconButton(
                        "remove", "Fewer signatures",
                        onPressed = if (contract.signers > SugarContract.SIGNER_RANGE.first) ({ contract = contract.withSigners(contract.signers - 1) }) else null,
                        busy = false,
                    )
                    FmIconButton(
                        "add", "More signatures",
                        onPressed = if (contract.signers < SugarContract.SIGNER_RANGE.last) ({ contract = contract.withSigners(contract.signers + 1) }) else null,
                        busy = false,
                    )
                }
            }
        }
        FmField("Names under the lines (optional)") {
            Column(verticalArrangement = Arrangement.spacedBy(FmSpace.x2)) {
                for (index in 0 until contract.signers) {
                    FmTextField(
                        value = contract.names.getOrElse(index) { "" },
                        onValueChange = { contract = contract.withName(index, it) },
                        hint = "Signer ${index + 1}",
                        keyboardOptions = KeyboardOptions(capitalization = KeyboardCapitalization.Words),
                    )
                }
            }
        }
        SfSolidButton(
            "Send contract",
            c.success,
            onClick = { share(context, contract, picture) },
            icon = "ios_share",
            minHeight = 52.dp,
            modifier = Modifier.fillMaxWidth(),
        )
    }

    when (picking) {
        "start" -> DayPickerDialog(
            contract.startDay, null,
            onDismiss = { picking = null },
            onPicked = { contract = contract.withStart(it); picking = null },
        )
        "end" -> DayPickerDialog(
            contract.endDay, contract.startDay,
            onDismiss = { picking = null },
            onPicked = { contract = contract.withEnd(it); picking = null },
        )
    }
}

@Composable
private fun DateRow(label: String, value: String, onClick: () -> Unit) {
    Row(
        Modifier.fillMaxWidth().heightIn(min = 44.dp).clickable(onClick = onClick),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, style = Fm.text.bodyLarge, modifier = Modifier.weight(1f))
        Box(
            Modifier
                .clip(RoundedCornerShape(FmRadius.md))
                .background(Fm.colors.bgOverlay)
                .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
        ) {
            Text(value, style = Fm.text.bodyLarge.copy(color = Fm.colors.accent))
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun DayPickerDialog(day: String, notBefore: String?, onDismiss: () -> Unit, onPicked: (String) -> Unit) {
    val initial = (HealthDates.parseDay(day) ?: java.time.LocalDate.now()).atStartOfDay(ZoneOffset.UTC).toInstant().toEpochMilli()
    val floor = notBefore?.let { HealthDates.parseDay(it)?.atStartOfDay(ZoneOffset.UTC)?.toInstant()?.toEpochMilli() }
    val state = rememberDatePickerState(
        initialSelectedDateMillis = initial,
        selectableDates = object : SelectableDates {
            override fun isSelectableDate(utcTimeMillis: Long): Boolean = floor == null || utcTimeMillis >= floor
        },
    )
    FmSoundOnShow()
    DatePickerDialog(
        onDismissRequest = onDismiss,
        modifier = Modifier.fmTapSounds(),
        confirmButton = {
            FmTextButton(onClick = {
                val millis = state.selectedDateMillis
                if (millis == null) {
                    onDismiss()
                } else {
                    onPicked(Instant.ofEpochMilli(millis).atZone(ZoneOffset.UTC).toLocalDate().toString())
                }
            }) { Text("OK") }
        },
        dismissButton = { FmTextButton(onClick = onDismiss) { Text("Cancel") } },
    ) { DatePicker(state) }
}

private fun share(context: Context, contract: SugarContract, picture: Bitmap) {
    val uri = runCatching {
        val folder = File(File(context.cacheDir, "files-open"), "sugar-contract").apply { mkdirs() }
        val target = File(folder, contract.fileName)
        target.outputStream().use { picture.compress(Bitmap.CompressFormat.PNG, 100, it) }
        FileProvider.getUriForFile(context, PhotoPicker.authority(context), target)
    }.getOrNull() ?: return
    val send = Intent(Intent.ACTION_SEND)
        .setType("image/png")
        .putExtra(Intent.EXTRA_STREAM, uri)
        .putExtra(Intent.EXTRA_SUBJECT, SugarContract.TITLE)
        .putExtra(Intent.EXTRA_TEXT, contract.message)
        .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
    send.clipData = android.content.ClipData.newRawUri(contract.fileName, uri)
    try {
        context.startActivity(Intent.createChooser(send, SugarContract.TITLE).addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION))
    } catch (_: ActivityNotFoundException) {
    }
}

internal object SugarContractCard {
    private const val WIDTH = 360f
    private const val SCALE = 3f
    private const val PAPER = 0xFFFBF7EE.toInt()
    private const val INK = 0xFF1F2A24.toInt()
    private const val GREEN = 0xFF2F7D5B.toInt()
    private const val MUTED = 0xFF6B7A70.toInt()
    private const val INNER = 300f
    private const val PAD_X = 30f
    private const val COLUMN_GAP = 22f
    private const val ROW_GAP = 18f

    private fun withAlpha(color: Int, alpha: Float): Int = (color and 0x00FFFFFF) or ((alpha * 255).toInt() shl 24)

    private fun paint(size: Float, color: Int, bold: Boolean = false, serif: Boolean = false, heavy: Boolean = false, tracking: Float = 0f): TextPaint =
        TextPaint(Paint.ANTI_ALIAS_FLAG).apply {
            textSize = size
            this.color = color
            typeface = Typeface.create(
                if (serif) Typeface.SERIF else Typeface.SANS_SERIF,
                if (bold || heavy) Typeface.BOLD else Typeface.NORMAL,
            )
            if (heavy) isFakeBoldText = true
            letterSpacing = if (tracking == 0f) 0f else tracking / size
        }

    private fun height(paint: Paint): Float = paint.fontMetrics.let { it.descent - it.ascent }

    private fun Canvas?.text(text: String, paint: Paint, x: Float, top: Float) {
        this?.drawText(text, x, top - paint.fontMetrics.ascent, paint)
    }

    private fun Canvas?.centered(text: String, paint: Paint, top: Float, extra: Float = 0f) {
        val width = paint.measureText(text) - extra
        text(text, paint, (WIDTH - width) / 2, top)
    }

    fun render(contract: SugarContract): Bitmap {
        val total = layout(contract, null)
        val bitmap = Bitmap.createBitmap(Math.ceil((WIDTH * SCALE).toDouble()).toInt(), Math.ceil((total * SCALE).toDouble()).toInt(), Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bitmap)
        canvas.drawColor(PAPER)
        canvas.scale(SCALE, SCALE)
        layout(contract, canvas)
        return bitmap
    }

    private fun layout(contract: SugarContract, canvas: Canvas?): Float {
        var y = 32f
        val gap = 14f

        val brand = paint(9.5f, GREEN, bold = true, tracking = 2.4f)
        val brandHeight = height(brand)
        val brandWidth = brand.measureText(SugarContract.BRAND)
        val brandLeft = (WIDTH - brandWidth) / 2
        canvas?.let {
            val line = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = withAlpha(GREEN, 0.45f); strokeWidth = 0.75f }
            val mid = y + brandHeight / 2
            it.drawLine(PAD_X, mid, brandLeft - 10f, mid, line)
            it.drawLine(brandLeft + brandWidth + 10f, mid, WIDTH - PAD_X, mid, line)
        }
        canvas.text(SugarContract.BRAND, brand, brandLeft, y)
        y += brandHeight + gap

        canvas?.let {
            val cx = WIDTH / 2
            val cy = y + 28f
            it.save()
            it.rotate(-8f, cx, cy)
            it.drawCircle(cx, cy, 28f, Paint(Paint.ANTI_ALIAS_FLAG).apply { color = GREEN })
            val dash = Paint(Paint.ANTI_ALIAS_FLAG).apply {
                style = Paint.Style.STROKE
                strokeWidth = 1f
                color = withAlpha(PAPER, 0.85f)
                pathEffect = DashPathEffect(floatArrayOf(2f, 2f), 0f)
            }
            it.drawCircle(cx, cy, 24f, dash)
            val no = paint(11f, PAPER, heavy = true)
            val sugar = paint(9f, PAPER, heavy = true, tracking = 0.8f)
            val noHeight = height(no)
            val sugarHeight = height(sugar)
            val top = cy - (noHeight + sugarHeight) / 2
            it.text("NO", no, cx - no.measureText("NO") / 2, top)
            it.text("SUGAR", sugar, cx - (sugar.measureText("SUGAR") - 0.8f) / 2, top + noHeight)
            it.restore()
        }
        y += 56f + gap

        val title = paint(28f, INK, bold = true, serif = true)
        canvas.centered(SugarContract.TITLE, title, y)
        y += height(title) + gap

        val pillText = paint(13f, GREEN, bold = true)
        val pillLabel = "${contract.dateLine}  ·  ${contract.lengthLine}"
        val pillWidth = pillText.measureText(pillLabel) + 24f
        val pillHeight = height(pillText) + 10f
        canvas?.let {
            val left = (WIDTH - pillWidth) / 2
            it.drawRoundRect(RectF(left, y, left + pillWidth, y + pillHeight), pillHeight / 2, pillHeight / 2, Paint(Paint.ANTI_ALIAS_FLAG).apply { color = withAlpha(GREEN, 0.12f) })
        }
        canvas.text(pillLabel, pillText, (WIDTH - pillWidth) / 2 + 12f, y + 5f)
        y += pillHeight + gap

        val terms = StaticLayout.Builder.obtain(contract.terms, 0, contract.terms.length, paint(14f, INK, serif = true), INNER.toInt())
            .setAlignment(Layout.Alignment.ALIGN_CENTER)
            .build()
        canvas?.let {
            it.save()
            it.translate(PAD_X, y)
            terms.draw(it)
            it.restore()
        }
        y += terms.height + gap

        val fine = paint(10.5f, MUTED, bold = false)
        canvas.centered(SugarContract.FINEPRINT, fine, y)
        y += height(fine) + gap + 4f

        y = signatures(contract, canvas, y) + gap + 2f

        val footer = paint(8.5f, withAlpha(MUTED, 0.8f), tracking = 0.6f)
        canvas.centered("Signed with ForkMesh Sugar Break", footer, y, extra = 0.6f)
        y += height(footer) + 32f

        canvas?.let {
            val outer = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 1.5f; color = withAlpha(GREEN, 0.75f) }
            val inner = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeWidth = 0.75f; color = withAlpha(GREEN, 0.35f) }
            it.drawRect(RectF(10f, 10f, WIDTH - 10f, y - 10f), outer)
            it.drawRect(RectF(14f, 14f, WIDTH - 14f, y - 14f), inner)
        }
        return y
    }

    private fun signatures(contract: SugarContract, canvas: Canvas?, start: Float): Float {
        val lines = contract.lines
        val perRow = if (lines.size == 1) 1 else 2
        val cellWidth = if (lines.size == 1) INNER else (INNER - COLUMN_GAP) / 2
        var y = start
        val rows = lines.chunked(perRow)
        rows.forEachIndexed { index, row ->
            if (index > 0) y += ROW_GAP
            var x = PAD_X
            var rowHeight = 0f
            for (line in row) {
                rowHeight = maxOf(rowHeight, signature(line, canvas, x, y, cellWidth))
                x += cellWidth + COLUMN_GAP
            }
            y += rowHeight
        }
        return y
    }

    private fun signature(line: SugarContractLine, canvas: Canvas?, x: Float, y: Float, width: Float): Float {
        val named = line.name.isNotEmpty()
        val caption = paint(9.5f, if (named) INK else MUTED, bold = named)
        val dateLabel = paint(9.5f, MUTED)
        val captionHeight = height(caption)
        canvas?.let {
            val cross = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = MUTED; strokeWidth = 1.1f; strokeCap = Paint.Cap.ROUND }
            val bottom = y + 30f
            it.drawLine(x, bottom - 7f, x + 7f, bottom, cross)
            it.drawLine(x + 7f, bottom - 7f, x, bottom, cross)
            val rule = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = withAlpha(INK, 0.7f); strokeWidth = 0.9f }
            it.drawLine(x + 11f, bottom - 0.45f, x + width, bottom - 0.45f, rule)
            val dateWidth = dateLabel.measureText("Date")
            val room = width - dateWidth - 4f
            var shown: TextPaint = caption
            var text = line.caption
            val natural = caption.measureText(text)
            if (natural > room) {
                val factor = maxOf(0.7f, room / natural)
                shown = paint(9.5f * factor, if (named) INK else MUTED, bold = named)
                text = android.text.TextUtils.ellipsize(text, shown, room, android.text.TextUtils.TruncateAt.END).toString()
            }
            it.text(text, shown, x, y + 34f)
            it.text("Date", dateLabel, x + width - dateWidth, y + 34f)
        }
        return 30f + 4f + captionHeight
    }
}
