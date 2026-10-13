package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.chat.NodeSyncApplyException
import com.forkmesh.nativeapp.chat.NodeSyncAuthor
import com.forkmesh.nativeapp.chat.NodeSyncFileContent
import com.forkmesh.nativeapp.chat.NodeSyncFileEntry
import com.forkmesh.nativeapp.chat.NodeSyncRepoRecord
import com.forkmesh.nativeapp.chat.NodeSyncRepositories
import com.forkmesh.nativeapp.chat.NodeSyncWorkspace

class LocalRepoNodeSync(private val store: LocalRepoStore) : NodeSyncRepositories {
    override fun ensureLoaded() = store.ensureLoaded()

    override val records: List<NodeSyncRepoRecord>
        get() = store.records.map { NodeSyncRepoRecord(name = it.name, path = it.path, publishedAs = it.publishedAs) }

    override fun workspace(record: NodeSyncRepoRecord): NodeSyncWorkspace {
        val local = store.records.firstOrNull { it.path == record.path }
            ?: throw NodeSyncApplyException("${record.name} is no longer on this device")
        return Workspace(store.workspace(local))
    }

    override fun refresh(name: String) {
        store.refresh(name)
    }

    private class Workspace(private val git: GitWorkspace) : NodeSyncWorkspace {
        override fun currentBranch(): String = git.summary().branch

        override fun readFile(path: String): NodeSyncFileContent =
            git.readFile(path).let { NodeSyncFileContent(text = it.text, isBinary = it.isBinary, isTooLarge = it.isTooLarge) }

        override fun listFiles(directory: String): List<NodeSyncFileEntry> =
            git.listFiles(directory).map { NodeSyncFileEntry(name = it.name, path = it.path, isDirectory = it.isDirectory) }

        override fun writeFile(path: String, text: String) {
            git.writeFile(path, text)
        }

        override fun deleteFile(path: String) {
            git.deleteFile(path)
        }

        override fun stage(paths: List<String>) {
            git.stage(paths)
        }

        override fun commit(message: String, author: NodeSyncAuthor): String =
            git.commit(message, GitAuthor(name = author.name, email = author.email))

        override fun dispose() {
            git.dispose()
        }
    }
}
