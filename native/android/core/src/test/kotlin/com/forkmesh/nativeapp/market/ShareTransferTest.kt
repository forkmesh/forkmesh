package com.forkmesh.nativeapp.market

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertBytes
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.util.Base64

class ShareTransferTest {
    private fun begin(id: String = "abc1", name: String = "face-changer-clip.webm", mime: String = "video/webm;codecs=vp9", size: Any? = 6L, title: String = "My clip") =
        Json.stringify(mapOf("event" to "begin", "id" to id, "name" to name, "mime" to mime, "size" to size, "title" to title))

    private fun chunk(bytes: ByteArray, id: String = "abc1") =
        Json.stringify(mapOf("event" to "chunk", "id" to id, "data" to Base64.getEncoder().encodeToString(bytes)))

    private fun end(id: String = "abc1") = Json.stringify(mapOf("event" to "end", "id" to id))

    fun testAWholeTransferAppendsEveryChunkThenFinishes() {
        val transfer = ShareTransfer()
        assertEq(ShareStep.Begin("abc1", "face-changer-clip.webm", "video/webm", 6, "My clip"), transfer.accept(begin()))
        expect(transfer.active)
        val first = transfer.accept(chunk(byteArrayOf(1, 2, 3, 4)))
        assertBytes(byteArrayOf(1, 2, 3, 4), (first as ShareStep.Append).bytes)
        assertEq(ShareStep.Ignore, transfer.accept(chunk(byteArrayOf(9), id = "other")))
        expect(transfer.accept(chunk(byteArrayOf(5, 6))) is ShareStep.Append)
        assertEq(ShareStep.Ignore, transfer.accept(end("other")))
        assertEq(ShareStep.Finish("face-changer-clip.webm", "video/webm", "My clip"), transfer.accept(end()))
        expect(!transfer.active)
        assertEq(ShareStep.Ignore, transfer.accept(chunk(byteArrayOf(1))))
    }

    fun testRejectsBadTypesSizesAndOverflow() {
        val transfer = ShareTransfer(maxBytes = 10)
        assertEq(ShareStep.Reject("unsupported_type"), transfer.accept(begin(mime = "application/octet-stream")))
        assertEq(ShareStep.Reject("unsupported_type"), transfer.accept(begin(mime = "text/html")))
        assertEq(ShareStep.Reject("too_large"), transfer.accept(begin(size = 11L)))
        assertEq(ShareStep.Reject("invalid_size"), transfer.accept(begin(size = 0L)))
        assertEq(ShareStep.Reject("invalid_size"), transfer.accept(begin(size = "6")))
        assertEq(ShareStep.Reject("invalid_id"), transfer.accept(begin(id = "")))
        assertEq(ShareStep.Reject("invalid"), transfer.accept("not json"))
        expect(!transfer.active)
        expect(transfer.accept(begin(mime = "image/png", size = 3L)) is ShareStep.Begin)
        assertEq(ShareStep.Reject("too_large"), transfer.accept(chunk(byteArrayOf(1, 2, 3, 4))))
        expect(!transfer.active, "an overflowing chunk ends the transfer")
        assertEq(ShareStep.Ignore, transfer.accept(end()))
        expect(transfer.accept(begin(mime = "audio/ogg", size = 3L)) is ShareStep.Begin)
        transfer.accept(chunk(byteArrayOf(1)))
        assertEq(ShareStep.Reject("incomplete"), transfer.accept(end()))
        expect(transfer.accept(begin(size = 3L)) is ShareStep.Begin)
        assertEq(ShareStep.Reject("invalid_data"), transfer.accept(Json.stringify(mapOf("event" to "chunk", "id" to "abc1", "data" to "%%%"))))
        assertEq(ShareStep.Ignore, transfer.accept(Json.stringify(mapOf("event" to "other"))))
        assertEq(512L * 1024 * 1024, ShareTransfer.MAX_BYTES)
    }

    fun testFileNamesAreSafeBasenames() {
        assertEq("clip.mp4", ShareTransfer.fileName("../../clip.mp4"))
        assertEq("evil.png", ShareTransfer.fileName("C:\\x\\evil.png"))
        assertEq("ab.webm", ShareTransfer.fileName("a b.webm"))
        assertEq(ShareTransfer.FALLBACK_NAME, ShareTransfer.fileName(".."))
        assertEq(ShareTransfer.FALLBACK_NAME, ShareTransfer.fileName(""))
        assertEq(ShareTransfer.FALLBACK_NAME, ShareTransfer.fileName("dir/"))
        assertEq("hidden", ShareTransfer.fileName(".hidden"))
        assertEq(80, ShareTransfer.fileName("x".repeat(200) + ".mp4").length)
        assertEq("video/mp4", ShareTransfer.mimeType(" VIDEO/MP4 ; codecs=avc1"))
        assertEq(null, ShareTransfer.mimeType("video/"))
    }

    fun testFaceChangerPageKeepsItsOwnHost() {
        val origin = "https://app.forkmesh.com"
        assertEq("https://app.forkmesh.com/dashboard/face-changer", FaceChangerPage.url("$origin/"))
        expect(FaceChangerPage.isPageHost("https://APP.forkmesh.com/dashboard/face-changer", origin))
        expect(!FaceChangerPage.isPageHost("https://evil.example/dashboard/face-changer", origin))
        expect(!FaceChangerPage.isPageHost(null, origin))
        expect(FaceChangerPage.staysInView("https://app.forkmesh.com/login", origin))
        expect(!FaceChangerPage.staysInView("http://app.forkmesh.com/login", origin))
        expect(!FaceChangerPage.staysInView("https://forkmesh.com/", origin))
        expect(FaceChangerPage.staysInView("http://localhost:8787/x", "http://localhost:8787"))
    }
}
