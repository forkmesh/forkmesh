package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File

class GitNoProcessFallbackTest {
    fun testProductionSourcesUseNoProcessApiOrForeignGitLibrary() {
        val sources = File(System.getProperty("user.dir"), "src/main/kotlin/com/forkmesh/nativeapp/git")
        if (!sources.isDirectory) {
            println("SKIP GitNoProcessFallbackTest: engine sources not found from ${System.getProperty("user.dir")}")
            return
        }
        val forbiddenProcessApi = Regex("""\bProcess\b|\bProcessBuilder\b|\bProcessHandle\b|\bRuntime\s*\.\s*getRuntime\s*\(\s*\)\s*\.\s*exec\b""")
        val forbiddenGitPackage = Regex("""(?m)^\s*import\s+(org\.eclipse\.jgit|com\.github\.git24j|[\w.]*libgit2)""")
        val violations = sources.walkTopDown()
            .filter { it.isFile && it.name.endsWith(".kt") }
            .filter { file ->
                val content = file.readText()
                forbiddenProcessApi.containsMatchIn(content) || forbiddenGitPackage.containsMatchIn(content)
            }
            .map { it.relativeTo(sources).path }
            .toList()
        assertEq(emptyList<String>(), violations, "production-process-api=FAILED files")
        println("production-process-api=PROVED")
    }

    fun testRuntimeRepositoryUseNeverInvokesAGitExecutable() {
        val shell = File("/bin/sh")
        val java = File(File(System.getProperty("java.home"), "bin"), "java")
        if (!shell.canExecute() || !java.canExecute()) {
            println("SKIP GitNoProcessFallbackTest.testRuntimeRepositoryUseNeverInvokesAGitExecutable: needs /bin/sh and a java launcher")
            return
        }
        withTempDir("forkmesh-g1-gate-") { scratch ->
            val sentinelDirectory = scratch.child("no-git-bin").also { it.mkdir() }
            val sentinel = scratch.child("git-sentinel")
            val fakeGit = sentinelDirectory.child("git")
            fakeGit.writeText("#!/bin/sh\ntouch \"\$FORKMESH_G1_GIT_SENTINEL\"\nexit 97\n")
            expect(fakeGit.setExecutable(true, false), "chmod 755 the sentinel git")

            val result = runProcess(
                listOf(
                    java.path,
                    "-cp",
                    System.getProperty("java.class.path"),
                    GitRuntimeNoProcessProbe::class.java.name,
                ),
                environment = mapOf(
                    "PATH" to "${sentinelDirectory.path}:/usr/bin:/bin",
                    "FORKMESH_G1_GIT_SENTINEL" to sentinel.path,
                ),
            )

            assertEq(0, result.exitCode, result.stderr)
            expect(result.stdoutText.contains("runtime-no-git-fallback=PROVED"), result.stdoutText)
            expect(!sentinel.exists(), "Git executable sentinel was invoked")
        }
    }
}

object GitRuntimeNoProcessProbe {
    @JvmStatic
    fun main(args: Array<String>) {
        val sentinel = System.getenv("FORKMESH_G1_GIT_SENTINEL")
        check(!sentinel.isNullOrEmpty()) { "FORKMESH_G1_GIT_SENTINEL is required" }
        withTempDir("forkmesh-g1-runtime-") { root ->
            val repository = GitRepository.init(GitRepositoryInit(root))
            val objectId = repository.writeBlob(byteArrayOf(111, 107, 10))
            repository.readBlob(objectId)
            check(!File(sentinel).exists()) { "Git executable sentinel was invoked" }
            println("runtime-no-git-fallback=PROVED")
        }
    }
}
