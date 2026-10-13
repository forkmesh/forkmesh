package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.agents.PromptAttachment
import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.crypto.Sha256
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.ByteArrayOutputStream

private class FakeImageLink(override val transport: ControlTransport) : ControlLink {
    val calls = ArrayList<Pair<String, Map<String, Any?>>>()
    val staged = LinkedHashMap<String, ByteArrayOutputStream>()
    var refuse: String? = null

    override val isOpen: Boolean get() = true
    override val desktopName: String get() = "desk"

    override fun request(method: String, params: Map<String, Any?>, timeoutMs: Long): Map<String, Any?> {
        calls += method to params
        refuse?.let { if (method == "agents.image.begin") throw LocalControlException(it) }
        return when (method) {
            "agents.image.begin" -> {
                val id = "%032x".format(staged.size + 1)
                staged[id] = ByteArrayOutputStream()
                mapOf("image" to id)
            }
            "agents.image.chunk" -> {
                val data = B64.decode(params["data"] as String)
                expect(data.size <= DesktopPromptImages.CHUNK_BYTES, "a chunk fits the limit")
                val out = staged.getValue(params["image"] as String)
                out.write(data)
                mapOf("received" to out.size())
            }
            else -> emptyMap()
        }
    }

    override fun addEventTap(tap: (ControlEvent) -> Unit): Any = Any()

    override fun removeEventTap(token: Any) {}
}

class DesktopPromptImagesTest {
    private val big = byteArrayOf(0xFF.toByte(), 0xD8.toByte(), 0xFF.toByte()) + ByteArray(DesktopPromptImages.CHUNK_BYTES + 10) { 7 }
    private val small = byteArrayOf(0x89.toByte(), 0x50, 0x4E, 0x47, 1, 2)
    private val photos = listOf(
        PromptAttachment("a", "a.jpg", big, "image/jpeg"),
        PromptAttachment("b", "b.png", small, "image/png"),
    )

    fun testPhotosUploadInChunksAndArePacedThroughTheRelay() {
        val link = FakeImageLink(ControlTransport.Relay)
        val pauses = ArrayList<Long>()
        val ids = DesktopPromptImages.upload(photos, link) { pauses += it }
        assertEq(listOf("%032x".format(1), "%032x".format(2)), ids)
        expect(link.staged.getValue(ids[0]).toByteArray().contentEquals(big), "the big photo arrives whole")
        expect(link.staged.getValue(ids[1]).toByteArray().contentEquals(small), "the small photo arrives whole")
        val begin = link.calls.first().second
        assertEq("image/jpeg", begin["mediaType"])
        assertEq(big.size, begin["size"])
        assertEq(Sha256.hex(big), begin["sha256"])
        assertEq(3, pauses.size)
        expect(pauses.first() > 2000, "a full chunk waits out the relay's budget")
    }

    fun testOnTheLanNothingWaits() {
        val link = FakeImageLink(ControlTransport.Lan)
        val pauses = ArrayList<Long>()
        DesktopPromptImages.upload(photos, link) { pauses += it }
        assertEq(0, pauses.size)
    }

    fun testParamsNameTheUploadedImages() {
        val link = FakeImageLink(ControlTransport.Lan)
        val params = DesktopPromptImages.params(mapOf("id" to 7, "text" to "look"), photos.take(1), link)
        assertEq(listOf("%032x".format(1)), params["images"])
        assertEq(7, params["id"])
        val before = link.calls.size
        val none = DesktopPromptImages.params(mapOf("id" to 7), emptyList(), link)
        expect("images" !in none, "no photos, no list")
        assertEq(before, link.calls.size)
    }

    fun testAnOlderDesktopIsNamed() {
        val link = FakeImageLink(ControlTransport.Lan)
        link.refuse = "unsupported"
        val error = runCatching { DesktopPromptImages.upload(photos, link) }.exceptionOrNull()
        assertEq("images_unsupported", (error as? LocalControlException)?.code)
    }

    fun testAFailedUploadCancelsWhatWasStaged() {
        val link = object : ControlLink {
            val calls = ArrayList<String>()
            override val transport = ControlTransport.Lan
            override val isOpen = true
            override val desktopName = "desk"

            override fun request(method: String, params: Map<String, Any?>, timeoutMs: Long): Map<String, Any?> {
                calls += method
                if (method == "agents.image.chunk") throw LocalControlException("bad_chunk")
                return mapOf("image" to "c".repeat(32))
            }

            override fun addEventTap(tap: (ControlEvent) -> Unit): Any = Any()

            override fun removeEventTap(token: Any) {}
        }
        val error = runCatching { DesktopPromptImages.upload(photos, link) }.exceptionOrNull()
        assertEq("bad_chunk", (error as? LocalControlException)?.code)
        assertEq(listOf("agents.image.begin", "agents.image.chunk", "agents.image.cancel"), link.calls)
    }
}
