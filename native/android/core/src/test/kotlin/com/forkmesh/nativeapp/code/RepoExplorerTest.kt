package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.TestSettings
import com.forkmesh.nativeapp.model.RepoBranch
import com.forkmesh.nativeapp.model.RepoCodeSearchMatch
import com.forkmesh.nativeapp.model.RepoTree
import com.forkmesh.nativeapp.model.RepoTreeEntry
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.net.URI
import java.time.OffsetDateTime

class RepoExplorerTest {
    private val now = 1_786_636_663_064L
    private val hour = 3_600_000L
    private val day = 24 * hour

    fun testExplorerRowsShowKindSizeAndAge() {
        val dir = RepoTreeEntry(name = "lib", path = "lib", type = "dir", updatedMs = now - 2 * day)
        assertEq("Directory · 2d ago", RepoExplorer.fileRowMeta(dir, now))
        assertEq("2.0 KB", RepoExplorer.fileRowMeta(RepoTreeEntry(name = "README.md", path = "README.md", type = "file", size = 2048), now))
        assertEq("12 B", RepoExplorer.fileRowMeta(RepoTreeEntry(name = "tiny.txt", path = "tiny.txt", type = "file", size = 12), now))
        assertEq("File", RepoExplorer.fileRowMeta(RepoTreeEntry(name = "empty", path = "empty", type = "file"), now))
        val nested = RepoTreeEntry(name = "main.dart", path = "lib/main.dart", type = "file", size = 1536, updatedMs = now - 3 * hour)
        assertEq("1.5 KB · 3h ago", RepoExplorer.fileRowMeta(nested, now))
        assertEq("just now", agoLabel(now - 30_000, now))
    }

    fun testHumanSize() {
        assertEq("1023 B", RepoExplorer.humanSize(1023))
        assertEq("9.9 KB", RepoExplorer.humanSize(10_137))
        assertEq("12 KB", RepoExplorer.humanSize(12_288))
        assertEq("1.5 MB", RepoExplorer.humanSize(1_572_864))
        assertEq("2.0 GB", RepoExplorer.humanSize(2L * 1024 * 1024 * 1024))
    }

    fun testBreadcrumbsTrackTheOpenFolder() {
        assertEq(listOf(ExplorerCrumb("Root", "", true)), RepoExplorer.breadcrumbs(""))
        assertEq(
            listOf(ExplorerCrumb("Root", "", false), ExplorerCrumb("lib", "lib", false), ExplorerCrumb("src", "lib/src", true)),
            RepoExplorer.breadcrumbs("lib/src"),
        )
        assertEq("", RepoExplorer.parentPath("lib"))
        assertEq("lib", RepoExplorer.parentPath("lib/src"))
        assertEq("", RepoExplorer.parentPath(""))
    }

    fun testFileGlyphs() {
        assertEq("code_rounded", RepoExplorer.fileGlyph("main.dart"))
        assertEq("data_object_rounded", RepoExplorer.fileGlyph("pubspec.YAML"))
        assertEq("image_outlined", RepoExplorer.fileGlyph("logo.svg"))
        assertEq("article_outlined", RepoExplorer.fileGlyph("README.md"))
        assertEq("terminal_rounded", RepoExplorer.fileGlyph("Dockerfile"))
        assertEq("description_outlined", RepoExplorer.fileGlyph("notes"))
    }

    fun testBranchSelectorKeepsTheSelectedRefFirst() {
        val branches = listOf(RepoBranch(name = "main", sha = "aaa", isDefault = true), RepoBranch(name = "dev", sha = "bbb"))
        assertEq(listOf("main", "dev"), RepoExplorer.branchMenuNames("main", branches))
        assertEq(listOf("dev", "main"), RepoExplorer.branchMenuNames("dev", branches))
        assertEq(listOf("main", "dev"), RepoExplorer.branchMenuNames("", branches))
        assertEq(emptyList<String>(), RepoExplorer.branchMenuNames("", emptyList()))
        assertEq("Branch: main", RepoExplorer.branchPillLabel("main"))
        assertEq("Branch: Default branch", RepoExplorer.branchPillLabel(""))
    }

    fun testBranchesViewSortsTheDefaultFirstAndMarksWorktrees() {
        val branches = listOf(
            RepoBranch(name = "feature/x", sha = "bbbbbbb1234"),
            RepoBranch(name = "main", sha = "aaaaaaa1234", isDefault = true),
            RepoBranch(name = "agent/wt", sha = "ccccccc1234", worktreePath = "/tmp/wt"),
        )
        val sorted = RepoExplorer.sortBranches(branches, "main")
        assertEq(listOf("main", "agent/wt", "feature/x"), sorted.map { it.name })
        expect(RepoExplorer.isCurrentBranch(sorted[0], "main"))
        expect(RepoExplorer.isCurrentBranch(RepoBranch(name = "trunk"), "trunk"))
        expect(!RepoExplorer.isCurrentBranch(RepoBranch(name = "trunk"), ""))
        assertEq("ccccccc · worktree", RepoExplorer.branchRowSubtitle(branches[2]))
        assertEq("bbbbbbb", RepoExplorer.branchRowSubtitle(branches[0]))
        assertEq("Branch", RepoExplorer.branchRowSubtitle(RepoBranch(name = "x")))
        assertEq(mapOf("hash" to "bbbbbbb1234", "message" to "Tip of feature/x"), RepoExplorer.branchTipCommit(branches[0]))
        assertEq(listOf("agent/wt"), RepoExplorer.worktreeBranches(branches).map { it.name })
        assertEq("abc", RepoExplorer.worktreeShortSha("abc"))
        assertEq("ccccccc", RepoExplorer.worktreeShortSha("ccccccc1234"))
    }

    fun testPreviewKinds() {
        assertEq(PreviewKind.Image, RepoExplorer.previewKind("assets/Logo.PNG"))
        assertEq(PreviewKind.Image, RepoExplorer.previewKind("a.webp"))
        assertEq(PreviewKind.Video, RepoExplorer.previewKind("demo.mov"))
        assertEq(PreviewKind.Text, RepoExplorer.previewKind("logo.svg"))
    }

    fun testDecodedTextRefusesBinary() {
        assertEq(null, RepoExplorer.decodedText("AAAB", "base64"))
        assertEq("hello", RepoExplorer.decodedText("aGVs\nbG8=", "BASE64"))
        assertEq(null, RepoExplorer.decodedText("not base64!", "base64"))
        assertEq(null, RepoExplorer.decodedText("a\u0000b", ""))
        assertEq("void main() {}", RepoExplorer.decodedText("void main() {}", ""))
        val late = "x".repeat(600) + "\u0000"
        assertEq(late, RepoExplorer.decodedText(late, ""))
    }

    fun testSourceLabels() {
        assertEq("served by qt-node-a", RepoExplorer.sourceLabel("qt-node-a"))
        assertEq(null, RepoExplorer.sourceLabel("  "))
        assertEq("live source", RepoExplorer.sourceLabel("LIVE_SOURCE"))
        assertEq("live mirror", RepoExplorer.sourceLabel("live-mirror"))
        assertEq("served by x", RepoExplorer.sourceLabel("served by x"))
        assertEq("live relay", RepoExplorer.sourceLabel("live relay"))
    }

    fun testRawUrlCarriesTheSelectedRef() {
        val api = ApiService(TestSettings(serverUrl = "wss://forkmesh.test/ws"))
        val raw = api.rawUri("owner", "repo", "README.md", ref = "main")
        expect(raw.contains("ref=main"), raw)
        expect(!api.rawUri("owner", "repo", "README.md").contains("ref="))
    }

    fun testCloneUrlIsTheSmartHttpEndpoint() {
        val api = ApiService(TestSettings(serverUrl = "wss://forkmesh.test/ws"))
        assertEq("/owner/repo", URI(api.cloneUri("owner", "repo")).path)
    }

    fun testEditorStatusBarAndLanguage() {
        val lines = RepoExplorer.codeLines("void main() {}\n")
        assertEq(1, lines.size)
        assertEq("Ln 1 · 1 line · 15 B", RepoExplorer.statusBarText(1, lines.size, 15))
        assertEq("Ln 3 · 20 lines · 2.0 KB", RepoExplorer.statusBarText(3, 20, 2048))
        assertEq("Markdown", codeLangForPath("README.md").label)
        assertEq(44f, RepoExplorer.gutterWidth(9))
        assertEq(58f, RepoExplorer.gutterWidth(1000))
        assertEq(66.5f, RepoExplorer.gutterWidth(10_000))
        assertEq(84f, RepoExplorer.gutterWidth(Int.MAX_VALUE))
    }

    fun testGoToFileRows() {
        assertEq(null, RepoExplorer.searchQuery(" m "))
        assertEq("ma", RepoExplorer.searchQuery(" ma "))
        assertEq("1: void main", RepoExplorer.searchRowSubtitle(RepoCodeSearchMatch(path = "lib/main.dart", line = 1, text = "void main")))
        assertEq("hit", RepoExplorer.searchRowSubtitle(RepoCodeSearchMatch(path = "a", text = "hit")))
        assertEq(null, RepoExplorer.searchRowSubtitle(RepoCodeSearchMatch(path = "a", line = 4)))
    }

    fun testTreeEntriesParseLastCommitDateAndSubject() {
        val entry = RepoTreeEntry.fromJson(
            mapOf(
                "name" to ".gitignore",
                "path" to ".gitignore",
                "type" to "blob",
                "size" to 2063L,
                "date" to "2026-08-21T18:09:41-04:00",
                "subject" to "Stop node_modules symlinks reaching commits",
                "message" to "Stop node_modules symlinks reaching commits\n\nLong body",
            ),
        )
        expect(!entry.isDirectory)
        assertEq(OffsetDateTime.parse("2026-08-21T18:09:41-04:00").toInstant().toEpochMilli(), entry.updatedMs)
        assertEq("Stop node_modules symlinks reaching commits", entry.lastMessage)

        val seconds = RepoTreeEntry.fromJson(mapOf("name" to "app", "type" to "tree", "date" to 1755900000L))
        expect(seconds.isDirectory)
        assertEq(1755900000L * 1000, seconds.updatedMs)

        val prefixed = RepoTree.fromJson(
            mapOf(
                "path" to "mobile",
                "entries" to listOf(mapOf("name" to "lib", "path" to "lib", "type" to "tree", "date" to "2026-08-01T00:00:00Z")),
            ),
            path = "mobile",
        )
        assertEq("mobile/lib", prefixed.entries.single().path)
        expect(prefixed.entries.single().updatedMs > 0)
    }
}
