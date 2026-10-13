package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.nio.file.Files
import java.time.Instant

class LocalRepoStoreTest {
    private val author = GitAuthor(name = "Alice", email = "alice@example.com")
    private val fixedNow = Instant.parse("2026-08-23T12:00:00Z").toEpochMilli()

    private inline fun withStore(block: (temp: File, store: LocalRepoStore) -> Unit) = withScratch("fm-repo-store-") { temp ->
        block(temp, LocalRepoStore(File(temp, "docs/forkmesh/repos"), now = { fixedNow }))
    }

    fun testValidatesRepositoryNames() {
        expect(LocalRepoStore.isValidName("my-repo_1.0"))
        expect(!LocalRepoStore.isValidName(""))
        expect(!LocalRepoStore.isValidName("."))
        expect(!LocalRepoStore.isValidName(".."))
        expect(!LocalRepoStore.isValidName("a/b"))
        expect(!LocalRepoStore.isValidName("a b"))
        expect(!LocalRepoStore.isValidName("ü"))
        expect(!LocalRepoStore.isValidName("name\n"))
        expect(LocalRepoStore.isValidName("x".repeat(100)))
        expect(!LocalRepoStore.isValidName("x".repeat(101)))
        assertEq("Enter a name.", LocalRepoStore.validateName(""))
        assertEq("Keep the name under 100 characters.", LocalRepoStore.validateName("x".repeat(101)))
        assertEq("Use letters, digits, dots, dashes, or underscores only.", LocalRepoStore.validateName("bad name!"))
        assertEq(null, LocalRepoStore.validateName("ok"))
    }

    fun testCreatesListsRefreshesAndDeletesRepositories() = withStore { _, store ->
        expect(store.list().isEmpty())
        expect(store.root.isDirectory)

        var notifications = 0
        store.addListener { notifications += 1 }

        val record = store.create(name = "notes", description = "My notes", author = author)
        assertEq("notes", record.name)
        assertEq("${store.root.path}${File.separator}notes", record.path)
        assertEq("main", record.branch)
        assertEq("Initial commit", record.lastCommitSubject)
        expect(record.lastCommitAt != null)
        assertEq(fixedNow, record.createdAt)
        expect(!record.isPublished)
        expect(notifications > 0)

        assertEq("# notes\n\nMy notes\n", File(record.path, "README.md").readText())

        val json = Json.parse(File(store.root, "index.json").readText()) as Map<*, *>
        assertEq(1L, json["version"])
        val repos = json["repos"] as List<*>
        assertEq(1, repos.size)
        assertEq("notes", (repos.single() as Map<*, *>)["name"])

        val reloaded = LocalRepoStore(store.root)
        assertEq("notes", reloaded.list().single().name)
        assertEq("My notes", reloaded.find("notes")?.description)
        assertEq(null, reloaded.find("missing"))

        val workspace = store.workspace(record)
        workspace.writeFile("a.txt", "a")
        workspace.stageAll()
        workspace.commit(message = "Add a", author = author)
        workspace.dispose()
        assertEq("Add a", store.refresh("notes")?.lastCommitSubject)

        store.markPublished("notes", "alice/notes")
        assertEq("alice/notes", store.find("notes")?.publishedAs)
        assertEq("Published", store.find("notes")?.syncLabel)

        store.delete("notes")
        expect(store.list().isEmpty())
        expect(!File(record.path).exists())
    }

    fun testRejectsDuplicatesBadNamesAndSymlinkedPaths() = withStore { temp, store ->
        store.create(name = "dup", author = author)
        assertEq(
            "A local repository with that name already exists.",
            assertFails<LocalRepoStoreException> { store.create(name = "dup", author = author) }.message,
        )
        assertFails<LocalRepoStoreException> { store.create(name = "../escape", author = author) }
        assertFails<LocalRepoStoreException> { store.create(name = "..", author = author) }
        val outside = File(temp, "outside").apply { mkdirs() }
        File(outside, "keep.txt").writeText("keep")
        Files.createSymbolicLink(File(store.root, "linked").toPath(), outside.toPath())
        assertFails<LocalRepoStoreException> { store.create(name = "linked", author = author) }
        assertEq(
            "Repository path is a symlink and was refused.",
            assertFails<LocalRepoStoreException> { store.delete("linked") }.message,
        )
        expect(File(outside, "keep.txt").isFile)
        File(store.root, "stray").mkdirs()
        assertEq(
            "A folder with that name already exists on this device.",
            assertFails<LocalRepoStoreException> { store.create(name = "stray", author = author) }.message,
        )
    }

    fun testRefusesASymlinkedStoreRoot() = withScratch("fm-repo-store-") { temp ->
        val real = File(temp, "real").apply { mkdirs() }
        val link = File(temp, "link")
        Files.createSymbolicLink(link.toPath(), real.toPath())
        val linked = LocalRepoStore(link)
        assertEq(
            "Local repository root is a symlink and was refused.",
            assertFails<LocalRepoStoreException> { linked.list() }.message,
        )
    }

    fun testAFailedCreateLeavesNoFolderOrIndexEntry() = withStore { _, store ->
        val bad = GitAuthor(name = "bad\nname", email = "x@y")
        assertFails<GitWorkspaceException> { store.create(name = "broken", author = bad) }
        expect(store.list().isEmpty())
        expect(!File(store.root, "broken").exists())
        expect(!store.isBusy)
    }

    fun testDropsIndexEntriesWhoseWorktreeVanished() = withStore { temp, store ->
        val record = store.create(name = "gone", author = author)
        LocalScratch.delete(File(record.path))
        expect(File(temp, "docs/forkmesh/repos").isDirectory)
        val reloaded = LocalRepoStore(store.root)
        expect(reloaded.list().isEmpty())
        val json = Json.parse(File(store.root, "index.json").readText()) as Map<*, *>
        expect((json["repos"] as List<*>).isEmpty())
    }

    fun testImportsADirectoryIntoANewRepository() = withStore { temp, store ->
        val source = File(temp, "src").apply { mkdirs() }
        File(source, "hello.txt").writeText("hi")
        val record = store.importDirectory(name = "imported", source = source, author = author)
        assertEq("Import from Files", record.lastCommitSubject)
        val workspace = store.workspace(record)
        try {
            assertEq("hi", workspace.readFile("hello.txt").text)
        } finally {
            workspace.dispose()
        }
    }

    fun testAFailedImportLeavesNothingBehind() = withStore { temp, store ->
        val empty = File(temp, "empty").apply { mkdirs() }
        assertEq(
            "Nothing importable was found.",
            assertFails<GitWorkspaceException> { store.importDirectory(name = "empty", source = empty, author = author) }.message,
        )
        expect(store.list().isEmpty())
        expect(!File(store.root, "empty").exists())
    }

    fun testIndexIsWrittenLikeDartsJsonEncoderWithIndent() = withStore { _, store ->
        val record = store.create(name = "notes", author = author)
        val text = File(store.root, "index.json").readText()
        val commitAt = DartDateTime.toIso8601Utc(record.lastCommitAt!!)
        assertEq(
            "{\n" +
                "  \"version\": 1,\n" +
                "  \"repos\": [\n" +
                "    {\n" +
                "      \"name\": \"notes\",\n" +
                "      \"path\": ${Json.stringify(record.path)},\n" +
                "      \"description\": \"\",\n" +
                "      \"createdAt\": \"2026-08-23T12:00:00.000Z\",\n" +
                "      \"branch\": \"main\",\n" +
                "      \"lastCommit\": \"Initial commit\",\n" +
                "      \"lastCommitAt\": \"$commitAt\",\n" +
                "      \"publishedAs\": null,\n" +
                "      \"origin\": \"authored\"\n" +
                "    }\n" +
                "  ]\n" +
                "}",
            text,
        )
        store.delete("notes")
        assertEq("{\n  \"version\": 1,\n  \"repos\": []\n}", File(store.root, "index.json").readText())
    }

    fun testNewRepositoriesStartOnTheChosenBranch() = withStore { _, store ->
        val record = store.create(name = "trunked", author = author, branch = "trunk")
        assertEq("trunk", record.branch)
        assertEq("ref: refs/heads/trunk\n", File(record.path, ".git/HEAD").readText())
        expect(File(record.path, ".git/refs/heads/trunk").isFile)
    }

    fun testRebasesMovedWorktreesAndAdoptsUnlistedOnes() = withStore { _, store ->
        store.root.mkdirs()
        File(store.root, "moved/.git").mkdirs()
        File(store.root, "orphan/.git").mkdirs()
        File(store.root, "plain-folder").mkdirs()
        File(store.root, "index.json").writeText(
            """
            {"version": 1, "repos": [
              {"name": "moved", "path": "/data/user/0/com.forkmesh.forkmesh/app_flutter/forkmesh/repos/moved",
               "createdAt": "2026-08-20T10:00:00.000Z", "publishedAs": "alice/moved", "origin": "cloned"}
            ]}
            """.trimIndent(),
        )
        val records = LocalRepoStore(store.root).list()
        assertEq(listOf("moved", "orphan"), records.map { it.name }.sorted())
        val moved = records.first { it.name == "moved" }
        assertEq(File(store.root, "moved").path, moved.path)
        assertEq("alice/moved", moved.publishedAs)
        assertEq(File(store.root, "orphan").path, records.first { it.name == "orphan" }.path)
        val json = Json.parse(File(store.root, "index.json").readText()) as Map<*, *>
        val paths = (json["repos"] as List<*>).map { (it as Map<*, *>)["path"] }.toSet()
        assertEq(setOf(File(store.root, "moved").path, File(store.root, "orphan").path), paths)
    }

    fun testReadsAnIndexWrittenByTheFlutterApp() = withStore { _, store ->
        store.root.mkdirs()
        val worktree = File(store.root, "field-notes").apply { mkdirs() }
        File(store.root, "index.json").writeText(
            """
            {
              "version": 1,
              "repos": [
                {"name": "field-notes", "path": ${Json.stringify(worktree.path)}, "description": "Notes",
                 "createdAt": "2026-08-20T10:00:00.000Z", "branch": "main", "lastCommit": "Initial commit",
                 "lastCommitAt": "2026-08-21T09:30:00.123456Z", "publishedAs": "alice/field-notes", "origin": "cloned"},
                {"name": "../evil", "path": "/tmp"},
                {"name": "no-path"}
              ]
            }
            """.trimIndent(),
        )
        val record = store.list().single()
        assertEq("field-notes", record.name)
        assertEq("Notes", record.description)
        assertEq(Instant.parse("2026-08-20T10:00:00Z").toEpochMilli(), record.createdAt)
        assertEq(Instant.parse("2026-08-21T09:30:00.123Z").toEpochMilli(), record.lastCommitAt)
        assertEq("alice/field-notes", record.publishedAs)
        assertEq(LocalRepoOrigin.cloned, record.origin)
        assertEq("Cloned", record.syncLabel)

        File(store.root, "index.json").writeText("{not json")
        expect(store.list().isEmpty())
    }

    fun testDeleteRemovesLinksInsideTheRepositoryWithoutFollowingThem() = withStore { temp, store ->
        val record = store.create(name = "linky", author = author)
        val outside = File(temp, "precious").apply { mkdirs() }
        File(outside, "data.txt").writeText("keep me")
        Files.createSymbolicLink(File(record.path, "outside-link").toPath(), outside.toPath())
        Files.createSymbolicLink(File(record.path, "file-link").toPath(), File(outside, "data.txt").toPath())
        store.delete("linky")
        expect(!File(record.path).exists())
        assertEq("keep me", File(outside, "data.txt").readText())
    }

    fun testUpdateSummaryKeepsTheListMostRecentFirst() = withStore { _, store ->
        val older = store.create(name = "older", author = author)
        store.create(name = "newer", author = author)
        store.updateSummary(
            "older",
            LocalRepoSummary(branch = "work", headId = null, lastCommitSubject = "", lastCommitAt = null),
        )
        val updated = store.find("older")!!
        assertEq("work", updated.branch)
        assertEq(null, updated.lastCommitAt)
        assertEq(older.path, updated.path)
        store.updateSummary(
            "older",
            LocalRepoSummary(branch = "work", headId = "a", lastCommitSubject = "Later", lastCommitAt = Long.MAX_VALUE / 2),
        )
        assertEq(listOf("older", "newer"), store.records.map { it.name })
        assertEq(null, store.updateSummary("missing", LocalRepoSummary("main", null, "", null)))
    }

    fun testDartDateTimeRoundTrips() {
        val millis = Instant.parse("2026-08-23T12:34:56.789Z").toEpochMilli()
        assertEq("2026-08-23T12:34:56.789Z", DartDateTime.toIso8601Utc(millis))
        assertEq("1970-01-01T00:00:00.000Z", DartDateTime.toIso8601Utc(0))
        assertEq(millis, DartDateTime.tryParse("2026-08-23T12:34:56.789Z"))
        assertEq(millis, DartDateTime.tryParse("2026-08-23 14:34:56.789+02:00"))
        assertEq(millis, DartDateTime.tryParse("20260823T123456.789Z"))
        assertEq(Instant.parse("2026-08-23T12:34:56Z").toEpochMilli(), DartDateTime.tryParse("2026-08-23T12:34:56z"))
        assertEq(null, DartDateTime.tryParse(""))
        assertEq(null, DartDateTime.tryParse("yesterday"))
        expect(DartDateTime.tryParse("2026-08-23") != null)
    }
}
