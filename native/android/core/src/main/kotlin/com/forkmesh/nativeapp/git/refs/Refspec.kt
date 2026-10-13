package com.forkmesh.nativeapp.git.refs

import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException

class GitFetchRefspec private constructor(
    val source: String,
    val destination: String,
    val force: Boolean,
) {
    val isWildcard: Boolean get() = source.endsWith("/*")

    fun destinationFor(remoteRef: String): String? {
        if (!isWildcard) return if (remoteRef == source) destination else null
        val prefix = source.substring(0, source.length - 1)
        if (!remoteRef.startsWith(prefix)) return null
        val tail = remoteRef.substring(prefix.length)
        if (tail.isEmpty()) return null
        return destination.substring(0, destination.length - 1) + tail
    }

    val value: String get() = "${if (force) "+" else ""}$source:$destination"

    companion object {
        fun parse(value: String): GitFetchRefspec {
            var text = value
            var force = false
            if (text.startsWith("+")) {
                force = true
                text = text.substring(1)
            }
            val separator = text.indexOf(':')
            if (separator <= 0 || separator != text.lastIndexOf(':')) {
                throw GitException(GitErrorCode.invalidRef, "fetch refspec must have one source and one destination")
            }
            val source = text.substring(0, separator)
            val destination = text.substring(separator + 1)
            val sourceWildcard = source.endsWith("/*")
            val destinationWildcard = destination.endsWith("/*")
            if (sourceWildcard != destinationWildcard ||
                !source.startsWith("refs/heads/") ||
                !destination.startsWith("refs/remotes/") ||
                source.count { it == '*' } > 1 ||
                destination.count { it == '*' } > 1
            ) {
                throw GitException(GitErrorCode.invalidRef, "fetch refspec must map refs/heads to refs/remotes")
            }
            val sourceBase = if (sourceWildcard) source.substring(0, source.length - 2) else source
            val destinationBase =
                if (destinationWildcard) destination.substring(0, destination.length - 2) else destination
            GitRefName.parse(if (sourceWildcard) "$sourceBase/x" else sourceBase)
            GitRefName.parse(if (destinationWildcard) "$destinationBase/x" else destinationBase)
            if (destinationWildcard &&
                (destinationBase.length <= "refs/remotes/".length ||
                    destinationBase.substring("refs/remotes/".length).contains("/"))
            ) {
                throw GitException(
                    GitErrorCode.invalidRef,
                    "fetch refspec destination must be refs/remotes/<remote>/*",
                )
            }
            return GitFetchRefspec(source, destination, force)
        }

        fun standard(remote: String): GitFetchRefspec {
            GitRemoteName.validate(remote)
            return GitFetchRefspec("refs/heads/*", "refs/remotes/$remote/*", true)
        }
    }
}

object GitRemoteName {
    private val pattern = Regex("[A-Za-z0-9][A-Za-z0-9._-]{0,63}")

    fun validate(name: String) {
        if (!pattern.matches(name) || name.contains("..") || name.endsWith(".lock")) {
            throw GitException(GitErrorCode.invalidRef, "remote name is invalid")
        }
    }
}

class GitPushRefspec(localBranch: String, remoteBranch: String? = null) {
    val local: GitRefName = GitRefName.branch("refs/heads/$localBranch")
    val remote: GitRefName = GitRefName.branch("refs/heads/${remoteBranch ?: localBranch}")

    val localBranch: String get() = local.value.substring(11)
    val remoteBranch: String get() = remote.value.substring(11)

    companion object {
        fun parse(value: String): GitPushRefspec {
            if (value.startsWith("+") || value.startsWith(":")) {
                throw GitException(GitErrorCode.invalidRef, "push refspec may not force or delete")
            }
            val separator = value.indexOf(':')
            val source = if (separator == -1) value else value.substring(0, separator)
            val destination = if (separator == -1) value else value.substring(separator + 1)
            if (source.isEmpty() || destination.isEmpty() || source.contains("*") || destination.contains("*")) {
                throw GitException(
                    GitErrorCode.invalidRef,
                    "push refspec must name one local and one remote branch",
                )
            }
            return GitPushRefspec(localBranch = stripHeads(source), remoteBranch = stripHeads(destination))
        }

        private fun stripHeads(value: String): String =
            if (value.startsWith("refs/heads/")) value.substring(11) else value
    }
}
