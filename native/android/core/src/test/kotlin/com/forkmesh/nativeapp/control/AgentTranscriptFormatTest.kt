package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File

class AgentTranscriptFormatTest {
    fun testDesktopLogBecomesStatusPromptToolAndText() {
        val log = listOf(
            "==> Agent: Claude Code",
            "==> Creating worktree /w/agent-7",
            "",
            "> Fix the login bug",
            "please",
            "",
            "I'll look at **auth** first.",
            "",
            "[Read src/auth.ts]",
            "",
            "[Bash npm test]",
            "!! npm exited 1",
            "==> User steering prompt",
            "try again",
            "==> Stop requested by user.",
        ).joinToString("\n")
        assertEq(
            listOf(
                TranscriptBlock.Status(listOf("Agent: Claude Code", "Creating worktree /w/agent-7")),
                TranscriptBlock.Prompt("Fix the login bug\nplease"),
                TranscriptBlock.Text("I'll look at **auth** first."),
                TranscriptBlock.Tools(listOf(TranscriptTool("Read", "src/auth.ts"), TranscriptTool("Bash", "npm test"))),
                TranscriptBlock.Error("npm exited 1"),
                TranscriptBlock.Prompt("try again"),
                TranscriptBlock.Status(listOf("Stop requested by user.")),
            ),
            AgentTranscriptFormat.blocks(log),
        )
    }

    fun testMarkdownHeadingsListsCodeAndTables() {
        val text = listOf(
            "## Summary",
            "- one",
            "  - nested",
            "2. second",
            "",
            "```swift",
            "let x = 1",
            "",
            "print(x)",
            "```",
            "| a | b |",
            "|---|---|",
            "---",
            "[x]",
        ).joinToString("\n")
        assertEq(
            listOf(
                TranscriptBlock.Heading(2, "Summary"),
                TranscriptBlock.Bullet("•", 0, "one"),
                TranscriptBlock.Bullet("•", 1, "nested"),
                TranscriptBlock.Bullet("2.", 0, "second"),
                TranscriptBlock.Code("swift", "let x = 1\n\nprint(x)"),
                TranscriptBlock.Table(listOf("a", "b"), emptyList(), listOf(TranscriptAlign.Start, TranscriptAlign.Start)),
                TranscriptBlock.Rule,
                TranscriptBlock.Text("[x]"),
            ),
            AgentTranscriptFormat.blocks(text),
        )
    }

    fun testTablesBecomeGridsWithAlignmentAndEscapes() {
        val text = listOf(
            "| Item | Count | Note |",
            "| :--- | ---: | :-: |",
            "| `a|b` | 3 | x \\| y |",
            "| short |",
            "",
            "| not | a table |",
            "| no separator |",
        ).joinToString("\n")
        assertEq(
            listOf(
                TranscriptBlock.Table(
                    listOf("Item", "Count", "Note"),
                    listOf(listOf("`a|b`", "3", "x | y"), listOf("short", "", "")),
                    listOf(TranscriptAlign.Start, TranscriptAlign.End, TranscriptAlign.Center),
                ),
                TranscriptBlock.Code("", "| not | a table |\n| no separator |"),
            ),
            AgentTranscriptFormat.blocks(text),
        )
    }

    fun testTaskNotificationsBecomeCardsWithTheirResult() {
        val log = listOf(
            "==> Task completed: Agent \"Fix runner\" finished",
            "    Done.",
            "",
            "    | a | b |",
            "    |---|---|",
            "    | 1 | 2 |",
            "",
            "==> Task failed: Background command \"make\" failed with exit code 2",
            "==> Task update: Monitor event: \"build\"",
            "    EXIT 0",
            "Back to the reply.",
            "==> Task cost: \$0.12",
        ).joinToString("\n")
        val blocks = AgentTranscriptFormat.blocks(log)
        assertEq(
            listOf(
                TranscriptBlock.Task("completed", "Agent \"Fix runner\" finished", "Done.\n\n| a | b |\n|---|---|\n| 1 | 2 |"),
                TranscriptBlock.Task("failed", "Background command \"make\" failed with exit code 2", ""),
                TranscriptBlock.Task("update", "Monitor event: \"build\"", "EXIT 0"),
                TranscriptBlock.Text("Back to the reply."),
                TranscriptBlock.Status(listOf("Task cost: \$0.12")),
            ),
            blocks,
        )
        assertEq(
            listOf(TranscriptTaskTone.Done, TranscriptTaskTone.Failed, TranscriptTaskTone.Running),
            blocks.take(3).map { (it as TranscriptBlock.Task).tone },
        )
        assertEq(
            listOf(
                TranscriptBlock.Text("Done."),
                TranscriptBlock.Table(listOf("a", "b"), listOf(listOf("1", "2")), listOf(TranscriptAlign.Start, TranscriptAlign.Start)),
            ),
            AgentTranscriptFormat.blocks((blocks[0] as TranscriptBlock.Task).body),
        )
    }

    fun testSteersAndPeerMessagesAreNamedBubbles() {
        val log = listOf(
            "> ⟵ ForkMesh (phone): Also run the tests",
            "",
            "> ⟵ a-13: ping from a",
            "",
            "> ⟵ no colon here",
        ).joinToString("\n")
        val blocks = AgentTranscriptFormat.blocks(log)
        assertEq(
            listOf(
                TranscriptBlock.Steer("ForkMesh (phone)", "Also run the tests"),
                TranscriptBlock.Steer("a-13", "ping from a"),
                TranscriptBlock.Prompt("⟵ no colon here"),
            ),
            blocks,
        )
        expect((blocks[0] as TranscriptBlock.Steer).fromYou, "a ForkMesh steer is the user's own")
        expect(!(blocks[1] as TranscriptBlock.Steer).fromYou, "another session is not the user")
    }

    fun testToolRunsGroupUntilSomethingElseArrives() {
        val log = listOf("[Read a]", "", "[Grep b]", "", "[Bash make]", "", "Done.", "", "[Edit c]").joinToString("\n")
        assertEq(
            listOf(
                TranscriptBlock.Tools(listOf(TranscriptTool("Read", "a"), TranscriptTool("Grep", "b"), TranscriptTool("Bash", "make"))),
                TranscriptBlock.Text("Done."),
                TranscriptBlock.Tools(listOf(TranscriptTool("Edit", "c"))),
            ),
            AgentTranscriptFormat.blocks(log),
        )
    }

    fun testMultiLineToolCallsFoldIntoOneChip() {
        val log = listOf(
            "[Bash timeout 60 python3 -u - <<'EOF'",
            "import next_common as c",
            "",
            "cmd=[\"ssh\",\"-v\"]",
            "p=c.run(cmd)…]",
            "",
            "[Bash cd tools && python3 - <<'EOF'",
            "x = [1, 2]",
            "print(x)",
            "EOF]",
            "",
            "[Read a.py]",
            "",
            "Done.",
        ).joinToString("\n")
        assertEq(
            listOf(
                TranscriptBlock.Tools(
                    listOf(
                        TranscriptTool("Bash", "timeout 60 python3 -u - <<'EOF'\nimport next_common as c\n\ncmd=[\"ssh\",\"-v\"]\np=c.run(cmd)…"),
                        TranscriptTool("Bash", "cd tools && python3 - <<'EOF'\nx = [1, 2]\nprint(x)\nEOF"),
                        TranscriptTool("Read", "a.py"),
                    ),
                ),
                TranscriptBlock.Text("Done."),
            ),
            AgentTranscriptFormat.blocks(log),
        )
    }

    fun testAnOpenBracketThatIsNotAToolStaysText() {
        assertEq(
            listOf(TranscriptBlock.Text("[see the docs](https://x.dev) for more\nand here"), TranscriptBlock.Text("Next.")),
            AgentTranscriptFormat.blocks("[see the docs](https://x.dev) for more\nand here\n\nNext."),
        )
        val long = "[Note " + "word ".repeat(80)
        assertEq(
            listOf(TranscriptBlock.Text(long + "\nmore"), TranscriptBlock.Text("Next.")),
            AgentTranscriptFormat.blocks(long + "\nmore\n\nNext."),
        )
        assertEq(listOf(TranscriptBlock.Text("[Bash make\nall")), AgentTranscriptFormat.blocks("[Bash make\nall"))
    }

    fun testTheCacheFoldsAMultiLineToolOnceItCloses() {
        val cache = TranscriptBlockCache()
        val text = "[Bash make \\\n  all]\n\nOk.\n"
        for (end in 1..text.length) {
            val part = text.substring(0, end)
            assertEq(AgentTranscriptFormat.blocks(part), cache.update(part), "at $end")
        }
        assertEq(
            listOf(TranscriptBlock.Tools(listOf(TranscriptTool("Bash", "make \\\n  all"))), TranscriptBlock.Text("Ok.")),
            cache.update(text),
        )
    }

    fun testJsonIsPrettyPrinted() {
        val log = listOf(
            "```json",
            "{\"a\":1,\"b\":[true,null,\"x,y\"],\"c\":{}}",
            "```",
            "",
            "```",
            "[1, 2.5e3, -0.1]",
            "```",
            "",
            "{\"k\": \"v\"}",
            "",
            "```json",
            "{not json}",
            "```",
        ).joinToString("\n")
        assertEq(
            listOf(
                TranscriptBlock.Json("{\n  \"a\": 1,\n  \"b\": [\n    true,\n    null,\n    \"x,y\"\n  ],\n  \"c\": {}\n}"),
                TranscriptBlock.Json("[\n  1,\n  2.5e3,\n  -0.1\n]"),
                TranscriptBlock.Json("{\n  \"k\": \"v\"\n}"),
                TranscriptBlock.Code("json", "{not json}"),
            ),
            AgentTranscriptFormat.blocks(log),
        )
        assertEq(null, TranscriptJson.pretty("{\"a\":01}"))
        assertEq(null, TranscriptJson.pretty("{\"a\":1} x"))
        assertEq("{\n  \"q\": \"say \\\"hi\\\"\"\n}", TranscriptJson.pretty("{\"q\":\"say \\\"hi\\\"\"}"))
    }

    fun testAnUnclosedFenceStillShowsItsCode() {
        assertEq(listOf(TranscriptBlock.Code("", "streaming")), AgentTranscriptFormat.blocks("```\nstreaming"))
    }

    fun testInlineBoldCodeAndLinks() {
        assertEq(
            listOf(
                TranscriptInline("Run "),
                TranscriptInline("make", TranscriptInlineStyle.Code),
                TranscriptInline(" then "),
                TranscriptInline("ship", TranscriptInlineStyle.Bold),
                TranscriptInline(" it, see "),
                TranscriptInline("docs", TranscriptInlineStyle.Link, "https://forkmesh.com/docs"),
                TranscriptInline(" or [x](file)."),
            ),
            AgentTranscriptFormat.inline("Run `make` then **ship** it, see [docs](https://forkmesh.com/docs) or [x](file)."),
        )
        assertEq(listOf(TranscriptInline("a ** b `")), AgentTranscriptFormat.inline("a ** b `"))
    }

    fun testInlineItalicAndBareLinks() {
        assertEq(
            listOf(
                TranscriptInline("merged but "),
                TranscriptInline("not pushed", TranscriptInlineStyle.Italic),
                TranscriptInline(", "),
                TranscriptInline("also", TranscriptInlineStyle.Italic),
                TranscriptInline(" "),
                TranscriptInline("bold", TranscriptInlineStyle.Bold),
                TranscriptInline("; see "),
                TranscriptInline("https://forkmesh.com/a", TranscriptInlineStyle.Link, "https://forkmesh.com/a"),
                TranscriptInline("."),
            ),
            AgentTranscriptFormat.inline("merged but *not pushed*, _also_ __bold__; see https://forkmesh.com/a."),
        )
        assertEq(listOf(TranscriptInline("snake_case_name and 2 * 3 * 4")), AgentTranscriptFormat.inline("snake_case_name and 2 * 3 * 4"))
    }

    fun testTheCacheMatchesAFullParseAsTextArrives() {
        val full = fixture("transcript.txt")
        val cache = TranscriptBlockCache()
        var at = 0
        var step = 1
        while (at < full.length) {
            at = minOf(full.length, at + step)
            step = step % 97 + 13
            val text = full.substring(0, at)
            assertEq(AgentTranscriptFormat.blocks(text), cache.update(text), "at $at")
        }
        assertEq(AgentTranscriptFormat.blocks("reset\n"), cache.update("reset\n"), "a reset starts over")
    }

    fun testTheSharedTranscriptParsesToTheSharedBlocks() {
        val blocks = AgentTranscriptFormat.blocks(fixture("transcript.txt"))
        assertEq(fixture("blocks.txt").trimEnd('\n'), blocks.joinToString("\n") { describe(it) })
    }

    private fun describe(block: TranscriptBlock): String = when (block) {
        is TranscriptBlock.Status -> "status " + block.lines.joinToString(" | ")
        is TranscriptBlock.Error -> "error " + block.text
        is TranscriptBlock.Prompt -> "prompt " + block.text.lineSequence().first()
        is TranscriptBlock.Steer -> "steer " + block.from + (if (block.fromYou) " (you) " else " ") + block.text
        is TranscriptBlock.Tools -> "tools " + block.calls.joinToString(",") { it.name }
        is TranscriptBlock.Heading -> "heading${block.level} " + block.text
        is TranscriptBlock.Bullet -> "bullet ${block.marker} ${block.depth} " + block.text
        is TranscriptBlock.Code -> "code ${block.language} ${block.text.lines().size}"
        is TranscriptBlock.Json -> "json ${block.text.lines().size}"
        is TranscriptBlock.Table -> "table ${block.header.size}x${block.rows.size} " + block.header.joinToString("|") + " " +
            block.align.joinToString("") { it.name.first().lowercase() }
        is TranscriptBlock.Task -> "task ${block.status} ${block.tone.name.lowercase()} " + block.summary + " [" +
            AgentTranscriptFormat.blocks(block.body).joinToString(", ") { describe(it).substringBefore(' ') } + "]"
        TranscriptBlock.Rule -> "rule"
        is TranscriptBlock.Text -> "text " + block.text.lineSequence().first()
    }

    private fun fixture(name: String): String {
        var dir: File? = File(AgentTranscriptFormatTest::class.java.protectionDomain.codeSource.location.toURI()).absoluteFile
        while (dir != null && !File(dir, "conformance/agent-transcript/v1").isDirectory) dir = dir.parentFile
        val root = dir ?: error("conformance/agent-transcript/v1 is not reachable from the test classes")
        return File(root, "conformance/agent-transcript/v1/$name").readText()
    }
}
