package com.forkmesh.nativeapp.composer

import com.forkmesh.nativeapp.agents.PromptAttachmentException
import com.forkmesh.nativeapp.agents.PromptAttachmentLimits
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.awt.image.BufferedImage
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import javax.imageio.IIOImage
import javax.imageio.ImageIO
import javax.imageio.ImageWriteParam

class PhotoPrepTest {
    private fun jpeg(width: Int, height: Int): ByteArray {
        val image = BufferedImage(width, height, BufferedImage.TYPE_INT_RGB)
        for (y in 0 until height) for (x in 0 until width) image.setRGB(x, y, (x * 7 + y * 13) and 0xFFFFFF)
        val out = ByteArrayOutputStream()
        val writer = ImageIO.getImageWritersByFormatName("jpeg").next()
        val params = writer.defaultWriteParam
        params.compressionMode = ImageWriteParam.MODE_EXPLICIT
        params.compressionQuality = 0.8f
        ImageIO.createImageOutputStream(out).use { stream ->
            writer.output = stream
            writer.write(null, IIOImage(image, null, null), params)
        }
        writer.dispose()
        return out.toByteArray()
    }

    private fun segment(marker: Int, payload: ByteArray): ByteArray {
        val length = payload.size + 2
        return byteArrayOf(0xFF.toByte(), marker.toByte(), (length shr 8).toByte(), length.toByte()) + payload
    }

    private fun exifPayload(orientation: Int, littleEndian: Boolean): ByteArray {
        val out = ByteArrayOutputStream()
        out.write("Exif".toByteArray())
        out.write(0)
        out.write(0)
        fun u16(value: Int) {
            if (littleEndian) {
                out.write(value and 0xFF)
                out.write(value shr 8 and 0xFF)
            } else {
                out.write(value shr 8 and 0xFF)
                out.write(value and 0xFF)
            }
        }
        fun u32(value: Int) {
            if (littleEndian) {
                u16(value and 0xFFFF)
                u16(value ushr 16)
            } else {
                u16(value ushr 16)
                u16(value and 0xFFFF)
            }
        }
        out.write(if (littleEndian) "II".toByteArray() else "MM".toByteArray())
        u16(42)
        u32(8)
        u16(2)
        u16(0x010F)
        u16(2)
        u32(4)
        out.write("Cam".toByteArray())
        out.write(0)
        u16(0x0112)
        u16(3)
        u32(1)
        u16(orientation)
        u16(0)
        u32(0)
        out.write("GPS 51.5,-0.12".toByteArray())
        return out.toByteArray()
    }

    private fun withSegments(jpeg: ByteArray, vararg segments: ByteArray): ByteArray {
        val soi = jpeg.copyOfRange(0, 2)
        return segments.fold(soi) { acc, s -> acc + s } + jpeg.copyOfRange(2, jpeg.size)
    }

    fun testScaledSizeKeepsAspectAndCapsTheLongSide() {
        assertEq(PixelSize(2048, 1536), PhotoPrep.scaledSize(4032, 3024))
        assertEq(PixelSize(1536, 2048), PhotoPrep.scaledSize(3024, 4032))
        assertEq(PixelSize(800, 600), PhotoPrep.scaledSize(800, 600))
        assertEq(PixelSize(2048, 2048), PhotoPrep.scaledSize(2048, 2048))
        assertEq(PixelSize(2048, 1), PhotoPrep.scaledSize(10000, 2))
        assertEq(PixelSize(1, 1), PhotoPrep.scaledSize(0, 0))
    }

    fun testSampleSizeNeverUndershootsTheTarget() {
        assertEq(1, PhotoPrep.sampleSize(4000, 3000))
        assertEq(2, PhotoPrep.sampleSize(8000, 6000))
        assertEq(4, PhotoPrep.sampleSize(8192, 6144))
        assertEq(8, PhotoPrep.sampleSize(16384, 1000))
        for (w in listOf(100, 2048, 4095, 4096, 4097, 9000, 20000)) {
            expect(w / PhotoPrep.sampleSize(w, 1) >= minOf(w, 2048), "width $w")
        }
    }

    fun testStripRemovesExifXmpAndCommentsButKeepsTheImage() {
        val plain = jpeg(64, 48)
        val tagged = withSegments(
            plain,
            segment(0xE1, exifPayload(6, littleEndian = true)),
            segment(0xE1, "http://ns.adobe.com/xap/1.0/\u0000<x:xmpmeta/>".toByteArray()),
            segment(0xE2, "ICC_PROFILE\u0000junk".toByteArray()),
            segment(0xFE, "shot by alice".toByteArray()),
        )
        assertEq(listOf(0xE1, 0xE1, 0xE2, 0xFE), PhotoPrep.metadataMarkers(tagged))
        val clean = PhotoPrep.stripJpegMetadata(tagged)
        assertEq(emptyList<Int>(), PhotoPrep.metadataMarkers(clean))
        expect(!String(clean, Charsets.ISO_8859_1).contains("GPS"))
        expect(!String(clean, Charsets.ISO_8859_1).contains("alice"))
        val decoded = ImageIO.read(ByteArrayInputStream(clean))
        assertEq(64, decoded.width)
        assertEq(48, decoded.height)
        expect(String(clean, Charsets.ISO_8859_1).contains("JFIF"), "the JFIF header stays")
    }

    fun testStripLeavesUnknownDataAlone() {
        val notJpeg = "hello".toByteArray()
        expect(PhotoPrep.stripJpegMetadata(notJpeg).contentEquals(notJpeg))
        val truncated = jpeg(8, 8).copyOfRange(0, 30) + byteArrayOf(0xFF.toByte(), 0xE1.toByte(), 0x7F, 0x7F)
        expect(PhotoPrep.stripJpegMetadata(truncated).contentEquals(truncated))
    }

    fun testExifOrientationInBothByteOrders() {
        val plain = jpeg(16, 16)
        for (value in 1..8) {
            assertEq(value, PhotoPrep.exifOrientation(withSegments(plain, segment(0xE1, exifPayload(value, true)))))
            assertEq(value, PhotoPrep.exifOrientation(withSegments(plain, segment(0xE1, exifPayload(value, false)))))
        }
        assertEq(1, PhotoPrep.exifOrientation(plain))
        assertEq(1, PhotoPrep.exifOrientation(withSegments(plain, segment(0xE1, exifPayload(12, true)))))
        assertEq(1, PhotoPrep.exifOrientation("png?".toByteArray()))
    }

    fun testOrientationFixes() {
        assertEq(OrientationFix(0, false), PhotoPrep.orientationFix(1))
        assertEq(OrientationFix(90, false), PhotoPrep.orientationFix(6))
        assertEq(OrientationFix(270, false), PhotoPrep.orientationFix(8))
        assertEq(OrientationFix(180, false), PhotoPrep.orientationFix(3))
        assertEq(OrientationFix(0, true), PhotoPrep.orientationFix(2))
        expect(PhotoPrep.orientationFix(6).swapsSides)
        expect(!PhotoPrep.orientationFix(3).swapsSides)
    }

    fun testAttachmentNamesAreJpegAndNeverCarryAPath() {
        assertEq("IMG_0001.jpg", PhotoPrep.attachmentName("IMG_0001.HEIC"))
        assertEq("photo.jpg", PhotoPrep.attachmentName(""))
        assertEq("shot.jpg", PhotoPrep.attachmentName("/sdcard/DCIM/shot.png"))
        assertEq("photo.jpg", PhotoPrep.attachmentName(".jpg"))
    }

    fun testAttachmentIsStrippedAndSizeChecked() {
        val tagged = withSegments(jpeg(32, 32), segment(0xE1, exifPayload(1, true)))
        val attachment = PhotoPrep.attachment(tagged, "a.png", 32, 32)
        assertEq("a.jpg", attachment.name)
        assertEq("image/jpeg", attachment.mediaType)
        assertEq(emptyList<Int>(), PhotoPrep.metadataMarkers(attachment.bytes))
        val other = PhotoPrep.attachment(tagged, "b", 32, 32)
        expect(attachment.id != other.id)
        val huge = ByteArray(PromptAttachmentLimits.MAX_BYTES + 1)
        val thrown = assertFails<PromptAttachmentException> { PhotoPrep.attachment(huge, "x", 1, 1) }
        assertEq("That photo is still larger than 5 MB after compressing it.", thrown.message)
    }

    fun testRoomAndLimitsMatchTheRelay() {
        assertEq(5, PhotoPrep.roomFor(0))
        assertEq(0, PhotoPrep.roomFor(7))
        assertEq(2048, PromptAttachmentLimits.MAX_DIMENSION)
        assertEq(80, PromptAttachmentLimits.JPEG_QUALITY)
        assertEq("A prompt can carry 5 photos. Remove one to add another.", PhotoPrep.fullMessage())
    }
}
