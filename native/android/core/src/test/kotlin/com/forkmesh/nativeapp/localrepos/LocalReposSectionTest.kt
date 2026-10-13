package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.time.Instant

class LocalReposSectionTest {
    private val author = GitAuthor(name = "Alice", email = "alice@example.com")

    fun testShowsTheEmptyStateBeforeAnyRepository() = withScratch("fm-local-section-") { temp ->
        val store = LocalRepoStore(File(temp, "repos"))
        expect(!store.isLoaded)
        store.ensureLoaded()
        expect(store.isLoaded)
        expect(store.records.isEmpty())
        expect(localMeshIdentities(store.records).isEmpty())
    }

    fun testListsLocalRepositoriesAndOpensTheWorkspace() = withScratch("fm-local-section-") { temp ->
        LocalRepoStore(File(temp, "repos")).create(name = "field-notes", description = "Notes", author = author)
        val store = LocalRepoStore(File(temp, "repos"))
        store.ensureLoaded()
        val record = store.records.single()
        assertEq("field-notes", record.name)
        assertEq("Local only", record.syncLabel)
        expect(localRepoTileMeta(record).startsWith("main · committed"), localRepoTileMeta(record))
        val workspace = store.workspace(record)
        try {
            expect(workspace.listFiles().any { it.name == "README.md" })
        } finally {
            workspace.dispose()
        }
    }

    fun testNewRepositoryDialogValidatesTheNameThenCreates() = withScratch("fm-local-section-") { temp ->
        val store = LocalRepoStore(File(temp, "repos"))
        assertEq("Use letters, digits, dots, dashes, or underscores only.", LocalRepoStore.validateName("bad name!".trim()))
        var notified = 0
        store.addListener { notified++ }
        store.create(name = "good-name", description = "", author = author)
        expect(notified > 0)
        assertEq("good-name", store.list().single().name)
        expect(!store.isBusy)
    }

    fun testTileMetaCarriesTheMeshIdentityAndTheLastCommit() {
        val now = Instant.parse("2026-08-23T12:00:00Z").toEpochMilli()
        val local = LocalRepoRecord(name = "notes", path = "/x", createdAt = 0, lastCommitAt = now - 3 * 3_600_000L)
        assertEq("main · committed 3h ago", localRepoTileMeta(local, now))
        val published = local.copyWith(publishedAs = "alice/notes")
        assertEq("alice/notes · committed 3h ago", localRepoTileMeta(published, now))
        assertEq("Published", published.syncLabel)
        assertEq("Cloned", published.copyWith(origin = LocalRepoOrigin.cloned).syncLabel)
        assertEq("main · no commits yet", localRepoTileMeta(local.copyWith(clearLastCommit = true), now))
        assertEq("Local only", published.copyWith(clearPublishedAs = true).syncLabel)
    }

    fun testMeshIdentitiesJoinPublishedAndClonedRowsLowercased() {
        val records = listOf(
            LocalRepoRecord(name = "a", path = "/a", createdAt = 0, publishedAs = "Alice/Notes"),
            LocalRepoRecord(name = "b", path = "/b", createdAt = 0, publishedAs = "net/Catalog", origin = LocalRepoOrigin.cloned),
            LocalRepoRecord(name = "c", path = "/c", createdAt = 0),
            LocalRepoRecord(name = "d", path = "/d", createdAt = 0, publishedAs = ""),
        )
        assertEq(setOf("alice/notes", "net/catalog"), localMeshIdentities(records))
    }

    fun testImportNameSuggestions() {
        assertEq("My-Project-v2", slugifyImportName("My Project (v2)"))
        assertEq("imported", slugifyImportName("..."))
        assertEq("imported", slugifyImportName("---"))
        assertEq("a.b", slugifyImportName(".a.b-"))
        assertEq("x".repeat(100), slugifyImportName("x".repeat(150)))
        assertEq("Archive", suggestedImportName("Archive.ZIP", zip = true))
        assertEq("project-backup", suggestedImportName("project backup.zip", zip = true))
        assertEq("My-Folder", suggestedImportName("docs/My Folder", zip = false))
        assertEq("imported", suggestedImportName("", zip = false))
    }

    fun testFailureMessagesAreReadable() {
        assertEq("That branch does not exist.", describeLocalRepoFailure(GitWorkspaceException("That branch does not exist.")))
        assertEq(
            "A local repository with that name already exists.",
            describeLocalRepoFailure(LocalRepoStoreException("A local repository with that name already exists.")),
        )
        expect(describeLocalRepoFailure(IllegalStateException("boom")).contains("boom"))
    }
}
