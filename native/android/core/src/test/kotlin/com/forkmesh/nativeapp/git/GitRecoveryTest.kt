package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class GitRecoveryTest {
    fun testContractExposesStableTypedErrorCodesAndImmutableHardCaps() {
        assertEq("invalidRepository", GitErrorCode.invalidRepository.wireName)
        assertEq("cancelled", GitErrorCode.cancelled.wireName)
        assertFails<IllegalArgumentException> {
            GitResourceLimits(maxObjectBytes = GitResourceLimits.hardMaxObjectBytes + 1)
        }
    }

    fun testQuarantineRejectionAndRecoveryRemoveOperationQuarantineWithoutChangingHead() =
        withTempDir("forkmesh-quarantine-") { root ->
            val repository = GitRepository.init(GitRepositoryInit(root))
            val headBefore = repository.gitDirectory.child("HEAD").readText()
            val quarantine = GitObjectQuarantine.begin(repository.gitDirectory)
            quarantine.stage("incoming.pack", byteArrayOf(1, 2, 3))

            quarantine.reject()

            expect(!quarantine.directory.exists())
            assertEq(headBefore, repository.gitDirectory.child("HEAD").readText())
        }

    fun testQuarantineCancellationAndRestartRecoveryLeaveNoValidQuarantineDirectory() =
        withTempDir("forkmesh-quarantine-cancel-") { root ->
            val repository = GitRepository.init(GitRepositoryInit(root))
            val cancellation = GitCancellationToken()
            cancellation.cancel()
            val quarantine = GitObjectQuarantine.begin(repository.gitDirectory, cancellation = cancellation)

            assertGitError(GitErrorCode.cancelled) { quarantine.stage("incoming.pack", byteArrayOf(1)) }
            GitObjectQuarantine.recover(repository.gitDirectory)

            expect(!quarantine.directory.exists())
        }

    fun testQuarantineOverLimitStagingIsRejectedAndLeavesNoOperationDirectory() =
        withTempDir("forkmesh-quarantine-limit-") { root ->
            val repository = GitRepository.init(GitRepositoryInit(root))
            val quarantine = GitObjectQuarantine.begin(
                repository.gitDirectory,
                limits = GitResourceLimits(maxInflatedBytes = 1),
            )

            assertGitError(GitErrorCode.resourceLimitExceeded) { quarantine.stage("incoming.pack", byteArrayOf(1, 2)) }

            expect(!quarantine.directory.exists())
        }

    fun testQuarantineRepositoryOpenClearsAnInterruptedQuarantineBeforeExposingState() =
        withTempDir("forkmesh-quarantine-open-") { root ->
            val repository = GitRepository.init(GitRepositoryInit(root))
            val quarantine = GitObjectQuarantine.begin(repository.gitDirectory)
            quarantine.stage("interrupted.pack", byteArrayOf(1, 2, 3))

            GitRepository.open(GitRepositoryOpen(root))

            expect(!quarantine.directory.exists())
        }

    fun testJournalRefusesToOpenWhenARecoveryJournalIsMalformed() = withTempDir("forkmesh-journal-") { root ->
        GitRepository.init(GitRepositoryInit(root))
        val journal = root.child(".git").child("forkmesh-journal")
        journal.mkdir()
        journal.child("broken").writeText("not a ForkMesh journal")

        assertGitError(GitErrorCode.operationInterrupted) { GitRepository.open(GitRepositoryOpen(root)) }
    }

    fun testUntrustedPackRejectionLeavesRepositoryRefsAndObjectStoreUnchanged() =
        withTempDir("forkmesh-fail-closed-") { root ->
            val repository = GitRepository.init(GitRepositoryInit(root))
            val head = repository.gitDirectory.child("HEAD")
            val headBefore = head.readText()
            val objects = repository.gitDirectory.child("objects")
            val objectEntriesBefore = objects.list()!!.size
            val pack = GitPackWriter.write(
                listOf(
                    GitPackWriteObject(GitObjectType.blob, byteArrayOf(1)),
                    GitPackWriteObject(GitObjectType.blob, byteArrayOf(2)),
                ),
            )

            assertGitError(GitErrorCode.hashScreeningUnavailable) {
                GitFetchResponse.validateUntrustedPack(pack.packBytes, GitResourceLimits())
            }

            assertEq(headBefore, head.readText())
            assertEq(objectEntriesBefore, objects.list()!!.size)
        }
}
