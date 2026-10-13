package com.forkmesh.nativeapp.composer

import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.agents.PromptAttachmentException
import com.forkmesh.nativeapp.agents.PromptAttachmentLimits
import java.io.ByteArrayOutputStream
import java.util.concurrent.atomic.AtomicInteger
import kotlin.math.max
import kotlin.math.roundToInt

data class PixelSize(val width: Int, val height: Int)

data class OrientationFix(val rotateDegrees: Int, val mirror: Boolean) {
    val swapsSides: Boolean get() = rotateDegrees == 90 || rotateDegrees == 270
}

object PhotoPrep {
    private val counter = AtomicInteger(0)

    fun scaledSize(width: Int, height: Int, maxDimension: Int = PromptAttachmentLimits.MAX_DIMENSION): PixelSize {
        if (width <= 0 || height <= 0) return PixelSize(max(1, width), max(1, height))
        val longest = max(width, height)
        if (longest <= maxDimension) return PixelSize(width, height)
        val scale = maxDimension.toDouble() / longest
        return PixelSize(max(1, (width * scale).roundToInt()), max(1, (height * scale).roundToInt()))
    }

    fun sampleSize(width: Int, height: Int, maxDimension: Int = PromptAttachmentLimits.MAX_DIMENSION): Int {
        var sample = 1
        val longest = max(width, height)
        while (longest / (sample * 2) >= maxDimension) sample *= 2
        return sample
    }

    fun isJpeg(bytes: ByteArray): Boolean =
        bytes.size >= 3 && (bytes[0].toInt() and 0xFF) == 0xFF && (bytes[1].toInt() and 0xFF) == 0xD8

    fun stripJpegMetadata(jpeg: ByteArray): ByteArray {
        if (jpeg.size < 4 || !isJpeg(jpeg)) return jpeg
        val out = ByteArrayOutputStream(jpeg.size)
        out.write(0xFF)
        out.write(0xD8)
        var i = 2
        while (i + 4 <= jpeg.size) {
            if (u(jpeg, i) != 0xFF) break
            val marker = u(jpeg, i + 1)
            if (marker == 0xFF) {
                i++
                continue
            }
            if (marker == 0xDA) {
                out.write(jpeg, i, jpeg.size - i)
                return out.toByteArray()
            }
            val length = (u(jpeg, i + 2) shl 8) or u(jpeg, i + 3)
            val end = i + 2 + length
            if (length < 2 || end > jpeg.size) break
            if (!isMetadataSegment(jpeg, i, marker, length)) out.write(jpeg, i, end - i)
            i = end
        }
        return jpeg
    }

    fun metadataMarkers(jpeg: ByteArray): List<Int> {
        if (!isJpeg(jpeg)) return emptyList()
        val found = ArrayList<Int>()
        var i = 2
        while (i + 4 <= jpeg.size) {
            if (u(jpeg, i) != 0xFF) break
            val marker = u(jpeg, i + 1)
            if (marker == 0xFF) {
                i++
                continue
            }
            if (marker == 0xDA) break
            val length = (u(jpeg, i + 2) shl 8) or u(jpeg, i + 3)
            if (length < 2 || i + 2 + length > jpeg.size) break
            if (isMetadataSegment(jpeg, i, marker, length)) found.add(marker)
            i += 2 + length
        }
        return found
    }

    private fun isMetadataSegment(jpeg: ByteArray, at: Int, marker: Int, length: Int): Boolean {
        val isJfif = marker == 0xE0 && length >= 7 && at + 7 < jpeg.size &&
            u(jpeg, at + 4) == 0x4A && u(jpeg, at + 5) == 0x46 && u(jpeg, at + 6) == 0x49 && u(jpeg, at + 7) == 0x46
        return (marker in 0xE0..0xEF && !isJfif) || marker == 0xFE
    }

    fun exifOrientation(jpeg: ByteArray): Int {
        if (!isJpeg(jpeg)) return 1
        var i = 2
        while (i + 4 <= jpeg.size) {
            if (u(jpeg, i) != 0xFF) return 1
            val marker = u(jpeg, i + 1)
            if (marker == 0xFF) {
                i++
                continue
            }
            if (marker == 0xDA || marker == 0xD9) return 1
            val length = (u(jpeg, i + 2) shl 8) or u(jpeg, i + 3)
            if (length < 2 || i + 2 + length > jpeg.size) return 1
            if (marker == 0xE1 && length >= 16 && isExifHeader(jpeg, i + 4)) {
                return readTiffOrientation(jpeg, i + 10, i + 2 + length)
            }
            i += 2 + length
        }
        return 1
    }

    private fun isExifHeader(b: ByteArray, at: Int): Boolean =
        u(b, at) == 0x45 && u(b, at + 1) == 0x78 && u(b, at + 2) == 0x69 && u(b, at + 3) == 0x66 &&
            u(b, at + 4) == 0 && u(b, at + 5) == 0

    private fun readTiffOrientation(b: ByteArray, tiff: Int, end: Int): Int {
        if (tiff + 8 > end) return 1
        val little = when {
            u(b, tiff) == 0x49 && u(b, tiff + 1) == 0x49 -> true
            u(b, tiff) == 0x4D && u(b, tiff + 1) == 0x4D -> false
            else -> return 1
        }
        fun u16(at: Int): Int = if (little) u(b, at) or (u(b, at + 1) shl 8) else (u(b, at) shl 8) or u(b, at + 1)
        fun u32(at: Int): Long = if (little) {
            (u(b, at).toLong()) or (u(b, at + 1).toLong() shl 8) or (u(b, at + 2).toLong() shl 16) or (u(b, at + 3).toLong() shl 24)
        } else {
            (u(b, at).toLong() shl 24) or (u(b, at + 1).toLong() shl 16) or (u(b, at + 2).toLong() shl 8) or u(b, at + 3).toLong()
        }
        if (u16(tiff + 2) != 42) return 1
        val ifd = tiff + u32(tiff + 4)
        if (ifd < tiff || ifd + 2 > end) return 1
        val first = ifd.toInt()
        val count = u16(first)
        for (n in 0 until count) {
            val entry = first + 2 + n * 12
            if (entry + 12 > end) return 1
            if (u16(entry) == 0x0112) {
                val value = u16(entry + 8)
                return if (value in 1..8) value else 1
            }
        }
        return 1
    }

    fun orientationFix(orientation: Int): OrientationFix = when (orientation) {
        2 -> OrientationFix(0, true)
        3 -> OrientationFix(180, false)
        4 -> OrientationFix(180, true)
        5 -> OrientationFix(90, true)
        6 -> OrientationFix(90, false)
        7 -> OrientationFix(270, true)
        8 -> OrientationFix(270, false)
        else -> OrientationFix(0, false)
    }

    fun attachmentName(raw: String): String {
        val trimmed = raw.trim().substringAfterLast('/')
        val base = if (trimmed.isEmpty()) "photo" else trimmed.replace(Regex("\\.[A-Za-z0-9]+$"), "")
        return "${base.ifEmpty { "photo" }}.jpg"
    }

    fun newId(nowMicros: Long = System.currentTimeMillis() * 1000): String =
        nowMicros.toString(36) + counter.getAndIncrement().toString(36)

    fun attachment(prepared: ByteArray, name: String, width: Int, height: Int): PromptAttachment {
        val clean = stripJpegMetadata(prepared)
        if (clean.size > PromptAttachmentLimits.MAX_BYTES) {
            throw PromptAttachmentException("That photo is still larger than 5 MB after compressing it.")
        }
        return PromptAttachment(id = newId(), name = attachmentName(name), bytes = clean, width = width, height = height)
    }

    fun roomFor(existing: Int): Int = max(0, PromptAttachmentLimits.MAX_COUNT - existing)

    fun fullMessage(): String =
        "A prompt can carry ${PromptAttachmentLimits.MAX_COUNT} photos. Remove one to add another."

    private fun u(b: ByteArray, at: Int): Int = b[at].toInt() and 0xFF
}
