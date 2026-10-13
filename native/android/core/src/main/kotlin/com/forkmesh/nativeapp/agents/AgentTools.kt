package com.forkmesh.nativeapp.agents

import com.forkmesh.nativeapp.localrepos.GitAuthor
import com.forkmesh.nativeapp.localrepos.GitWorkspace
import com.forkmesh.nativeapp.localrepos.GitWorkspaceException
import com.forkmesh.nativeapp.localrepos.LocalChange
import com.forkmesh.nativeapp.localrepos.LocalFileContent
import com.forkmesh.nativeapp.localrepos.LocalFileKind

class AgentTool(
    val name: String,
    val description: String,
    val schema: Map<String, Any?>,
    val mutatesRepository: Boolean = false,
    val run: (Map<String, Any?>) -> String,
)

data class AgentToolResult(val callId: String, val name: String, val output: String, val isError: Boolean = false)

internal fun splitLines(text: String): List<String> {
    if (text.isEmpty()) return emptyList()
    val lines = ArrayList<String>()
    var start = 0
    var i = 0
    while (i < text.length) {
        val ch = text[i]
        if (ch == '\n' || ch == '\r') {
            lines.add(text.substring(start, i))
            if (ch == '\r' && i + 1 < text.length && text[i + 1] == '\n') i++
            start = i + 1
        }
        i++
    }
    if (start < text.length) lines.add(text.substring(start))
    return lines
}

class AgentToolset(val workspace: GitWorkspace, val readOnly: Boolean = false) {
    companion object {
        const val MAX_TOOL_OUTPUT_CHARS = 24000
        const val MAX_GREP_MATCHES = 80
        const val MAX_GREP_FILES = 2000

        private fun obj(properties: Map<String, Any?>, required: List<String> = emptyList()): Map<String, Any?> {
            val schema = linkedMapOf<String, Any?>("type" to "object", "properties" to properties)
            if (required.isNotEmpty()) schema["required"] = required
            return schema
        }

        private fun string(description: String): Map<String, Any?> = linkedMapOf("type" to "string", "description" to description)

        private fun requireString(args: Map<String, Any?>, key: String): String =
            args[key] as? String ?: throw IllegalArgumentException("\"$key\" is required and must be a string.")

        private fun optionalString(args: Map<String, Any?>, key: String): String? = args[key] as? String

        fun truncate(value: String): String {
            if (value.length <= MAX_TOOL_OUTPUT_CHARS) return value
            return value.substring(0, MAX_TOOL_OUTPUT_CHARS) + "\n… output truncated at $MAX_TOOL_OUTPUT_CHARS characters."
        }
    }

    @Volatile
    var author: GitAuthor = GitAuthor("forkmesh", "forkmesh@mobile.forkmesh")

    val tools: List<AgentTool> = listOf(
        AgentTool(
            "read_file",
            "Read a UTF-8 file from the repository. Returns the file with 1-based line numbers so you can refer to lines precisely.",
            obj(mapOf("path" to string("Repository-relative path, e.g. lib/main.dart")), listOf("path")),
        ) { readFile(requireString(it, "path")) },
        AgentTool(
            "write_file",
            "Create a file, or replace one entirely. Prefer replace_text for edits to an existing file; a whole-file write of a large file is easy to get wrong.",
            obj(
                linkedMapOf("path" to string("Repository-relative path"), "content" to string("The complete new file contents")),
                listOf("path", "content"),
            ),
            mutatesRepository = true,
        ) { writeFile(requireString(it, "path"), requireString(it, "content")) },
        AgentTool(
            "replace_text",
            "Replace an exact snippet in a file. The snippet must appear exactly once — include surrounding lines to make it unique. This is the preferred way to edit an existing file.",
            obj(
                linkedMapOf(
                    "path" to string("Repository-relative path"),
                    "old" to string("Exact text to replace; must match once"),
                    "new" to string("Replacement text"),
                ),
                listOf("path", "old", "new"),
            ),
            mutatesRepository = true,
        ) { replaceText(requireString(it, "path"), requireString(it, "old"), requireString(it, "new")) },
        AgentTool(
            "list_directory",
            "List the direct children of a directory. Use \"\" or \".\" for the repository root.",
            obj(mapOf("path" to string("Directory path, default the repository root"))),
        ) { listDirectory(optionalString(it, "path") ?: "") },
        AgentTool(
            "grep",
            "Search file contents across the repository and return matching lines with their paths. Use this to locate code before reading whole files.",
            obj(
                linkedMapOf(
                    "pattern" to string("Text or regular expression to find"),
                    "path" to string("Directory to search under, default the whole repo"),
                    "regex" to linkedMapOf("type" to "boolean", "description" to "Treat pattern as a regular expression (default true)"),
                ),
                listOf("pattern"),
            ),
        ) { grep(requireString(it, "pattern"), optionalString(it, "path") ?: "", it["regex"] as? Boolean ?: true) },
        AgentTool(
            "git_status",
            "Show the current branch and which files are staged, modified, or untracked.",
            obj(emptyMap()),
        ) { gitStatus() },
        AgentTool(
            "git_commit",
            "Stage every change and commit it. Call this once the requested change is complete and you have reviewed it.",
            obj(mapOf("message" to string("Commit message; a subject line, then why")), listOf("message")),
            mutatesRepository = true,
        ) { gitCommit(requireString(it, "message")) },
    )

    private val byName: Map<String, AgentTool> = tools.associateBy { it.name }

    fun invoke(callId: String, name: String, args: Map<String, Any?>): AgentToolResult {
        val tool = byName[name]
            ?: return AgentToolResult(callId, name, "No tool named \"$name\". Available: ${byName.keys.joinToString(", ")}.", true)
        if (readOnly && tool.mutatesRepository) {
            return AgentToolResult(
                callId,
                name,
                "This session is read-only, so $name did not run. Describe the change you would make instead.",
                true,
            )
        }
        return try {
            AgentToolResult(callId, name, truncate(tool.run(args)))
        } catch (error: GitWorkspaceException) {
            AgentToolResult(callId, name, "$name failed: ${error.message}", true)
        } catch (error: Exception) {
            AgentToolResult(callId, name, "$name failed: ${error.message ?: error}", true)
        }
    }

    private fun readFile(path: String): String {
        val file = workspace.readFile(path)
        if (file.isBinary) return "$path is a binary file (${file.bytes} bytes); it cannot be read as text."
        if (file.isTooLarge) {
            return "$path is ${file.bytes} bytes, too large to read in full. Use grep to find the part you need."
        }
        val lines = splitLines(file.text)
        if (lines.isEmpty()) return "$path is empty."
        val width = lines.size.toString().length
        val out = StringBuilder()
        for ((i, line) in lines.withIndex()) {
            out.append((i + 1).toString().padStart(width)).append("  ").append(line).append('\n')
        }
        return out.toString()
    }

    private fun writeFile(path: String, content: String): String {
        workspace.writeFile(path, content)
        return "Wrote $path (${splitLines(content).size} lines)."
    }

    private fun replaceText(path: String, oldText: String, newText: String): String {
        if (oldText.isEmpty()) return "The \"old\" snippet is empty. Use write_file to create a file."
        val file = workspace.readFile(path)
        if (!file.isEditable) return "$path cannot be edited as text."
        var occurrences = 0
        var from = 0
        while (true) {
            val at = file.text.indexOf(oldText, from)
            if (at < 0) break
            occurrences++
            from = at + oldText.length
        }
        if (occurrences == 0) {
            return "That snippet does not appear in $path. Read the file and copy the exact text, including indentation."
        }
        if (occurrences > 1) {
            return "That snippet appears $occurrences times in $path. Include more surrounding lines so it matches exactly once."
        }
        workspace.writeFile(path, file.text.replaceFirst(oldText, newText))
        return "Edited $path."
    }

    private fun listDirectory(path: String): String {
        val normalized = (if (path == ".") "" else path).trim()
        val entries = workspace.listFiles(normalized)
        if (entries.isEmpty()) return if (normalized.isEmpty()) "The repository is empty." else "$normalized is empty."
        val sorted = entries.sortedWith(compareBy({ !it.isDirectory }, { it.name }))
        return sorted.joinToString("\n") { entry ->
            if (entry.isDirectory) "${entry.path}/"
            else "${entry.path}  (${entry.size} bytes${if (entry.tracked) "" else ", untracked"})"
        }
    }

    private fun grep(pattern: String, path: String, useRegex: Boolean): String {
        val matcher = try {
            Regex(if (useRegex) pattern else Regex.escape(pattern))
        } catch (error: Exception) {
            return "That is not a valid regular expression: ${error.message}"
        }
        val files = walkFiles(if (path == ".") "" else path)
        val hits = ArrayList<String>()
        var scanned = 0
        for (file in files) {
            if (hits.size >= MAX_GREP_MATCHES) break
            scanned++
            val content: LocalFileContent = try {
                workspace.readFile(file)
            } catch (_: Exception) {
                continue
            }
            if (!content.isEditable) continue
            val lines = splitLines(content.text)
            for ((i, line) in lines.withIndex()) {
                if (!matcher.containsMatchIn(line)) continue
                hits.add("$file:${i + 1}: ${line.trim()}")
                if (hits.size >= MAX_GREP_MATCHES) break
            }
        }
        if (hits.isEmpty()) return "No matches for \"$pattern\" in $scanned files."
        val capped = if (hits.size >= MAX_GREP_MATCHES) "\n… more matches were not listed; narrow the pattern or path." else ""
        return hits.joinToString("\n") + capped
    }

    private fun walkFiles(root: String): List<String> {
        val found = ArrayList<String>()
        val queue = ArrayDeque<String>().apply { add(root) }
        while (queue.isNotEmpty() && found.size < MAX_GREP_FILES) {
            val directory = queue.removeFirst()
            val entries = try {
                workspace.listFiles(directory)
            } catch (_: Exception) {
                continue
            }
            for (entry in entries) {
                if (entry.kind == LocalFileKind.symlink) continue
                if (entry.isDirectory) queue.add(entry.path)
                else if (found.size < MAX_GREP_FILES) found.add(entry.path)
            }
        }
        return found
    }

    private fun gitStatus(): String {
        val status = workspace.status()
        val current = workspace.branches().filter { it.isCurrent }.joinToString("") { it.name }
        val head = if (current.isEmpty()) "no branch yet" else "on $current"
        if (status.isClean) return "Clean worktree, $head."
        val out = StringBuilder("Worktree $head.\n")
        fun section(label: String, changes: List<LocalChange>) {
            if (changes.isEmpty()) return
            out.append(label).append(":\n")
            for (change in changes) out.append("  ").append(change.kind.name).append(' ').append(change.path).append('\n')
        }
        section("Staged", status.staged)
        section("Modified", status.unstaged)
        section("Untracked", status.untracked)
        return out.toString().trimEnd()
    }

    private fun gitCommit(message: String): String {
        val trimmed = message.trim()
        if (trimmed.isEmpty()) return "A commit needs a message."
        if (workspace.status().isClean) return "Nothing to commit; the worktree is clean."
        workspace.stageAll()
        val id = workspace.commit(trimmed, author)
        return "Committed ${if (id.length >= 8) id.substring(0, 8) else id}: ${trimmed.split('\n').first()}"
    }
}
