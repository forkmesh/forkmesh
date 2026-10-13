package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.chat.NodeSyncApplyException
import com.forkmesh.nativeapp.chat.NodeSyncAuthor
import com.forkmesh.nativeapp.chat.NodeSyncRepoRecord
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.File

class LocalRepoNodeSyncTest {
    private val author = GitAuthor(name = "Alice", email = "alice@example.com")

    fun testListsRecordsWithTheirMeshIdentity() = withScratch("fm-node-sync-") { temp ->
        val store = LocalRepoStore(File(temp, "docs/forkmesh/repos"))
        val record = store.create(name = "notes", author = author)
        store.create(name = "draft", author = author)
        store.markPublished("notes", "alice/notes")
        val repos = LocalRepoNodeSync(store)
        repos.ensureLoaded()
        val byName = repos.records.associateBy { it.name }
        assertEq(setOf("notes", "draft"), byName.keys)
        assertEq("alice/notes", byName.getValue("notes").publishedAs)
        assertEq(null, byName.getValue("draft").publishedAs)
        assertEq(record.path, byName.getValue("notes").path)
    }

    fun testWritesStagesAndCommitsThroughTheWorkspace() = withScratch("fm-node-sync-") { temp ->
        val store = LocalRepoStore(File(temp, "docs/forkmesh/repos"))
        store.create(name = "notes", author = author)
        val repos = LocalRepoNodeSync(store)
        repos.ensureLoaded()
        val workspace = repos.workspace(repos.records.single())
        try {
            assertEq("main", workspace.currentBranch())
            workspace.writeFile(".forkmesh/issues/1.json", "{\"title\":\"Hi\"}")
            workspace.stage(listOf(".forkmesh/issues/1.json"))
            val id = workspace.commit("Sync issue 1", NodeSyncAuthor("Alice", "alice@example.com"))
            expect(id.length == 40, "commit id $id")
            assertEq("{\"title\":\"Hi\"}", workspace.readFile(".forkmesh/issues/1.json").text)
            assertEq(listOf("1.json"), workspace.listFiles(".forkmesh/issues").map { it.name })
            val again = assertFails<Exception> { workspace.commit("Nothing", NodeSyncAuthor("Alice", "alice@example.com")) }
            expect((again.message ?: "").contains("Nothing is staged"), again.message ?: "")
        } finally {
            workspace.dispose()
        }
        repos.refresh("notes")
        assertEq("Sync issue 1", store.find("notes")?.lastCommitSubject)
    }

    fun testRefusesARepositoryThatIsGone() = withScratch("fm-node-sync-") { temp ->
        val store = LocalRepoStore(File(temp, "docs/forkmesh/repos"))
        val repos = LocalRepoNodeSync(store)
        repos.ensureLoaded()
        assertFails<NodeSyncApplyException> { repos.workspace(NodeSyncRepoRecord("gone", File(temp, "gone").path, null)) }
    }
}
