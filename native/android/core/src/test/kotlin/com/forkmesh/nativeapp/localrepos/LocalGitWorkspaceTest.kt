package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.GitErrorCode
import com.forkmesh.nativeapp.git.GitException
import com.forkmesh.nativeapp.git.GitRepository
import com.forkmesh.nativeapp.git.GitRepositoryOpen
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.nio.file.Files
import java.util.Collections
import java.util.concurrent.atomic.AtomicInteger
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

class LocalGitWorkspaceTest {
    private val author = GitAuthor(name = "Alice Mobile", email = "alice@example.com")

    private inline fun <T> withWorkspace(temp: File, name: String, block: (GitWorkspace, File) -> T): T {
        val worktree = File(temp, name)
        val workspace = GitWorkspace(worktree)
        try {
            return block(workspace, worktree)
        } finally {
            workspace.dispose()
        }
    }

    fun testAuthorKeepsUtf8ButStripsAngleBracketsAndControlCharacters() {
        val author = GitAuthor.fromAccount(nodeName = "ålice <x>", email = "alïce@example.com")
        assertEq("ålice x", author.name)
        assertEq("alïce@example.com", author.email)
        author.toIdentity(System.currentTimeMillis())
        val fallback = GitAuthor.fromAccount(nodeName = "", email = "")
        assertEq("forkmesh", fallback.name)
        assertEq("forkmesh@mobile.forkmesh", fallback.email)
        fallback.toIdentity(System.currentTimeMillis())
        assertEq("bob@mobile.forkmesh", GitAuthor.fromAccount(nodeName = " bob ", email = "no-at-sign").email)
    }

    fun testInitWriteStageCommitBranchCheckoutLogRoundTrip() = withScratch("fm-git-workspace-") { temp ->
        withWorkspace(temp, "demo") { workspace, worktree ->
            val first = workspace.init(files = mapOf("README.md" to "# demo\n"), commitMessage = "Initial commit\n", author = author)
            expect(first != null)
            assertEq(40, first!!.length)

            val repo = GitRepository.open(GitRepositoryOpen(worktree))
            val head = repo.readHeadObjectId()
            assertEq(first, head?.hex)
            val commit = repo.readCommit(head!!)
            assertEq("Initial commit\n", String(commit.message, Charsets.UTF_8))
            expect(commit.parents.isEmpty())
            val tree = repo.readTree(commit.tree)
            assertEq(listOf("README.md"), tree.entries.map { String(it.name, Charsets.UTF_8) })
            assertEq("# demo\n", String(repo.readBlob(tree.entries.single().objectId), Charsets.UTF_8))

            workspace.writeFile("README.md", "# demo\n\nHello.\n")
            workspace.writeFile("src/main.dart", "void main() {}\n")
            val files = workspace.listFiles()
            assertEq(listOf("src", "README.md"), files.map { it.path })
            expect(files.first().isDirectory)
            expect(!files.first().tracked)
            expect(files.last().tracked)
            assertEq("src/main.dart", workspace.listFiles("src").single().path)

            var status = workspace.status()
            expect(status.staged.isEmpty())
            assertEq(listOf(LocalChange(path = "README.md", kind = LocalChangeKind.modified, staged = false)), status.unstaged)
            assertEq(listOf("src/main.dart"), status.untracked.map { it.path })

            workspace.stage(listOf("README.md"))
            status = workspace.status()
            assertEq("README.md", status.staged.single().path)
            assertEq(LocalChangeKind.modified, status.staged.single().kind)
            expect(status.unstaged.isEmpty())
            workspace.unstage(listOf("README.md"))
            status = workspace.status()
            expect(status.staged.isEmpty())
            assertEq("README.md", status.unstaged.single().path)
            workspace.stageAll()
            status = workspace.status()
            assertEq(listOf("README.md", "src/main.dart"), status.staged.map { it.path })
            expect(!status.hasWorktreeChanges)

            assertFails<GitWorkspaceException> { workspace.commit(message = "   ", author = author) }
            val second = workspace.commit(message = "Add main", author = author)
            expect(second != first)
            expect(workspace.status().isClean)

            var branches = workspace.branches()
            assertEq(listOf("main"), branches.map { it.name })
            expect(branches.single().isCurrent)
            assertEq(second, branches.single().tipId)
            workspace.createBranch("feature/x")
            branches = workspace.branches()
            assertEq(listOf("feature/x", "main"), branches.map { it.name })
            expect(branches.first { it.name == "main" }.isCurrent)
            workspace.checkout("feature/x")
            expect(workspace.branches().first { it.name == "feature/x" }.isCurrent)
            workspace.writeFile("src/main.dart", "void main() { print(1); }\n")
            workspace.stage(listOf("src/main.dart"))
            val third = workspace.commit(message = "Feature work\n\nBody line.\n", author = author)

            workspace.writeFile("README.md", "dirty\n")
            val dirty = assertFails<GitWorkspaceException> { workspace.checkout("main") }
            assertEq(GitErrorCode.operationInterrupted, dirty.code)
            expect(dirty.message.contains("Commit or discard"), dirty.message)
            workspace.discard(listOf("README.md"))
            expect(workspace.status().isClean)
            workspace.checkout("main")
            assertEq("void main() {}\n", workspace.readFile("src/main.dart").text)
            workspace.checkout("feature/x")

            val log = workspace.log()
            assertEq(listOf(third, second, first), log.map { it.id })
            assertEq("Feature work", log.first().subject)
            assertEq("Feature work\n\nBody line.\n", log.first().message)
            assertEq("Alice Mobile", log.first().authorName)
            assertEq("alice@example.com", log.first().authorEmail)
            assertEq(listOf(second), log.first().parents)
            expect(log.last().parents.isEmpty())
            expect(Math.abs(log.first().time - System.currentTimeMillis()) < 5 * 60 * 1000)
            assertEq(2, workspace.log(limit = 2).size)

            val summary = workspace.summary()
            assertEq("feature/x", summary.branch)
            assertEq(third, summary.headId)
            assertEq("Feature work", summary.lastCommitSubject)

            val reopened = GitRepository.open(GitRepositoryOpen(worktree))
            assertEq("refs/heads/feature/x", reopened.readHead().target)
            assertEq(third, reopened.readHeadObjectId()?.hex)
            expect(reopened.status().isClean)
        }
    }

    fun testReadFileFlagsBinaryAndOversizeContent() = withScratch("fm-git-workspace-") { temp ->
        withWorkspace(temp, "bin") { workspace, worktree ->
            workspace.init()
            File(worktree, "blob.bin").writeBytes(byteArrayOf(0, 1, 2, 3))
            val content = workspace.readFile("blob.bin")
            expect(content.isBinary)
            expect(!content.isEditable)
            assertEq(4, content.bytes)
            assertEq("blob.bin", content.path)

            File(worktree, "big.txt").writeBytes(ByteArray(LocalFileContent.maxEditableBytes + 1) { 'a'.code.toByte() })
            val big = workspace.readFile("big.txt")
            expect(big.isTooLarge)
            expect(!big.isEditable)

            File(worktree, "latin.txt").writeBytes(byteArrayOf(0x63, 0x61, 0x66, 0xe9.toByte()))
            expect(workspace.readFile("latin.txt").isBinary)
        }
    }

    fun testRejectsUnsafePathsAndDotGitWrites() = withScratch("fm-git-workspace-") { temp ->
        withWorkspace(temp, "safe") { workspace, _ ->
            workspace.init()
            val escape = assertFails<GitWorkspaceException> { workspace.writeFile("../escape.txt", "x") }
            assertEq(GitErrorCode.pathEscapesWorktree, escape.code)
            assertEq("A path was rejected because it is not safe or portable.", escape.message)
            assertFails<GitWorkspaceException> { workspace.writeFile(".git/config", "x") }
            assertFails<GitWorkspaceException> { workspace.deleteFile(".git/HEAD") }
            assertFails<GitWorkspaceException> { workspace.listFiles("nope") }
            expect(!File(temp, "escape.txt").exists())
        }
    }

    fun testImportFromDirectoryCopiesFilesSkipsSymlinksAndDotGit() = withScratch("fm-git-workspace-") { temp ->
        val source = File(temp, "source").apply { mkdirs() }
        File(source, "a.txt").writeText("A")
        File(source, "lib").mkdirs()
        File(source, "lib/b.dart").writeText("B")
        File(source, ".git").mkdirs()
        File(source, ".git/HEAD").writeText("ref: refs/heads/x\n")
        Files.createSymbolicLink(File(source, "link").toPath(), File("a.txt").toPath())

        withWorkspace(temp, "imported") { workspace, worktree ->
            val result = workspace.importFromDirectory(source, author = author, message = "Import\n")
            assertEq(2, result.importedFiles)
            assertEq(2, result.skippedEntries)
            expect(!Files.exists(File(worktree, "link").toPath(), java.nio.file.LinkOption.NOFOLLOW_LINKS))
            assertEq("ref: refs/heads/main\n", File(worktree, ".git/HEAD").readText())
            assertEq("Import", workspace.log().single().subject)
            expect(workspace.status().isClean)
            assertEq(listOf("lib", "a.txt"), workspace.listFiles().map { it.path })
        }
    }

    fun testImportFromDirectoryRefusesAMissingFolder() = withScratch("fm-git-workspace-") { temp ->
        withWorkspace(temp, "missing") { workspace, _ ->
            val error = assertFails<GitWorkspaceException> {
                workspace.importFromDirectory(File(temp, "nope"), author = author)
            }
            assertEq("The selected folder could not be read.", error.message)
        }
    }

    fun testImportFromZipStripsASharedRootFolder() = withScratch("fm-git-workspace-") { temp ->
        val zip = File(temp, "proj.zip")
        ZipOutputStream(zip.outputStream()).use { out ->
            for ((name, text) in listOf("proj/README.md" to "# p\n", "proj/src/x.txt" to "x", "__MACOSX/proj/._README.md" to "junk")) {
                out.putNextEntry(ZipEntry(name))
                out.write(text.toByteArray())
                out.closeEntry()
            }
        }
        withWorkspace(temp, "fromzip") { workspace, _ ->
            val result = workspace.importFromZip(zip.path, author = author)
            assertEq(2, result.importedFiles)
            assertEq(listOf("src", "README.md"), workspace.listFiles().map { it.path })
            assertEq("# p\n", workspace.readFile("README.md").text)
            assertEq("x", workspace.readFile("src/x.txt").text)
        }
    }

    fun testCancelStopsTheWorkerAndSurfacesACancelledError() = withScratch("fm-git-workspace-") { temp ->
        withWorkspace(temp, "cancel") { workspace, worktree ->
            workspace.init()
            val filler = "x".repeat(2000)
            for (i in 0 until 400) File(worktree, "f$i.txt").writeText(filler)
            var failure: Throwable? = null
            val caller = Thread {
                try {
                    workspace.status()
                } catch (error: Throwable) {
                    failure = error
                }
            }
            caller.start()
            val deadline = System.currentTimeMillis() + 5000
            while (!workspace.isBusy && caller.isAlive && System.currentTimeMillis() < deadline) Thread.sleep(1)
            workspace.cancel()
            caller.join(60_000)
            expect(!caller.isAlive)
            val error = failure
            if (error != null) {
                expect(error is GitWorkspaceException, "unexpected $error")
                assertEq(GitErrorCode.cancelled, (error as GitWorkspaceException).code)
                assertEq("Operation cancelled.", error.message)
            }
            expect(!workspace.isBusy)
            assertEq(400, workspace.status().untracked.size)
        }
    }

    fun testRunnerRecordsDurationsAndSerializesOperations() = withScratch("fm-git-workspace-") { temp ->
        val names = Collections.synchronizedList(ArrayList<String>())
        val runner = GitIsolateRunner { name, _, _ -> names.add(name) }
        val workspace = GitWorkspace(File(temp, "perf"), runner = runner)
        try {
            workspace.init()
            workspace.status()
            workspace.branches()
            assertEq(listOf("init", "status", "branches"), names.toList())
        } finally {
            workspace.dispose()
        }

        val serial = GitIsolateRunner()
        val active = AtomicInteger()
        val peak = AtomicInteger()
        val threads = (0 until 4).map { index ->
            Thread {
                serial.run("op$index") {
                    peak.accumulateAndGet(active.incrementAndGet(), ::maxOf)
                    Thread.sleep(30)
                    active.decrementAndGet()
                }
            }
        }
        threads.forEach(Thread::start)
        threads.forEach { it.join(30_000) }
        assertEq(1, peak.get())
    }

    fun testRunnerMapsErrorsAndRefusesWorkOnceDisposed() {
        val runner = GitIsolateRunner()
        val engine = assertFails<GitException> { runner.run<Unit>("engine") { throw GitException(GitErrorCode.refLocked, "locked") } }
        assertEq(GitErrorCode.refLocked, engine.code)
        val worker = assertFails<GitWorkerException> { runner.run<Unit>("io") { throw java.io.IOException("disk gone") } }
        expect(worker.message.contains("disk gone"), worker.message)
        val progress = Collections.synchronizedList(ArrayList<Any>())
        assertEq(7, runner.runWithProgress("progress", { context -> context.progress("sample"); 7 }, { progress.add(it) }))
        assertEq(listOf<Any>("sample"), progress.toList())
        runner.dispose()
        val disposed = assertFails<GitException> { runner.run("late") { 1 } }
        assertEq(GitErrorCode.cancelled, disposed.code)
        assertEq("Operation cancelled.", GitWorkspace.describe(disposed))
    }

    fun testMonitorHookReceivesGitOperationEvents() = withScratch("fm-git-workspace-") { temp ->
        val events = Collections.synchronizedList(ArrayList<Pair<String, Map<String, Any?>>>())
        val workspace = GitWorkspace(
            File(temp, "mon"),
            performanceMonitor = GitPerformanceMonitor { name, _, details -> events.add(name to details) },
        )
        try {
            workspace.init()
            expect(events.any { it.first == "git.init" && it.second["repo"] == "mon" })
            assertFails<GitWorkspaceException> { workspace.listFiles("missing") }
            expect(events.any { it.first == "git.list-files" && it.second["failed"] == true })
        } finally {
            workspace.dispose()
        }
    }

    fun testTransferProgressLabels() {
        assertEq("Connecting to ForkMesh", GitTransferProgress(phase = "connecting").label)
        assertEq("Downloading", GitTransferProgress(phase = "receivingPack").label)
        assertEq("Downloading 512 B", GitTransferProgress(phase = "receivingPack", bytes = 512).label)
        assertEq("Downloading 2 KB", GitTransferProgress(phase = "receivingPack", bytes = 1536).label)
        assertEq("Downloading 3.0 MB", GitTransferProgress(phase = "receivingPack", bytes = 3L * 1024 * 1024).label)
        assertEq("Indexing 12 objects", GitTransferProgress(phase = "indexingPack", objects = 12).label)
        assertEq("Uploading 1.5 MB", GitTransferProgress(phase = "sendingPack", bytes = 1536L * 1024).label)
        assertEq("Working", GitTransferProgress(phase = "mystery").label)
    }

    fun testSyncStateComparesTheBranchWithItsTrackingRef() = withScratch("fm-git-workspace-") { temp ->
        withWorkspace(temp, "sync") { workspace, worktree ->
            val base = workspace.init(files = mapOf("a.txt" to "a\n"), commitMessage = "base\n", author = author)!!
            assertEq("Not pushed yet", workspace.syncState(branch = "main").label)
            val tracking = File(worktree, ".git/refs/remotes/origin/main")
            tracking.parentFile.mkdirs()
            tracking.writeText("$base\n")
            assertEq("In sync with ForkMesh", workspace.syncState(branch = "main").label)
            workspace.writeFile("a.txt", "b\n")
            workspace.stageAll()
            workspace.commit(message = "ahead", author = author)
            val state = workspace.syncState(branch = "main")
            assertEq(1, state.ahead)
            assertEq(null, state.behind)
            assertEq("1 to push · 500+ to fetch", state.label)
            assertEq("", LocalSyncState.unknown("").branch)
        }
    }

    fun testLocalSyncStateLabels() {
        assertEq("Not pushed yet", LocalSyncState("main", "a", null, null, null).label)
        assertEq("In sync with ForkMesh", LocalSyncState("main", "a", "a", 0, 0).label)
        assertEq("2 to push", LocalSyncState("main", "a", "b", 2, 0).label)
        assertEq("3 to fetch", LocalSyncState("main", "a", "b", 0, 3).label)
        assertEq("500+ to push · 500+ to fetch", LocalSyncState("main", "a", "b", null, null).label)
        assertEq("In sync with ForkMesh", LocalSyncState("main", "a", "b", 0, 0).label)
    }

    fun testRepositoryCreatedByTheAppPassesGitFsckStrict() = withScratch("fm-git-workspace-") { temp ->
        if (!HostGit.available) return@withScratch
        withWorkspace(temp, "interop") { workspace, worktree ->
            workspace.init(
                files = mapOf("README.md" to "# interop\n", "docs/guide.md" to "guide\n"),
                commitMessage = "Initial commit\n",
                author = author,
            )
            workspace.writeFile("docs/guide.md", "guide v2\n")
            workspace.stageAll()
            workspace.commit(message = "Update guide", author = author)
            workspace.createBranch("topic")

            val fsck = HostGit.run(worktree, "fsck", "--strict", "--no-dangling")
            assertEq(0, fsck.first, fsck.second)
            val log = HostGit.run(worktree, "log", "--format=%s")
            assertEq(0, log.first, log.second)
            assertEq(listOf("Update guide", "Initial commit"), log.second.trim().lines())
            val branches = HostGit.run(worktree, "branch", "--format=%(refname:short)").second.trim().lines()
            expect(branches.containsAll(listOf("main", "topic")), branches.toString())
            val status = HostGit.run(worktree, "status", "--porcelain")
            assertEq(0, status.first, status.second)
            assertEq("", status.second.trim(), "index must agree")
        }
    }
}
