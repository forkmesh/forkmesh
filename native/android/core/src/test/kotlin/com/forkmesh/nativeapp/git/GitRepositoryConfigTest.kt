package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect

class GitRepositoryConfigTest {
    fun testAcceptsOnlyTheSupportedNonBareSha1CoreConfiguration() {
        val config = RepositoryConfig.parse(
            utf8(
                "[core]\n" +
                    "repositoryformatversion = 0\n" +
                    "bare = false\n" +
                    "filemode = true\n" +
                    "[extensions]\n" +
                    "objectformat = sha1\n",
            ),
        )

        expect(config.fileMode)
    }

    fun testRejectsIncludeDirectivesAndExternalWorktrees() {
        assertFails<GitException> { RepositoryConfig.parse(utf8("[include]\npath = /tmp/x\n")) }
        assertFails<GitException> { RepositoryConfig.parse(utf8("[core]\nworktree = /tmp/x\n")) }
    }

    fun testParsesOneValidatedRemoteAndBranchUpstream() {
        val config = RepositoryConfig.parse(
            utf8(
                "[core]\n" +
                    "repositoryformatversion = 0\n" +
                    "[remote \"origin\"]\n" +
                    "\turl = https://forkmesh.test/ada/notes\n" +
                    "\tfetch = +refs/heads/*:refs/remotes/origin/*\n" +
                    "[branch \"main\"]\n" +
                    "\tremote = origin\n" +
                    "\tmerge = refs/heads/main\n",
            ),
        )

        val remote = config.remotes.getValue("origin")
        assertEq("https://forkmesh.test/ada/notes", remote.url.uri.toString())
        expect(remote.fetch.isWildcard)
        val branch = config.branches.getValue("main")
        assertEq("origin", branch.remote)
        assertEq("refs/heads/main", branch.merge.value)
    }

    fun testRejectsNonHttpsRemoteUrlsAndMultipleFetchRefspecs() {
        assertFails<GitException> { RepositoryConfig.parse(utf8("[remote \"origin\"]\nurl = ssh://host/repo\n")) }
        assertFails<GitException> {
            RepositoryConfig.parse(
                utf8(
                    "[remote \"origin\"]\n" +
                        "url = https://forkmesh.test/r\n" +
                        "fetch = +refs/heads/*:refs/remotes/origin/*\n" +
                        "fetch = +refs/heads/*:refs/remotes/other/*\n",
                ),
            )
        }
        assertFails<GitException> {
            RepositoryConfig.parse(utf8("[remote \"origin\"]\nurl = https://user:pass@forkmesh.test/r\n"))
        }
    }

    fun testRejectsAFetchRefspecThatTargetsAnotherRemoteNamespace() {
        assertFails<GitException> {
            RepositoryConfig.parse(
                utf8(
                    "[remote \"origin\"]\n" +
                        "url = https://forkmesh.test/r\n" +
                        "fetch = +refs/heads/*:refs/remotes/other/*\n",
                ),
            )
        }
    }

    fun testWritesRemoteAndBranchSectionsAtomicallyAndReValidates() = withTempDir("forkmesh-config-") { root ->
        val file = root.child("config")
        file.writeText("[core]\n\trepositoryformatversion = 0\n\tbare = false\n")

        val url = GitRemoteUrl.parse("https://forkmesh.test/ada/notes")
        RepositoryConfig.writeRemote(file, "origin", url = url, fetch = GitFetchRefspec.standard("origin"))
        RepositoryConfig.writeBranchUpstream(
            file,
            "main",
            remote = "origin",
            merge = GitRefName.branch("refs/heads/main"),
        )
        RepositoryConfig.writeRemote(file, "origin", url = url, fetch = GitFetchRefspec.standard("origin"))

        val config = RepositoryConfig.load(file)
        assertEq(listOf("origin"), config.remotes.keys.toList())
        assertEq(listOf("main"), config.branches.keys.toList())
        val text = file.readText()
        assertEq(1, Regex(Regex.escape("[remote \"origin\"]")).findAll(text).count())
        expect(text.contains("[core]"))
    }
}
