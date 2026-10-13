package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.GitCancellationToken
import com.forkmesh.nativeapp.git.GitErrorCode
import com.forkmesh.nativeapp.git.GitException
import com.forkmesh.nativeapp.git.GitHttpRequest
import com.forkmesh.nativeapp.git.GitHttpResponse
import com.forkmesh.nativeapp.git.GitHttpTransport
import com.forkmesh.nativeapp.git.GitResourceLimits
import com.forkmesh.nativeapp.git.SystemGit
import com.forkmesh.nativeapp.git.runProcess
import com.forkmesh.nativeapp.git.transport.classifyHttpFailure
import java.io.File
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CopyOnWriteArrayList

class LocalGitSmartServer : GitHttpTransport {
    private val repositories = ConcurrentHashMap<String, File>()
    private val redirects = ConcurrentHashMap<String, String>()
    private val downHosts: MutableSet<String> = ConcurrentHashMap.newKeySet()
    val requests = CopyOnWriteArrayList<GitHttpRequest>()

    fun serve(base: String, bare: File) {
        repositories[base] = bare
    }

    fun redirect(base: String, target: String) {
        redirects[base] = target
    }

    fun takeDown(host: String) {
        downHosts.add(host)
    }

    val fetchBodies: List<String>
        get() = requests.filter { it.method == "POST" }
            .map { String(it.body, Charsets.ISO_8859_1) }
            .filter { it.contains("command=fetch") }

    fun requested(host: String): List<GitHttpRequest> = requests.filter { it.url.host == host }

    override fun send(
        request: GitHttpRequest,
        cancellation: GitCancellationToken?,
        limits: GitResourceLimits?,
    ): GitHttpResponse {
        cancellation?.throwIfCancelled()
        requests.add(request)
        if (downHosts.contains(request.url.host)) {
            throw GitException(GitErrorCode.networkUnavailable, "connection failed: host is down")
        }
        val text = request.url.toString()
        val base = (repositories.keys + redirects.keys).filter { text.startsWith("$it/") }.maxByOrNull { it.length }
            ?: throw classifyHttpFailure(request, 404)
        val suffix = text.substring(base.length)
        redirects[base]?.let { target ->
            return GitHttpResponse(statusCode = 307, headers = mapOf("location" to target + suffix), body = ByteArray(0))
        }
        val directory = repositories[base] ?: throw classifyHttpFailure(request, 404)
        val response = respond(request, suffix, directory)
        if (response.body.size > request.maxResponseBytes) {
            throw GitException(GitErrorCode.resourceLimitExceeded, "response body exceeds the configured limit")
        }
        return response
    }

    override fun close() {}

    private fun respond(request: GitHttpRequest, suffix: String, directory: File): GitHttpResponse {
        val path = directory.path
        return when (request.method to suffix) {
            "GET" to "/info/refs?service=git-upload-pack" -> ok(
                "application/x-git-upload-pack-advertisement",
                run(listOf("upload-pack", "--stateless-rpc", "--http-backend-info-refs", path), protocolV2 = true),
            )
            "POST" to "/git-upload-pack" -> ok(
                "application/x-git-upload-pack-result",
                run(listOf("upload-pack", "--stateless-rpc", path), request.body, protocolV2 = true),
            )
            "GET" to "/info/refs?service=git-receive-pack" -> ok(
                "application/x-git-receive-pack-advertisement",
                "001f# service=git-receive-pack\n0000".toByteArray(Charsets.US_ASCII) +
                    run(listOf("receive-pack", "--stateless-rpc", "--http-backend-info-refs", path)),
            )
            "POST" to "/git-receive-pack" -> ok(
                "application/x-git-receive-pack-result",
                run(listOf("receive-pack", "--stateless-rpc", path), request.body),
            )
            else -> throw classifyHttpFailure(request, 404)
        }
    }

    private fun ok(contentType: String, body: ByteArray) =
        GitHttpResponse(statusCode = 200, headers = mapOf("content-type" to contentType), body = body)

    private fun run(arguments: List<String>, input: ByteArray? = null, protocolV2: Boolean = false): ByteArray {
        val environment = HashMap(SystemGit.environment)
        if (protocolV2) environment["GIT_PROTOCOL"] = "version=2"
        val result = runProcess(
            listOf(SystemGit.executable!!) + arguments,
            stdin = input,
            environment = environment,
            removeEnvironment = SystemGit.removedEnvironment,
        )
        if (result.exitCode != 0) throw IllegalStateException("git ${arguments.first()} failed: ${result.stderr}")
        return result.stdout
    }
}

class LocalGitOrigin private constructor(val work: File, val bare: File) {
    fun commit(file: String, text: String, message: String): String {
        File(work, file).writeText(text)
        SystemGit.git(work, "add", file)
        SystemGit.git(work, "-c", "user.name=Origin", "-c", "user.email=origin@example.test", "commit", "-q", "-m", message)
        return revParse("HEAD")
    }

    fun branch(name: String, at: String) {
        SystemGit.git(work, "branch", "-f", name, at)
    }

    fun publish(vararg refs: String) {
        SystemGit.git(work, "push", "-q", "-f", bare.path, *refs)
    }

    fun tip(ref: String): String = SystemGit.git(bare, "rev-parse", ref)

    fun revParse(revision: String): String = SystemGit.git(work, "rev-parse", revision)

    companion object {
        fun make(root: File, commits: Int): LocalGitOrigin {
            val work = File(root, "work")
            val bare = File(root, "origin.git")
            SystemGit.git(root, "init", "-q", "-b", "main", work.path)
            val origin = LocalGitOrigin(work, bare)
            for (index in 1..maxOf(commits, 1)) origin.commit("step.txt", "step $index\n", "Step $index")
            SystemGit.git(root, "init", "-q", "--bare", "-b", "main", bare.path)
            SystemGit.git(bare, "config", "receive.denyNonFastForwards", "true")
            SystemGit.git(work, "push", "-q", bare.path, "main")
            return origin
        }
    }
}
