package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.expect
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.concurrent.TimeUnit

class ProcessResult(val exitCode: Int, val stdout: ByteArray, val stderr: String) {
    val stdoutText: String get() = utf8Text(stdout)
}

fun runProcess(
    command: List<String>,
    stdin: ByteArray? = null,
    environment: Map<String, String> = emptyMap(),
    removeEnvironment: Collection<String> = emptyList(),
    directory: File? = null,
): ProcessResult {
    val builder = ProcessBuilder(command)
    if (directory != null) builder.directory(directory)
    val env = builder.environment()
    for (name in removeEnvironment) env.remove(name)
    env.putAll(environment)
    val process = builder.start()
    val stdout = ByteArrayOutputStream()
    val stderr = ByteArrayOutputStream()
    val stdoutReader = Thread { process.inputStream.use { it.copyTo(stdout) } }.also { it.start() }
    val stderrReader = Thread { process.errorStream.use { it.copyTo(stderr) } }.also { it.start() }
    process.outputStream.use { input -> if (stdin != null) input.write(stdin) }
    if (!process.waitFor(300, TimeUnit.SECONDS)) {
        process.destroyForcibly()
        throw AssertionError("process timed out: ${command.joinToString(" ")}")
    }
    stdoutReader.join()
    stderrReader.join()
    return ProcessResult(process.exitValue(), stdout.toByteArray(), stderr.toString(Charsets.UTF_8.name()))
}

object SystemGit {
    val executable: String? by lazy {
        val configured = System.getenv("FORKMESH_TEST_SYSTEM_GIT")
        if (configured != null && configured.startsWith("/") && File(configured).canExecute()) {
            return@lazy configured
        }
        for (directory in (System.getenv("PATH") ?: "").split(File.pathSeparator)) {
            if (directory.isEmpty()) continue
            val candidate = File(directory, "git")
            if (candidate.isFile && candidate.canExecute()) return@lazy candidate.absolutePath
        }
        null
    }

    val environment: Map<String, String> = mapOf(
        "GIT_CONFIG_NOSYSTEM" to "1",
        "GIT_CONFIG_GLOBAL" to "/dev/null",
        "GIT_TERMINAL_PROMPT" to "0",
    )

    val removedEnvironment = listOf("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_OBJECT_DIRECTORY")

    fun skipped(test: String): Boolean {
        if (executable != null) return false
        println("SKIP $test: no system git (set FORKMESH_TEST_SYSTEM_GIT or put git on PATH)")
        return true
    }

    fun run(arguments: List<String>, stdin: ByteArray? = null, directory: File? = null): ProcessResult =
        runProcess(
            listOf(executable!!) + arguments,
            stdin = stdin,
            environment = environment,
            removeEnvironment = removedEnvironment,
            directory = directory,
        )

    fun git(root: File, vararg arguments: String): String {
        val result = run(listOf("-C", root.path) + arguments)
        expect(result.exitCode == 0, "git ${arguments.joinToString(" ")}: ${result.stderr}")
        return result.stdoutText.trim()
    }
}
