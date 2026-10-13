package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.collab.RepoArchiveBuilder
import com.forkmesh.nativeapp.collab.RepoArchiveReader
import com.forkmesh.nativeapp.collab.ZipStoreWriter
import com.forkmesh.nativeapp.model.RepoTree
import com.forkmesh.nativeapp.model.RepoTreeEntry
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.ByteArrayInputStream
import java.util.zip.ZipInputStream

class CodeToolsTest {
    fun testCommitGraphLanes() {
        val rows = CommitGraph.layout(
            listOf(
                "m" to listOf("a", "b"),
                "b" to listOf("c"),
                "a" to listOf("c"),
                "c" to emptyList(),
            ),
        )
        assertEq(listOf(0, 1, 0, 0), rows.map { it.column })
        expect(rows[0].isMerge)
        assertEq(listOf(0, 1), rows[0].lanesOut)
        assertEq(listOf(CommitGraphEdge(0, 0)), rows[2].edges)
        assertEq(listOf(CommitGraphEdge(1, 0)), rows[3].edges)
        assertEq(emptyList<Int>(), rows[3].lanesOut)
        assertEq(2, CommitGraph.width(rows))
        assertEq(listOf("aa", "bb"), CommitGraph.parents(mapOf("parents" to listOf(mapOf("sha" to "AA"), "bb"))))
        assertEq(listOf("x", "y"), CommitGraph.parents(mapOf("parents" to "X Y")))
    }

    fun testCodeMapSquarifyAndCells() {
        val rects = CodeMapLayout.squarify(listOf(6.0, 6.0, 4.0, 3.0, 2.0, 2.0, 1.0), CodeMapRect(0.0, 0.0, 6.0, 4.0))
        assertEq(24.0, rects.sumOf { it.area }, "areas fill the rectangle")
        rects.forEach { expect(it.x >= -1e-9 && it.x + it.width <= 6.0 + 1e-9 && it.y + it.height <= 4.0 + 1e-9) }
        val root = CodeMapNode.fromJson(
            mapOf(
                "name" to "",
                "children" to listOf(
                    mapOf("name" to "src", "children" to listOf(mapOf("name" to "a.kt", "type" to "file", "size" to 300L))),
                    mapOf("name" to "README", "type" to "file", "size" to 100L),
                ),
            ),
        )
        assertEq(400L, root.size)
        assertEq("src/a.kt", root.find("src/a.kt")?.path)
        assertEq(2, root.fileCount)
        val cells = CodeMapLayout.cells(root, CodeMapRect(0.0, 0.0, 400.0, 300.0))
        assertEq(listOf("src", "src/a.kt", "README"), cells.map { it.node.path })
        assertEq("src/a.kt", CodeMapLayout.hit(cells, 10.0, 200.0)?.node?.path)
        assertEq(listOf("", "a", "a/b"), CodeMapLayout.breadcrumb("a/b").map { it.path })
        assertEq("1.5 KB", CodeMapLayout.sizeLabel(1536))
        assertEq("12 MB", CodeMapLayout.sizeLabel(12L * 1024 * 1024))
    }

    fun testConflictParseAndResolve() {
        val text = "top\n<<<<<<< HEAD\nours\n||||||| base\norig\n=======\ntheirs\n>>>>>>> feat\nbottom\n"
        val file = ConflictResolution.parse("f", text).getOrThrow()
        val hunk = file.hunks.single()
        assertEq(listOf("ours"), hunk.ours)
        assertEq(listOf("orig"), hunk.base)
        assertEq("feat", hunk.theirsLabel)
        assertEq(2, hunk.startLine)
        assertEq(null, ConflictResolution.resolve(file, emptyMap()))
        assertEq("top\ntheirs\nours\nbottom\n", ConflictResolution.resolve(file, mapOf(0 to ConflictChoice.BothTheirsFirst)))
        expect(ConflictResolution.parse("f", "<<<<<<< a\nx\n").isFailure)
        expect(ConflictResolution.parse("f", "<<<<<<< a\n<<<<<<< b\n").isFailure)
    }

    fun testConflictFixPromptNamesTheBase() {
        assertEq(
            "Merge `main` into your branch and resolve all merge conflicts. Make sure the build and tests still pass, then commit.",
            ConflictResolution.sessionPrompt("main"),
        )
        val listed = ConflictResolution.sessionPrompt("", listOf("a.kt", "b.kt"))
        expect(listed.startsWith("Merge the base branch into your branch"))
        expect(listed.endsWith("Conflicting files:\n- a.kt\n- b.kt"))
    }

    fun testFindReplaceGoToLineAndOutline() {
        val finder = FindInText()
        finder.search("Foo", "foo bar\nfoobar Foo")
        assertEq(listOf(1 to 1, 2 to 1, 2 to 8), finder.matches.map { it.line to it.column })
        finder.options = FindOptions(caseSensitive = false, wholeWord = true)
        finder.search("foo", "foo bar\nfoobar Foo")
        assertEq(2, finder.matches.size)
        finder.previous()
        assertEq("2 of 2", finder.status)
        finder.jumpToLine(1)
        assertEq(0, finder.current)
        assertEq("a-c-" to 2, FindInText.replacing("abcb", "b", "-"))
        assertEq(10, GoToLine.parse(" 12:4", 10))
        assertEq(null, GoToLine.parse("0", 10))
        val source = "class Widget {\n    func draw() {\n        if (x) {\n    }\n}\ndef helper():\n  pass\nconst go = () => 1"
        val symbols = CodeOutline.symbols(source)
        assertEq(listOf("Widget", "draw", "helper", "go"), symbols.map { it.name })
        assertEq(1, symbols[1].depth)
        val refs = CodeOutline.references("draw", "draw()\nredraw()\nx.draw ()\n", excludingLine = 1)
        assertEq(listOf(3), refs.map { it.line })
        expect(refs.single().isCall)
    }

    fun testDesktopCommandProofRefusesUnsupportedCommands() {
        val failure = try {
            OwnerProofs.desktopCommand(null, null, "alice", "tool", "pull.nuke", "pull:1")
            null
        } catch (error: Exception) {
            error.message
        }
        assertEq(com.forkmesh.nativeapp.control.DesktopCommandContract.UNSUPPORTED_MESSAGE, failure)
        assertEq(
            "forkmesh-desktop-command-v1\nalice\ntool\naction.rerun\nrun:4\n9",
            OwnerProofs.desktopCommandCanonical("alice", "tool", "action.rerun", "run:4", "9"),
        )
    }

    fun testZipWriterAndArchiveBuilder() {
        val writer = ZipStoreWriter()
        expect(writer.add("dir/a.txt", "hello".toByteArray()))
        expect(!writer.add("../evil", ByteArray(1)))
        expect(!writer.add("a//b", ByteArray(1)))
        val zip = ZipInputStream(ByteArrayInputStream(writer.finish()))
        val entry = zip.nextEntry!!
        assertEq("dir/a.txt", entry.name)
        assertEq("hello", String(zip.readBytes()))
        assertEq(null, zip.nextEntry)

        val reader = object : RepoArchiveReader {
            override fun tree(owner: String, name: String, path: String, ref: String): RepoTree = when (path) {
                "" -> RepoTree("", listOf(RepoTreeEntry("src", "src", "dir"), RepoTreeEntry("big", "big", "file", size = 9L * 1024 * 1024)))
                else -> RepoTree(path, listOf(RepoTreeEntry("m.kt", "src/m.kt", "file", size = 3)))
            }
            override fun rawBytes(owner: String, name: String, path: String, ref: String, maxBytes: Int) = "abc".toByteArray()
        }
        val result = RepoArchiveBuilder(reader).build("alice", "tool", "feat/x")
        assertEq("tool-feat-x.zip", result.fileName)
        assertEq(1, result.files)
        assertEq(listOf("big"), result.skipped)
        assertEq("tool-feat-x/src/m.kt", ZipInputStream(ByteArrayInputStream(result.data)).nextEntry!!.name)
    }
}
