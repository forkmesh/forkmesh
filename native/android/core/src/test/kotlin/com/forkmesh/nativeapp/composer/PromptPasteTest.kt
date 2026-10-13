package com.forkmesh.nativeapp.composer

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.util.Base64

class PromptPasteTest {
    private val png = byteArrayOf(0x89.toByte(), 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0x0D)
    private val jpeg = byteArrayOf(0xFF.toByte(), 0xD8.toByte(), 0xFF.toByte(), 0xE0.toByte(), 0, 0x10)

    fun testRecognisesImageBytesByTheirSignature() {
        expect(PromptPasteRules.looksLikeImage(png))
        expect(PromptPasteRules.looksLikeImage(jpeg))
        expect(PromptPasteRules.looksLikeImage("GIF89a".toByteArray()))
        expect(PromptPasteRules.looksLikeImage("RIFF\u0000\u0000\u0000\u0000WEBPVP8 ".toByteArray(Charsets.ISO_8859_1)))
        expect(PromptPasteRules.looksLikeImage("\u0000\u0000\u0000\u0018ftypheic".toByteArray(Charsets.ISO_8859_1)))
        expect(!PromptPasteRules.looksLikeImage("\u0000\u0000\u0000\u0018ftypmp42".toByteArray(Charsets.ISO_8859_1)))
        expect(!PromptPasteRules.looksLikeImage("hello".toByteArray()))
        expect(!PromptPasteRules.looksLikeImage(ByteArray(0)))
    }

    fun testImagesBecomeAttachmentsAndTextIsKept() {
        val paste = PromptPasteRules.paste(
            listOf(
                PasteboardItem(mapOf("image/png" to png)),
                PasteboardItem(mapOf("image/jpeg" to "not really".toByteArray(), "text/plain" to "caption￼".toByteArray())),
                PasteboardItem(mapOf("image/x-custom" to jpeg)),
            ),
        )
        assertEq(listOf("pasted-image-1", "pasted-image-2"), paste.images.map { it.name })
        assertEq("caption", paste.text)
    }

    fun testImagesInsideRichTextAreFoundInDataUris() {
        val encoded = Base64.getEncoder().encodeToString(png)
        val html = "<p>look</p><img src=\"data:image/png;base64,${encoded.chunked(4).joinToString("\n")}\"><img src=\"data:image/png;base64,aGVsbG8=\">"
        val paste = PromptPasteRules.paste(listOf(PasteboardItem(mapOf("text/html" to html.toByteArray(), "text/plain" to "look".toByteArray()))))
        assertEq(1, paste.images.size)
        expect(paste.images.single().data.contentEquals(png))
        assertEq("look", paste.text)
        val embedded = PromptPasteRules.paste(listOf(PasteboardItem(emptyMap(), embeddedImages = listOf(jpeg, "x".toByteArray()))))
        assertEq(1, embedded.images.size)
    }

    fun testAdmitsOnlyWhatFitsAndSaysHowManyWereLeftOut() {
        val images = (1..4).map { PastedImage("pasted-image-$it", png) }
        val all = PromptPasteRules.admit(images, existing = 1)
        assertEq(4, all.accepted.size)
        assertEq(null, all.problem)
        val some = PromptPasteRules.admit(images, existing = 3)
        assertEq(2, some.accepted.size)
        assertEq("A prompt can carry 5 photos, so 2 pasted images were left out.", some.problem)
        assertEq("A prompt can carry 5 photos, so 1 pasted image was left out.", PromptPasteRules.admit(images, existing = 2).problem)
    }
}
