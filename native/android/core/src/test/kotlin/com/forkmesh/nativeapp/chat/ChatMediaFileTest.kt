package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File

class ChatMediaFileTest {
    private val folder = File("/tmp/fm-chat-media-test/chat-media")
    private val base = folder.toPath().toAbsolutePath().normalize()

    private fun attachment(id: String, name: String) = ChatAttachment(id, name, "application/pdf", byteArrayOf(1))

    fun testASenderChosenMessageIdCannotLeaveTheMediaFolder() {
        for (id in listOf("../../Documents/Invoice", "/etc/passwd", "a/../../b", "..", "x\\..\\y")) {
            val file = ChatMediaFile.file(attachment(id, "2026.pdf"), folder) ?: throw AssertionError("no file for $id")
            assertEq(base, file.toPath().parent, id)
            expect(!file.name.contains("/"), id)
            expect(file.name.endsWith("-2026.pdf"), id)
        }
    }

    fun testTheFileNameCannotLeaveTheMediaFolder() {
        for (name in listOf("../../Documents/x", "..", "a/b", "..\\..\\c")) {
            val file = ChatMediaFile.file(attachment("m1", name), folder) ?: throw AssertionError("no file for $name")
            assertEq(base, file.toPath().parent, name)
        }
    }

    fun testTheNameIsStablePerMessageAndDiffersBetweenMessages() {
        val first = ChatMediaFile.name(attachment("message-1", "a.pdf"))
        assertEq(first, ChatMediaFile.name(attachment("message-1", "a.pdf")))
        expect(first != ChatMediaFile.name(attachment("message-2", "a.pdf")))
        assertEq(24 + 1 + "a.pdf".length, first.length)
        assertEq("Aa".hashCode(), "BB".hashCode())
        expect(ChatMediaFile.name(attachment("Aa", "a.pdf")) != ChatMediaFile.name(attachment("BB", "a.pdf")), "ids that share a String hash still get their own file")
    }
}
