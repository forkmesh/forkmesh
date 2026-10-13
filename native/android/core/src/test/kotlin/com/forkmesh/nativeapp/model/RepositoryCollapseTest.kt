package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.testing.assertEq

class RepositoryCollapseTest {
    private fun row(vararg pairs: Pair<String, Any?>): Repository = Repository.fromJson(mapOf(*pairs))

    fun testMirrorRowsFoldUnderTheOrganizationByAliasRootCommitAndCloneUrl() {
        val repos = collapseRepositoryMirrors(
            listOf(
                row("owner" to "mirror8", "name" to "forkmesh", "rootCommit" to "r1", "stars" to 3, "updatedAt" to 5),
                row(
                    "owner" to "mirror23",
                    "name" to "forkmesh",
                    "rootCommit" to "r1",
                    "logicalOwners" to listOf(mapOf("kind" to "organization", "owner" to "forkmesh")),
                    "stars" to 7,
                    "liveHost" to true,
                ),
                row(
                    "owner" to "mirror30",
                    "name" to "forkmesh",
                    "source" to "remote-clone",
                    "cloneUrl" to "https://app.forkmesh.com/mirror23/forkmesh.git",
                ),
                row("owner" to "alice", "name" to "forkmesh", "rootCommit" to "r1", "forkedFrom" to "forkmesh/forkmesh"),
                row("owner" to "bob", "name" to "notes"),
            ),
        )
        assertEq(listOf("forkmesh/forkmesh", "alice/forkmesh", "bob/notes"), repos.map { it.fullName })
        val flagship = repos.first()
        assertEq("mirror23", flagship.owner)
        assertEq("forkmesh", flagship.publicOwner)
        assertEq(3, flagship.mirrors)
        assertEq(7, flagship.stars)
        assertEq(true, flagship.cloneOnline)
        assertEq("", repos[1].publicOwner)
        assertEq("bob", repos[2].displayOwner)
    }

    fun testTheLegacyOwnerKindNamesTheOrganization() {
        val repo = row("owner" to "mirror8", "name" to "x", "ownerKind" to "organization", "logicalOwner" to "acme")
        assertEq("acme", repo.organization)
        assertEq("acme/x", collapseRepositoryMirrors(listOf(repo)).single().fullName)
    }
}
