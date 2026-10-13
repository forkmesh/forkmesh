package com.forkmesh.nativeapp.git.repository

import com.forkmesh.nativeapp.git.bytes.DartFormatException
import com.forkmesh.nativeapp.git.bytes.dartSplitLines
import com.forkmesh.nativeapp.git.bytes.dartTrimLeft
import com.forkmesh.nativeapp.git.bytes.utf8Decode
import com.forkmesh.nativeapp.git.bytes.utf8Encode
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.refs.GitFetchRefspec
import com.forkmesh.nativeapp.git.refs.GitRefName
import com.forkmesh.nativeapp.git.refs.GitRemoteName
import com.forkmesh.nativeapp.git.transport.GitRemoteUrl
import com.forkmesh.nativeapp.git.worktree.createFileExclusive
import com.forkmesh.nativeapp.git.worktree.fileExists
import com.forkmesh.nativeapp.git.worktree.renameFile
import com.forkmesh.nativeapp.git.worktree.writeBytesFlushed
import java.io.File

class GitRemoteConfig(val name: String, val url: GitRemoteUrl, val fetch: GitFetchRefspec)

class GitBranchConfig(val name: String, val remote: String, val merge: GitRefName)

class RepositoryConfig(
    val fileMode: Boolean,
    remotes: Map<String, GitRemoteConfig> = emptyMap(),
    branches: Map<String, GitBranchConfig> = emptyMap(),
) {
    val remotes: Map<String, GitRemoteConfig> = LinkedHashMap(remotes)
    val branches: Map<String, GitBranchConfig> = LinkedHashMap(branches)

    companion object {
        private val sectionHeader = Regex("""\[([A-Za-z][A-Za-z0-9-]*)(?:[ \t]+"([^"\\\x00-\x1f]*)")?\]""")

        private val keyLine = Regex("""([A-Za-z][A-Za-z0-9-]*)[ \t]*=[ \t]*([^\n\r\u2028\u2029]*)""")

        fun load(file: File): RepositoryConfig = parse(file.readBytes())

        fun parse(bytes: ByteArray): RepositoryConfig {
            val remoteValues = LinkedHashMap<String, LinkedHashMap<String, MutableList<String>>>()
            val branchValues = LinkedHashMap<String, LinkedHashMap<String, MutableList<String>>>()
            var fileMode = true
            for (entry in entries(bytes)) {
                val section = entry.section
                val subsection = entry.subsection
                val key = entry.key
                val value = entry.value
                if (section == "core" && key == "repositoryformatversion" && value != "0") {
                    throw GitException(GitErrorCode.unsupportedRepositoryFormat, "repository format is unsupported")
                }
                if (section == "core" && key == "bare" && value != "false") {
                    throw GitException(GitErrorCode.unsupportedRepositoryFormat, "bare repository is unsupported")
                }
                if (section == "core" && key == "worktree") {
                    throw GitException(GitErrorCode.unsupportedRepositoryFormat, "external worktree is unsupported")
                }
                if (section == "extensions" && key == "objectformat" && value != "sha1") {
                    throw GitException(GitErrorCode.unsupportedRepositoryFormat, "object format is unsupported")
                }
                if (section == "core" && key == "filemode") {
                    if (value != "true" && value != "false") {
                        throw GitException(GitErrorCode.unsupportedRepositoryFormat, "core.filemode is invalid")
                    }
                    fileMode = value == "true"
                }
                if (section == "remote" && subsection != null) {
                    remoteValues.getOrPut(subsection) { LinkedHashMap() }.getOrPut(key) { ArrayList() }.add(value)
                }
                if (section == "branch" && subsection != null) {
                    branchValues.getOrPut(subsection) { LinkedHashMap() }.getOrPut(key) { ArrayList() }.add(value)
                }
            }
            val remotes = LinkedHashMap<String, GitRemoteConfig>()
            for ((name, values) in remoteValues) {
                GitRemoteName.validate(name)
                val urls = values["url"] ?: emptyList<String>()
                val fetches = values["fetch"] ?: emptyList<String>()
                if (urls.size != 1 || fetches.size > 1) {
                    throw GitException(
                        GitErrorCode.unsupportedRepositoryFormat,
                        "remote must have one url and at most one fetch refspec",
                    )
                }
                val fetch = if (fetches.isEmpty()) GitFetchRefspec.standard(name) else GitFetchRefspec.parse(fetches.single())
                if (fetch.isWildcard && fetch.destination != "refs/remotes/$name/*") {
                    throw GitException(
                        GitErrorCode.unsupportedRepositoryFormat,
                        "remote fetch refspec must target its own remote namespace",
                    )
                }
                remotes[name] = GitRemoteConfig(name = name, url = GitRemoteUrl.parse(urls.single()), fetch = fetch)
            }
            val branches = LinkedHashMap<String, GitBranchConfig>()
            for ((name, values) in branchValues) {
                val remoteNames = values["remote"] ?: emptyList<String>()
                val merges = values["merge"] ?: emptyList<String>()
                if (remoteNames.isEmpty() && merges.isEmpty()) continue
                if (remoteNames.size != 1 || merges.size != 1) {
                    throw GitException(
                        GitErrorCode.unsupportedRepositoryFormat,
                        "branch must have one remote and one merge ref",
                    )
                }
                GitRefName.branch("refs/heads/$name")
                GitRemoteName.validate(remoteNames.single())
                branches[name] = GitBranchConfig(
                    name = name,
                    remote = remoteNames.single(),
                    merge = GitRefName.branch(merges.single()),
                )
            }
            return RepositoryConfig(fileMode = fileMode, remotes = remotes, branches = branches)
        }

        fun writeRemote(file: File, name: String, url: GitRemoteUrl, fetch: GitFetchRefspec) {
            GitRemoteName.validate(name)
            replaceSection(file, "remote", name, linkedMapOf("url" to url.uri.toString(), "fetch" to fetch.value))
        }

        fun writeBranchUpstream(file: File, name: String, remote: String, merge: GitRefName) {
            GitRefName.branch("refs/heads/$name")
            GitRemoteName.validate(remote)
            replaceSection(file, "branch", name, linkedMapOf("remote" to remote, "merge" to merge.value))
        }

        private fun replaceSection(file: File, section: String, subsection: String, values: Map<String, String>) {
            if (subsection.contains("\"") || subsection.contains("\\") || subsection.contains("\n")) {
                throw GitException(
                    GitErrorCode.unsupportedRepositoryFormat,
                    "config subsection name needs quoting that is unsupported",
                )
            }
            val original = if (fileExists(file)) file.readBytes() else ByteArray(0)
            parse(original)
            val lines = dartSplitLines(utf8Decode(original))
            val kept = ArrayList<String>()
            var skipping = false
            for (line in lines) {
                val header = sectionHeader.matchEntire(line.dartTrimLeft())
                if (header != null) {
                    skipping = header.groupValues[1].lowercase() == section && header.groups[2]?.value == subsection
                    if (skipping) continue
                }
                if (!skipping) kept.add(line)
            }
            while (kept.isNotEmpty() && kept.last().isEmpty()) {
                kept.removeAt(kept.size - 1)
            }
            val buffer = StringBuilder()
            for (line in kept) {
                buffer.append(line).append('\n')
            }
            buffer.append("[$section \"$subsection\"]\n")
            for ((key, value) in values) {
                if (value.contains("\n") || value.contains("\r")) {
                    throw GitException(GitErrorCode.unsupportedRepositoryFormat, "config value contains a line break")
                }
                buffer.append('\t').append(key).append(" = ").append(value).append('\n')
            }
            val bytes = utf8Encode(buffer.toString())
            parse(bytes)
            val lock = File("${file.path}.lock")
            if (!createFileExclusive(lock)) {
                throw GitException(GitErrorCode.refLocked, "config lock already exists")
            }
            try {
                writeBytesFlushed(lock, bytes)
                renameFile(lock, file)
            } finally {
                if (fileExists(lock)) lock.delete()
            }
        }

        private fun entries(bytes: ByteArray): Sequence<ConfigEntry> = sequence {
            if (bytes.size > 1024 * 1024) {
                throw GitException(GitErrorCode.resourceLimitExceeded, "config exceeds one MiB")
            }
            val text = try {
                utf8Decode(bytes)
            } catch (error: DartFormatException) {
                throw GitException(GitErrorCode.unsupportedRepositoryFormat, "config is not valid UTF-8")
            }
            var section: String? = null
            var subsection: String? = null
            for (line in dartSplitLines(text)) {
                val trimmed = line.dartTrimLeft()
                if (trimmed.isEmpty() || trimmed.startsWith("#") || trimmed.startsWith(";")) {
                    continue
                }
                if (trimmed.startsWith("[")) {
                    val sectionMatch = sectionHeader.matchEntire(trimmed)
                        ?: throw GitException(
                            GitErrorCode.unsupportedRepositoryFormat,
                            "config section header is malformed or uses unsupported quoting",
                        )
                    val name = sectionMatch.groupValues[1].lowercase()
                    section = name
                    subsection = sectionMatch.groups[2]?.value
                    if (name == "include" || name == "includeif") {
                        throw GitException(GitErrorCode.unsupportedRepositoryFormat, "config includes are unsupported")
                    }
                    if (name == "alias" || name == "credential" || name == "url") {
                        throw GitException(GitErrorCode.unsupportedRepositoryFormat, "config section is unsupported")
                    }
                    continue
                }
                val keyMatch = keyLine.matchEntire(trimmed)
                if (section == null || keyMatch == null) {
                    throw GitException(GitErrorCode.unsupportedRepositoryFormat, "config line is invalid")
                }
                val key = keyMatch.groupValues[1].lowercase()
                val value = keyMatch.groupValues[2]
                if (value.endsWith("\\") || value.startsWith("\"") || value.contains("\\\"")) {
                    throw GitException(
                        GitErrorCode.unsupportedRepositoryFormat,
                        "config value quoting or continuation is unsupported",
                    )
                }
                if (key.contains("hook") ||
                    key.contains("helper") ||
                    key.contains("command") ||
                    key == "external" ||
                    key == "driver" ||
                    key == "pushurl" ||
                    key == "proxy" ||
                    key == "sslcainfo" ||
                    key == "sslcert" ||
                    key == "sslkey"
                ) {
                    throw GitException(
                        GitErrorCode.unsupportedRepositoryFormat,
                        "executable or transport-overriding config is unsupported",
                    )
                }
                yield(ConfigEntry(section, subsection, key, value))
            }
        }
    }

    private class ConfigEntry(val section: String, val subsection: String?, val key: String, val value: String)
}
