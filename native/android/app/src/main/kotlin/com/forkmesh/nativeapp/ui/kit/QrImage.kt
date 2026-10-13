package com.forkmesh.nativeapp.ui.kit

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.size
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.qr.QrCode

@Composable
fun QrImage(
    text: String,
    modifier: Modifier = Modifier,
    size: Dp = 200.dp,
    ecl: QrCode.Ecl = QrCode.Ecl.Medium,
    margin: Int = 4,
    description: String = "QR code",
) {
    val matrix = remember(text, ecl) { QrCode.encodeText(text, ecl) } ?: return
    Canvas(modifier.size(size).aspectRatio(1f).semantics { contentDescription = description }) {
        drawRect(Color.White)
        val cells = matrix.size + margin * 2
        val cell = this.size.minDimension / cells
        for (y in 0 until matrix.size) {
            for (x in 0 until matrix.size) {
                if (matrix[x, y]) {
                    drawRect(Color.Black, Offset((x + margin) * cell, (y + margin) * cell), Size(cell + 0.5f, cell + 0.5f))
                }
            }
        }
    }
}
