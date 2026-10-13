package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class GitRepositoryPackTest {
    fun testWalksACommitAndItsNestedObjectsIntoANonDeltaPack() = withTempDir("forkmesh-reachability-") { root ->
        val repository = GitRepository.init(GitRepositoryInit(root))
        root.child("hello.txt").writeText("hello\n")
        repository.add(listOf(GitWorktreePath("hello.txt")))
        val identity = GitIdentity(name = "Ada", email = "ada@example.test", timestampSeconds = 0, timezoneOffsetMinutes = 0)
        val commit = repository.commit(GitCommitRequest(message = "fixture\n", author = identity, committer = identity))

        val pack = repository.writePack(listOf(commit))
        val objects = GitPackReader.parse(pack.packBytes, GitResourceLimits()).objects

        expect(objects.map { it.objectId }.contains(commit), "pack contains the commit")
        expect(
            objects.map { it.type }.containsAll(listOf(GitObjectType.commit, GitObjectType.tree, GitObjectType.blob)),
            "pack contains a commit, a tree and a blob",
        )
        assertEq(
            "hello\n",
            utf8Text(repository.readBlob(objects.first { it.type == GitObjectType.blob }.objectId)),
        )
    }
}
