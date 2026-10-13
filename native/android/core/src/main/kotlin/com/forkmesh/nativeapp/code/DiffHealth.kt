package com.forkmesh.nativeapp.code

import com.forkmesh.nativeapp.code.diff.DiffFile
import com.forkmesh.nativeapp.code.diff.DiffFileStatus
import com.forkmesh.nativeapp.code.diff.DiffLineKind

enum class DiffLineFlag(val label: String, val severe: Boolean) {
    Secret("secret", true),
    TlsBypass("TLS bypass", true),
    UnsafeApi("unsafe call", true),
    Permissive("permissive", true),
    InsecureUrl("plain HTTP", true),
    ConflictMarker("conflict marker", true),
    Todo("TODO", false),
    Debug("debug", false),
    BroadCatch("broad catch", false),
    LongLine("long line", false),
    TrailingWhitespace("trailing space", false),
    DeepNesting("deep nesting", false),
    Request("network call", false),
    ;

    val metric: String
        get() = when (this) {
            Secret, TlsBypass, UnsafeApi, Permissive -> "security"
            InsecureUrl, Request -> "network"
            ConflictMarker, TrailingWhitespace -> "practices"
            Todo, Debug, BroadCatch, LongLine -> "quality"
            DeepNesting -> "complexity"
        }
}

data class DiffLineMark(val metric: String, val severe: Boolean) {
    companion object {
        val metricOrder = listOf("merge", "complexity", "quality", "security", "network", "practices")

        fun of(flags: List<DiffLineFlag>): List<DiffLineMark> {
            val marks = metricOrder.mapNotNull { key ->
                val hits = flags.filter { it.metric == key }
                if (hits.isEmpty()) null else DiffLineMark(key, hits.any { it.severe })
            }
            return marks.filter { it.severe } + marks.filterNot { it.severe }
        }
    }
}

enum class DiffHealthTone(val label: String) {
    Green("green"),
    Orange("orange"),
    Red("red"),
    ;

    companion object {
        fun of(score: Int): DiffHealthTone = if (score >= 85) Green else if (score >= 60) Orange else Red
    }
}

data class DiffHealthMetric(val key: String, val label: String, val score: Int, val detail: String) {
    val tone: DiffHealthTone get() = DiffHealthTone.of(score)
    val pill: String get() = "$label $score"
    val explanation: String get() = "$label: $score/100 · ${tone.label}\n$detail\n\nGreen 85–100 · orange 60–84 · red 0–59"
    val accessibility: String get() = "$label health score: $score out of 100, ${tone.label}"
}

sealed class DiffHealthGit {
    object Unknown : DiffHealthGit()

    object Merged : DiffHealthGit()

    data class State(val behind: Int, val ahead: Int, val mergeConflict: Boolean = false, val worktreeConflict: Boolean = false) : DiffHealthGit()
}

class DiffHealthLineStats {
    var controlFlowLines = 0
    var nestedControlFlowLines = 0
    var longLines = 0
    var todoLines = 0
    var debugLines = 0
    var broadCatchLines = 0
    var unsafeApiLines = 0
    var transportBypassLines = 0
    var permissiveSecurityLines = 0
    var requestLines = 0
    var hardcodedUrls = 0
    var insecureUrls = 0
    var conflictMarkerLines = 0
    var trailingWhitespaceLines = 0
    var secretMatches = 0
    var resilienceEvidence = false
}

object DiffHealth {
    private val codeExtensions = setOf(
        "c", "cc", "cpp", "cxx", "h", "hpp", "cs", "java", "kt", "go", "rs", "py", "rb", "js", "jsx", "ts", "tsx",
        "mjs", "cjs", "php", "swift", "scala", "sh", "bash", "sql", "dart", "ex", "exs", "vue", "svelte",
    )
    private val manifests = setOf(
        "package.json", "pyproject.toml", "requirements.txt", "cargo.toml", "go.mod", "pom.xml", "build.gradle",
        "build.gradle.kts", "gemfile", "composer.json", "pubspec.yaml", "mix.exs",
    )
    private val lockfiles = setOf(
        "package-lock.json", "pnpm-lock.yaml", "yarn.lock", "cargo.lock", "go.sum", "gemfile.lock", "composer.lock",
        "pubspec.lock", "pipfile.lock", "poetry.lock",
    )
    private val oddExtensions = setOf("exe", "dll", "dylib", "so", "a", "o", "db", "sqlite", "zip", "tar", "gz", "7z")

    private fun caseless(pattern: String) = Regex(pattern, RegexOption.IGNORE_CASE)

    private val controlFlow = caseless("""\b(?:if|elif|else\s+if|for|foreach|while|switch|case|catch|except|match|when)\b|&&|\|\|""")
    private val todoMarker = Regex("""\b(?:TODO|FIXME|HACK|XXX)\b""")
    private val debugCall = caseless("""\b(?:console\.(?:log|debug|trace)|debugger\s*;|qDebug\s*\(|System\.out\.print|var_dump\s*\(|dd\s*\()""")
    private val broadCatch = caseless("""(?:except\s*(?::|Exception\b)|catch\s*\([^)]*(?:Exception|Throwable|\.\.\.)[^)]*\))""")
    private val unsafeApi = caseless("""\b(?:eval|Function)\s*\(|\bexec\s*\(|\bsystem\s*\(|child_process\s*\.\s*exec|shell\s*=\s*True|dangerouslySetInnerHTML|\.innerHTML\s*=""")
    private val transportBypass = caseless("""verify\s*=\s*(?:false|0)|rejectUnauthorized\s*:\s*false|NODE_TLS_REJECT_UNAUTHORIZED\s*=\s*['"]?0|CURLOPT_SSL_VERIFYPEER\s*,\s*(?:0|false)|danger_accept_invalid_certs\s*\(\s*true""")
    private val permissiveSecurity = caseless("""Access-Control-Allow-Origin[^\n]*['"]\*['"]|\bcors\s*\(\s*\)|\bchmod\s+(?:-R\s+)?777\b""")
    private val requestSite = caseless("""\bfetch\s*\(|\baxios\s*(?:\.|\[)|\brequests\s*\.\s*(?:get|post|put|patch|delete|request)\s*\(|\b(?:http|https)\s*\.\s*(?:get|request)\s*\(|\bQNetwork(?:Request|AccessManager)\b|\bURLSession\b|\bWebSocket\s*\(|\bcurl_easy_perform\b|\bHttpClient\b""")
    private val resilience = caseless("""\b(?:timeout|AbortController|CancellationToken|retry|retries|backoff|cancell?ation)\b""")
    private val url = caseless("""https?://[^\s'"<>]+""")

    private val secretPatterns: List<Pair<String, Regex>> = listOf(
        "GitHub token" to Regex("""\bgh[pousr]_[A-Za-z0-9_]{36,}\b"""),
        "GitHub fine-grained PAT" to Regex("""\bgithub_pat_[A-Za-z0-9_]{22,}\b"""),
        "AWS access key" to Regex("""\bAKIA[0-9A-Z]{16,}\b"""),
        "AWS temporary access key" to Regex("""\bASIA[0-9A-Z]{16,}\b"""),
        "Slack token" to Regex("""\bxox[baprs]-[A-Za-z0-9-]{20,}\b"""),
        "OpenAI API key" to Regex("""\bsk-proj-[A-Za-z0-9_-]{20,}\b|\bsk-[A-Za-z0-9]{48,}\b"""),
        "Anthropic API key" to Regex("""\bsk-ant-[A-Za-z0-9_-]{20,}\b"""),
        "Stripe secret key" to Regex("""\bsk_(live|test)_[A-Za-z0-9]{24,}\b"""),
        "Stripe restricted key" to Regex("""\brk_(live|test)_[A-Za-z0-9]{24,}\b"""),
        "Google API key" to Regex("""\bAIza[A-Za-z0-9_-]{35,}\b"""),
        "Google OAuth token" to Regex("""\bya29\.[A-Za-z0-9_-]{20,}\b"""),
        "Google OAuth client secret" to Regex("""\bGOCSPX-[A-Za-z0-9_-]{28,}\b"""),
        "SendGrid API key" to Regex("""\bSG\.[A-Za-z0-9_-]{22,}\.[A-Za-z0-9_-]{43,}\b"""),
        "Twilio auth token" to Regex("""\bSK[a-f0-9]{32,}\b"""),
        "npm access token" to Regex("""\bnpm_[A-Za-z0-9]{36,}\b"""),
        "HashiCorp Vault token" to Regex("""\bhv[sbr]\.[A-Za-z0-9]{24,}\b"""),
        "Cloudflare API token" to Regex("""(?:CLOUDFLARE_API_TOKEN|CF_API_TOKEN)\s*=\s*['"]?([A-Za-z0-9_-]{40,})['"]?"""),
        "PEM private key" to Regex("""-----BEGIN (?:RSA |EC |DSA |OPENSSH |PGP |)PRIVATE KEY(?:-----| BLOCK-----)"""),
        "Secret/token assignment" to caseless("""(?:password|passwd|api[_\-]?(?:key|secret|token)|auth[_\-]?token|secret[_\-]?key|access[_\-]?token|private[_\-]?key)\s*[:=]\s*['"]([^'"\s]{20,})['"]"""),
    )
    private val secretFragments = listOf(
        "ghp_", "gho_", "ghu_", "ghs_", "ghr_", "github_pat_", "akia", "asia", "xoxb-", "xoxa-", "xoxp-", "xoxr-",
        "xoxs-", "sk-proj-", "sk-", "sk_live_", "sk_test_", "rk_live_", "rk_test_", "aiza", "ya29.", "gocspx-", "sg.",
        "npm_", "hvs.", "hvb.", "hvr.", "cloudflare_api_token", "cf_api_token", "-----begin ", "password", "passwd",
        "api_key", "api-key", "apikey", "api_secret", "api-secret", "apisecret", "api_token", "api-token", "apitoken",
        "auth_token", "auth-token", "authtoken", "secret_key", "secret-key", "secretkey", "access_token",
        "access-token", "accesstoken", "private_key", "private-key", "privatekey",
    )
    private val placeholderMarkers = listOf(
        "correct-horse-battery-staple", "changeme", "change-me", "change_me", "placeholder", "example", "sample",
        "dummy", "redacted", "notreal", "not-a-real", "not_a_real", "fake", "your-", "your_", "test-password",
        "test_password", "testpassword", "\${", "{{", "<",
    )
    private val indirection = Regex("""^(?:\$\w+|\$\{[^}]*\}|\$\([^)]*\)|%\w+%)$""")
    private const val IGNORE_MARKER = "forkmesh-secret-scan:ignore-line"

    private fun fileName(path: String): String = path.substringAfterLast('/')

    private fun suffix(path: String): String {
        val name = fileName(path)
        val dot = name.lastIndexOf('.')
        return if (dot < 0) "" else name.substring(dot + 1).lowercase()
    }

    fun isCode(path: String): Boolean = suffix(path) in codeExtensions

    fun isTest(path: String): Boolean {
        val lower = path.lowercase()
        if (lower.startsWith("test/") || lower.startsWith("tests/") || "/test/" in lower || "/tests/" in lower ||
            "/__tests__/" in lower || "/spec/" in lower
        ) {
            return true
        }
        val base = fileName(lower)
        return base.startsWith("test_") || "_test." in base || ".test." in base || ".spec." in base || "_spec." in base
    }

    fun isGenerated(path: String): Boolean {
        val lower = "/" + path.lowercase()
        return "/node_modules/" in lower || "/vendor/" in lower || "/dist/" in lower || "/generated/" in lower ||
            "/build/" in lower || lower.endsWith(".min.js") || lower.endsWith(".min.css") || ".generated." in lower
    }

    private fun isSensitive(file: DiffFile): Boolean {
        if (file.status != DiffFileStatus.Added) return false
        val base = fileName(file.path.lowercase())
        return base == ".env" || base == "id_rsa" || base == "id_ed25519" || suffix(file.path) in setOf("pem", "p12", "pfx", "key")
    }

    private fun isOddArtifact(file: DiffFile): Boolean = file.status == DiffFileStatus.Added && suffix(file.path) in oddExtensions

    private fun mayContainSecret(text: String): Boolean {
        if ("SK" in text) return true
        val lower = text.lowercase()
        return secretFragments.any { it in lower }
    }

    private fun looksLikePlaceholder(value: String): Boolean {
        val lower = value.lowercase()
        if (placeholderMarkers.any { it in lower }) return true
        if (indirection.containsMatchIn(value)) return true
        return lower.isNotEmpty() && lower.all { it == lower[0] }
    }

    fun secretMatches(text: String): List<String> {
        if (IGNORE_MARKER in text || !mayContainSecret(text)) return emptyList()
        val found = ArrayList<String>()
        for ((name, regex) in secretPatterns) {
            for (match in regex.findAll(text)) {
                if (name == "Secret/token assignment" && looksLikePlaceholder(match.groupValues.getOrElse(1) { "" })) continue
                found.add(name)
            }
        }
        return found
    }

    fun urls(text: String): Pair<Int, Int> {
        var all = 0
        var insecure = 0
        for (match in url.findAll(text)) {
            val found = match.value.lowercase()
            all++
            if (!found.startsWith("http://")) continue
            if (found.startsWith("http://localhost") || found.startsWith("http://127.0.0.1") || found.startsWith("http://0.0.0.0") ||
                found.startsWith("http://[::1]") || "example." in found || ".invalid" in found
            ) {
                continue
            }
            insecure++
        }
        return all to insecure
    }

    private fun isConflictMarker(text: String): Boolean {
        val trimmed = text.trim()
        return trimmed.startsWith("<<<<<<<") || trimmed.startsWith(">>>>>>>") || trimmed == "======="
    }

    private fun indent(text: String): Int {
        var count = 0
        while (count < text.length && text[count].isWhitespace()) count++
        return count
    }

    fun flags(path: String, added: String): List<DiffLineFlag> = buildList {
        if (secretMatches(added).isNotEmpty()) add(DiffLineFlag.Secret)
        if (transportBypass.containsMatchIn(added)) add(DiffLineFlag.TlsBypass)
        if (unsafeApi.containsMatchIn(added)) add(DiffLineFlag.UnsafeApi)
        if (permissiveSecurity.containsMatchIn(added)) add(DiffLineFlag.Permissive)
        if (urls(added).second > 0) add(DiffLineFlag.InsecureUrl)
        if (isConflictMarker(added)) add(DiffLineFlag.ConflictMarker)
        if (todoMarker.containsMatchIn(added)) add(DiffLineFlag.Todo)
        if (debugCall.containsMatchIn(added)) add(DiffLineFlag.Debug)
        if (broadCatch.containsMatchIn(added)) add(DiffLineFlag.BroadCatch)
        if (isCode(path)) {
            if (added.length > 140) add(DiffLineFlag.LongLine)
            if (added.endsWith(" ") || added.endsWith("\t")) add(DiffLineFlag.TrailingWhitespace)
            if (controlFlow.containsMatchIn(added) && indent(added) >= 12) add(DiffLineFlag.DeepNesting)
        }
        if (requestSite.containsMatchIn(added)) add(DiffLineFlag.Request)
    }

    fun lineStats(files: List<DiffFile>): DiffHealthLineStats {
        val stats = DiffHealthLineStats()
        for (file in files) {
            val code = file.status != DiffFileStatus.Deleted && isCode(file.newPath)
            for (hunk in file.hunks) {
                for (line in hunk.lines) {
                    if (line.kind != DiffLineKind.Added) continue
                    val added = line.text
                    if (code) {
                        if (controlFlow.containsMatchIn(added)) {
                            stats.controlFlowLines++
                            if (indent(added) >= 12) stats.nestedControlFlowLines++
                        }
                        if (added.length > 140) stats.longLines++
                        if (added.endsWith(" ") || added.endsWith("\t")) stats.trailingWhitespaceLines++
                    }
                    if (todoMarker.containsMatchIn(added)) stats.todoLines++
                    if (debugCall.containsMatchIn(added)) stats.debugLines++
                    if (broadCatch.containsMatchIn(added)) stats.broadCatchLines++
                    if (unsafeApi.containsMatchIn(added)) stats.unsafeApiLines++
                    if (transportBypass.containsMatchIn(added)) stats.transportBypassLines++
                    if (permissiveSecurity.containsMatchIn(added)) stats.permissiveSecurityLines++
                    if (requestSite.containsMatchIn(added)) stats.requestLines++
                    if (!stats.resilienceEvidence && resilience.containsMatchIn(added)) stats.resilienceEvidence = true
                    if (isConflictMarker(added)) stats.conflictMarkerLines++
                    val (all, insecure) = urls(added)
                    stats.hardcodedUrls += all
                    stats.insecureUrls += insecure
                    stats.secretMatches += secretMatches(added).size
                }
            }
        }
        return stats
    }

    private fun evidence(parts: List<String>, fallback: String, caveat: String): String {
        var result = if (parts.isEmpty()) fallback else parts.joinToString("; ")
        if (!result.endsWith(".")) result += "."
        return "$result $caveat"
    }

    private fun bounded(score: Int): Int = score.coerceIn(0, 100)

    fun report(files: List<DiffFile>, git: DiffHealthGit = DiffHealthGit.Unknown): List<DiffHealthMetric> {
        var totalAdds = 0
        var totalDels = 0
        var maxFileChurn = 0
        var maxChurnPath = ""
        var sourceFiles = 0
        var testFiles = 0
        var testAdds = 0
        var testDels = 0
        var binaryFiles = 0
        var generatedFiles = 0
        var sensitiveFiles = 0
        var oddArtifacts = 0
        var lockfileChanged = false
        var manifestChanged = false
        for (file in files) {
            val adds = file.additions
            val dels = file.deletions
            totalAdds += adds
            totalDels += dels
            if (adds + dels > maxFileChurn) {
                maxFileChurn = adds + dels
                maxChurnPath = file.path
            }
            val test = isTest(file.path)
            if (isCode(file.path) && !test) sourceFiles++
            if (test) {
                testFiles++
                testAdds += adds
                testDels += dels
            }
            if (file.binary) binaryFiles++
            if (isGenerated(file.path)) generatedFiles++
            if (isSensitive(file)) sensitiveFiles++
            if (isOddArtifact(file)) oddArtifacts++
            val base = fileName(file.path).lowercase()
            lockfileChanged = lockfileChanged || base in lockfiles
            manifestChanged = manifestChanged || base in manifests
        }
        val s = lineStats(files)
        val churn = totalAdds + totalDels
        val fileCount = files.size

        var complexity = 100
        if (churn > 80) complexity -= 4
        if (churn > 250) complexity -= 8
        if (churn > 600) complexity -= 14
        if (churn > 1500) complexity -= 20
        if (maxFileChurn > 250) complexity -= 8
        if (maxFileChurn > 750) complexity -= 10
        if (s.controlFlowLines > 20) complexity -= 6
        if (s.controlFlowLines > 50) complexity -= 10
        if (s.controlFlowLines > 120) complexity -= 15
        if (s.nestedControlFlowLines > 8) complexity -= 8
        if (s.nestedControlFlowLines > 30) complexity -= 12
        complexity = bounded(complexity)
        val complexityBits = mutableListOf("$churn changed lines across $fileCount files")
        if (s.controlFlowLines > 0) complexityBits += "${s.controlFlowLines} added control-flow lines"
        if (s.nestedControlFlowLines > 0) complexityBits += "${s.nestedControlFlowLines} deeply indented branches"
        if (maxFileChurn > 0 && maxChurnPath.isNotEmpty()) complexityBits += "largest file is $maxFileChurn lines in $maxChurnPath"

        var quality = 100
        val qualityBits = mutableListOf<String>()
        if (sourceFiles > 0 && testFiles == 0) {
            quality -= 18
            qualityBits += "$sourceFiles source files changed with no test file in the diff"
            if (totalAdds > 250) quality -= 10
        } else if (testFiles > 0) {
            qualityBits += "$testFiles test files changed"
        } else {
            qualityBits += "no source-code files changed"
        }
        if (testFiles > 0 && testAdds == 0 && testDels > 0) {
            quality -= 20
            qualityBits += "tests were only removed"
        }
        quality -= minOf(25, s.todoLines * 5)
        quality -= minOf(15, s.debugLines * 5)
        quality -= minOf(15, s.broadCatchLines * 5)
        quality -= minOf(15, s.longLines * 2)
        if (s.todoLines > 0) qualityBits += "${s.todoLines} new TODO/FIXME markers"
        if (s.debugLines > 0) qualityBits += "${s.debugLines} debug statements"
        if (s.broadCatchLines > 0) qualityBits += "${s.broadCatchLines} broad exception handlers"
        if (s.longLines > 0) qualityBits += "${s.longLines} code lines over 140 characters"
        quality = bounded(quality)

        var security = 100
        val securityBits = mutableListOf<String>()
        if (s.secretMatches > 0) {
            security -= 70 + minOf(20, (s.secretMatches - 1) * 10)
            securityBits += "${s.secretMatches} high-confidence secret matches"
        }
        security -= minOf(70, s.transportBypassLines * 35)
        security -= minOf(50, s.unsafeApiLines * 20)
        security -= minOf(30, s.permissiveSecurityLines * 15)
        security -= minOf(70, sensitiveFiles * 55)
        if (s.transportBypassLines > 0) securityBits += "${s.transportBypassLines} TLS/certificate bypasses"
        if (s.unsafeApiLines > 0) securityBits += "${s.unsafeApiLines} dynamic execution or unsafe DOM calls"
        if (s.permissiveSecurityLines > 0) securityBits += "${s.permissiveSecurityLines} permissive security settings"
        if (sensitiveFiles > 0) securityBits += "$sensitiveFiles newly tracked key or environment files"
        security = bounded(security)

        var network = 100
        val networkBits = mutableListOf<String>()
        if (s.requestLines > 0) {
            network -= minOf(20, s.requestLines * 4)
            networkBits += "${s.requestLines} added network request sites"
            if (!s.resilienceEvidence) {
                network -= 18
                networkBits += "no added timeout, cancellation, retry, or backoff evidence"
            } else {
                networkBits += "timeout/retry/cancellation evidence present"
            }
        }
        if (s.hardcodedUrls > 0) {
            network -= minOf(10, s.hardcodedUrls * 2)
            networkBits += "${s.hardcodedUrls} hard-coded URLs"
        }
        if (s.insecureUrls > 0) {
            network -= minOf(80, s.insecureUrls * 40)
            networkBits += "${s.insecureUrls} non-local plain-HTTP endpoints"
        }
        network = bounded(network)

        val lockfileWithoutManifest = lockfileChanged && !manifestChanged
        var practices = 100
        val practicesBits = mutableListOf<String>()
        if (s.conflictMarkerLines > 0) {
            practices = 10
            practicesBits += "${s.conflictMarkerLines} unresolved conflict-marker lines"
        }
        practices -= minOf(30, generatedFiles * 12)
        practices -= minOf(20, binaryFiles * 8)
        practices -= minOf(50, oddArtifacts * 25)
        practices -= minOf(15, s.trailingWhitespaceLines * 2)
        practices -= minOf(15, (s.todoLines + s.debugLines) * 3)
        if (lockfileWithoutManifest) practices -= 8
        if (churn > 1000) practices -= 10
        if (generatedFiles > 0) practicesBits += "$generatedFiles generated/vendor paths changed"
        if (binaryFiles > 0) practicesBits += "$binaryFiles binary files changed"
        if (oddArtifacts > 0) practicesBits += "$oddArtifacts unusual compiled/archive artifacts added"
        if (s.trailingWhitespaceLines > 0) practicesBits += "${s.trailingWhitespaceLines} added code lines have trailing whitespace"
        if (lockfileWithoutManifest) practicesBits += "a lockfile changed without its manifest"
        practices = bounded(practices)

        val weighted = (complexity * 25 + quality * 20 + security * 25 + network * 10 + practices * 20 + 50) / 100
        var merge = weighted
        val mergeBits = mutableListOf<String>()
        when (git) {
            DiffHealthGit.Unknown -> {
                merge = minOf(merge, 84)
                mergeBits += "this device has no Git base or conflict check for the branch"
            }
            DiffHealthGit.Merged -> mergeBits += "already merged into the base"
            is DiffHealthGit.State -> when {
                git.mergeConflict -> {
                    merge = minOf(merge, 15)
                    mergeBits += "Git predicts content conflicts with the base"
                }
                git.worktreeConflict -> {
                    merge = minOf(merge, 35)
                    mergeBits += "the branch worktree has unresolved local conflicts"
                }
                else -> {
                    if (git.behind > 0) {
                        merge -= minOf(24, 4 + git.behind * 2)
                        mergeBits += "${git.behind} commits behind the base"
                    } else {
                        mergeBits += "up to date with the base"
                    }
                    if (git.ahead >= 0) mergeBits += "${git.ahead} commits ahead"
                }
            }
        }
        if (fileCount > 25) merge -= 5
        if (fileCount > 75) merge -= 10
        if (security < 60 || network < 40 || practices < 30) merge = minOf(merge, 55)
        merge = bounded(merge)
        mergeBits += "weighted static diff health $weighted/100"

        return listOf(
            DiffHealthMetric(
                "merge", "Merge", merge,
                evidence(mergeBits, "No merge signals available", "Combines Git divergence/conflict state with the five static diff scores."),
            ),
            DiffHealthMetric(
                "complexity", "Complexity", complexity,
                evidence(complexityBits, "No changed lines", "Estimated from churn, control flow, and concentration; no code was executed."),
            ),
            DiffHealthMetric(
                "quality", "Quality", quality,
                evidence(qualityBits, "No quality findings", "Tests are inferred from paths and were not executed."),
            ),
            DiffHealthMetric(
                "security", "Security", security,
                evidence(
                    securityBits, "No high-confidence secrets or risky added-line patterns found",
                    "Static heuristic only; it does not replace dependency or SAST scanning.",
                ),
            ),
            DiffHealthMetric(
                "network", "Network", network,
                evidence(
                    networkBits, "No added request sites or hard-coded URLs detected",
                    "This is syntax analysis; ForkMesh sent no traffic for this check.",
                ),
            ),
            DiffHealthMetric(
                "practices", "Practices", practices,
                evidence(
                    practicesBits, "No unusual artifacts, conflict markers, or diff-hygiene issues found",
                    "Checks only the files and added lines in this diff.",
                ),
            ),
        )
    }
}
