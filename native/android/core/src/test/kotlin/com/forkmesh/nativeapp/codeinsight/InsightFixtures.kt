package com.forkmesh.nativeapp.codeinsight

import java.io.File

class MemorySource(
    private val files: Map<String, ByteArray>,
    override val git: Boolean = true,
    private val log: List<InsightCommit>? = null,
    override val name: String = "fixture",
) : InsightSource {
    override val description: String = "memory:$name"
    override val head: String = "0000000000000000000000000000000000000000"
    override val branch: String = "main"

    override fun entries(): List<InsightEntry> = files.map { InsightEntry(it.key, it.value.size.toLong()) }

    override fun read(path: String, limit: Long): ByteArray? = files[path]

    override fun history(options: InsightOptions, isCancelled: () -> Boolean): List<InsightCommit>? = log
}

class FolderSource(private val root: File) : InsightSource {
    override val name: String = root.name
    override val description: String = root.path
    override val head: String = ""
    override val branch: String = ""
    override val git: Boolean = false

    override fun entries(): List<InsightEntry> = root.walkTopDown()
        .onEnter { it == root || !it.name.startsWith('.') }
        .filter { it.isFile }
        .map { InsightEntry(it.relativeTo(root).invariantSeparatorsPath, it.length()) }
        .toList()

    override fun read(path: String, limit: Long): ByteArray? {
        val file = File(root, path)
        if (!file.isFile || file.length() > limit) return null
        return file.readBytes()
    }

    override fun history(options: InsightOptions, isCancelled: () -> Boolean): List<InsightCommit>? = null
}

object InsightFixtures {
    val worktree: File by lazy {
        var dir: File? = File(System.getProperty("user.dir")).absoluteFile
        while (dir != null && !File(dir, "native/fixtures/insight/expected.json").isFile) dir = dir.parentFile
        dir ?: throw AssertionError("native/fixtures/insight not found above ${System.getProperty("user.dir")}")
    }

    val fixture: File get() = System.getenv("FORKMESH_INSIGHT_FIXTURE")?.let(::File) ?: File(worktree, "native/fixtures/insight")

    val repoFiles: Map<String, ByteArray> by lazy {
        val repo = File(fixture, "repo")
        repo.walkTopDown().filter { it.isFile }
            .associate { it.relativeTo(repo).invariantSeparatorsPath to it.readBytes() }
            .toSortedMap()
    }

    val historyLog: String by lazy { File(fixture, "history.log").readText(Charsets.UTF_8) }

    val expectedText: String by lazy { File(fixture, "expected.json").readText(Charsets.UTF_8) }

    const val NOW = 1792000000L

    fun snapshotOf(vararg files: Pair<String, String>): InsightSnapshot =
        InsightSnapshot(files = files.map { Insight.measureSource(it.first, it.second.toByteArray()) })
}
