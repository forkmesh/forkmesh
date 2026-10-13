package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class PromptTextFilesTest {
    fun testReadsUtf8AndNormalisesLineEndings() {
        val file = PromptTextFiles.read(" notes.md ", "﻿one\r\ntwo\r\n".toByteArray(), id = "x")
        assertEq("notes.md", file.name)
        assertEq("one\ntwo\n", file.text)
        assertEq("8 characters", PromptTextFiles.sizeLabel(file))
    }

    fun testRefusesEmptyBinaryNulAndLargeFiles() {
        val empty = assertFails<PromptAttachmentException> { PromptTextFiles.read("a.txt", ByteArray(0)) }
        expect(empty.message.contains("a.txt is empty"), empty.message)
        val jpeg = byteArrayOf(0xFF.toByte(), 0xD8.toByte(), 0xFF.toByte(), 0xE0.toByte())
        val binary = assertFails<PromptAttachmentException> { PromptTextFiles.read("", jpeg) }
        expect(binary.message.startsWith("That file is not a text file."), binary.message)
        assertFails<PromptAttachmentException> { PromptTextFiles.read("u16.txt", byteArrayOf(0x68, 0x00, 0x69, 0x00)) }
        val big = assertFails<PromptAttachmentException> {
            PromptTextFiles.read("big.log", ByteArray(PromptTextFiles.MAX_BYTES + 1) { 0x61 })
        }
        assertEq("big.log is larger than 32 KB, more than a prompt can carry.", big.message)
    }

    fun testFoldWrapsEveryFileAfterThePrompt() {
        val files = listOf(PromptTextFile("1", "a.log", "boom"), PromptTextFile("2", "b.txt", "ok"))
        assertEq("fix it", PromptTextFiles.fold("  fix it ", emptyList()))
        assertEq(
            "fix it\n\n${PromptTextFiles.HEADER}\n\n===== BEGIN FILE a.log =====\nboom\n===== END FILE a.log =====\n\n" +
                "===== BEGIN FILE b.txt =====\nok\n===== END FILE b.txt =====",
            PromptTextFiles.fold(" fix it ", files),
        )
        expect(PromptTextFiles.fold("", files).startsWith(PromptTextFiles.HEADER))
    }

    fun testBudgetCountsTheFoldedPrompt() {
        val file = PromptTextFile("1", "a.txt", "x".repeat(90))
        val used = PromptTextFiles.cost("hi", listOf(file))
        assertEq(PromptTextFiles.fold("hi", listOf(file)).length, used)
        assertEq(null, PromptTextFiles.problem(file, emptyList(), "hi", budget = used))
        expect(PromptTextFiles.problem(file, emptyList(), "hi", budget = used - 1) != null)
        assertEq(null, PromptTextFiles.overBudget("hi", listOf(file), budget = used))
        expect(PromptTextFiles.overBudget("hi!", listOf(file), budget = used) != null)
        assertEq(null, PromptTextFiles.overBudget("y".repeat(9000), emptyList(), budget = 10))
        val full = List(PromptTextFiles.MAX_COUNT) { PromptTextFile("$it", "f", "t") }
        assertEq("A prompt can carry 5 files. Remove one to add another.", PromptTextFiles.problem(file, full, "", budget = 100_000))
        expect(PromptTextFiles.newId() != PromptTextFiles.newId())
    }
}
