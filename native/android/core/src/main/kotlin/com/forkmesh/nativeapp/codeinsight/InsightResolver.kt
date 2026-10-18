package com.forkmesh.nativeapp.codeinsight

internal class Resolver(private val s: InsightSnapshot) {
    private val files = s.files
    private val byName = HashMap<String, MutableList<Int>>()
    private val dirFiles = HashMap<String, MutableList<Int>>()
    private val packages = HashMap<String, MutableList<Int>>()
    val namespaces = HashSet<String>()
    val types = HashMap<String, HashMap<String, MutableList<Int>>>()

    init {
        for ((i, file) in files.withIndex()) {
            byName.getOrPut(Paths.fileNameOf(file.path)) { ArrayList() }.add(i)
            dirFiles.getOrPut(Paths.dirOf(file.path)) { ArrayList() }.add(i)
            for (declared in file.declares) {
                if (declared.startsWith("package:")) {
                    val name = declared.substring(8)
                    packages.getOrPut(name) { ArrayList() }.add(i)
                    namespaces.add(name)
                    var prefix = name
                    while (prefix.contains('.')) {
                        prefix = QText.section(prefix, ".", 0, -2)
                        namespaces.add(prefix)
                    }
                } else {
                    types.getOrPut(family(file.language)) { HashMap() }.getOrPut(declared) { ArrayList() }.add(i)
                }
            }
        }
    }

    companion object {
        fun family(language: String): String =
            if (language == "Java" || language == "Kotlin" || language == "Scala" || language == "Groovy") "jvm" else language

        private val jsExtensions = listOf(
            "", ".ts", ".tsx", ".js", ".jsx", ".mjs", ".cjs", ".mts", ".cts", ".vue", ".svelte", ".d.ts",
            "/index.ts", "/index.tsx", "/index.js", "/index.jsx", "/index.mjs", "/index.vue",
        )
        private val jsToTs = listOf(".ts", ".tsx", ".mts", ".cts")
        private val jsSuffixes = listOf(".ts", ".tsx", ".js", ".jsx", "/index.ts", "/index.js")
        private val none = listOf("")
        private val jvm = listOf(".java", ".kt", ".scala", ".groovy")
        private val pythonExtensions = listOf(".py", "/__init__.py", ".pyi")
        private val rustExtensions = listOf(".rs", "/mod.rs")
    }

    private fun exact(path: String): Int {
        val clean = Paths.normalized(path)
        return if (clean.isEmpty()) -1 else s.byPath[clean] ?: -1
    }

    private fun probe(base: String, extensions: List<String>): Int {
        for (extension in extensions) {
            val hit = exact(base + extension)
            if (hit >= 0) return hit
        }
        return -1
    }

    private fun best(candidates: List<Int>, importer: String): Int {
        var chosen = -1
        var score = -1
        for (candidate in candidates) {
            val common = Paths.commonPrefix(files[candidate].path, importer)
            if (common > score) {
                score = common
                chosen = candidate
            }
        }
        return chosen
    }

    private fun suffix(spec: String, importer: String, extensions: List<String>): Int {
        val clean = QText.cleanPath(spec)
        if (clean.isEmpty() || clean.startsWith("..")) return -1
        for (extension in extensions) {
            val wanted = clean + extension
            val named = byName[Paths.fileNameOf(wanted)] ?: continue
            val matches = named.filter { val path = files[it].path; path == wanted || path.endsWith("/$wanted") }
            if (matches.isNotEmpty()) return best(matches, importer)
        }
        return -1
    }

    private fun one(index: Int): List<Int> = if (index >= 0) listOf(index) else emptyList()

    fun resolve(importer: Int, rawSpec: String): List<Int> {
        val file = files[importer]
        val language = file.language
        val dir = Paths.dirOf(file.path)
        val spec = if (rawSpec.startsWith('?')) rawSpec.substring(1) else rawSpec
        when (language) {
            "C", "C++", "Objective-C" -> {
                if (spec.startsWith('<')) {
                    val inner = QText.mid(spec, 1, spec.length - 2)
                    if (!inner.contains('.') && !inner.contains('/')) return emptyList()
                    return one(suffix(inner, file.path, none))
                }
                var hit = exact(Paths.join(dir, spec))
                if (hit < 0) hit = exact(spec)
                if (hit < 0) hit = suffix(spec, file.path, none)
                return one(hit)
            }
            "JavaScript", "TypeScript", "Vue", "Svelte" -> {
                val bases = when {
                    spec.startsWith("./") || spec.startsWith("../") || spec == "." || spec == ".." -> listOf(Paths.join(dir, spec))
                    spec.startsWith("@/") || spec.startsWith("~/") -> listOf("src/" + spec.substring(2), spec.substring(2))
                    spec.startsWith('/') -> listOf(spec.substring(1))
                    else -> listOf(spec, "src/$spec")
                }
                for (base in bases) {
                    var hit = probe(base, jsExtensions)
                    if (hit < 0) {
                        for (js in listOf(".js", ".mjs", ".jsx", ".cjs")) {
                            if (base.endsWith(js)) {
                                hit = probe(base.substring(0, base.length - js.length), jsToTs)
                                break
                            }
                        }
                    }
                    if (hit >= 0) return one(hit)
                }
                if (!spec.startsWith('.') && spec.contains('/') && !spec.startsWith('@')) return one(suffix(spec, file.path, jsSuffixes))
                return emptyList()
            }
            "Python" -> return one(resolvePython(file, spec))
            "Go" -> {
                for ((key, root) in s.packageRoots) {
                    if (!key.startsWith("go:")) continue
                    val module = key.substring(3)
                    if (spec != module && !spec.startsWith("$module/")) continue
                    val target = Paths.normalized(Paths.join(root, QText.mid(spec, module.length + 1)))
                    val hits = ArrayList<Int>()
                    for (candidate in dirFiles[target].orEmpty()) {
                        val other = files[candidate]
                        if (other.language == "Go" && !other.test) hits.add(candidate)
                        if (hits.size >= 16) break
                    }
                    return hits
                }
                return emptyList()
            }
            "Rust" -> return one(resolveRust(file, spec))
            "Java", "Kotlin", "Scala", "Groovy" -> {
                if (spec.endsWith(".*")) return emptyList()
                var current = spec
                var attempt = 0
                while (attempt < 2 && current.contains('.')) {
                    val hit = suffix(current.replace('.', '/'), file.path, jvm)
                    if (hit >= 0) return one(hit)
                    val pkg = QText.section(current, ".", 0, -2)
                    val type = QText.section(current, ".", -1)
                    val declared = packages[pkg].orEmpty().filter {
                        Paths.stemOf(files[it].path) == type || files[it].declares.contains(type)
                    }
                    if (declared.isNotEmpty()) return one(best(declared, file.path))
                    current = pkg
                    attempt++
                }
                return emptyList()
            }
            "PHP" -> {
                if (spec.contains('\\')) {
                    val parts = spec.split('\\').filter { it.isNotEmpty() }.toMutableList()
                    while (parts.size >= 2) {
                        val hit = suffix(parts.joinToString("/"), file.path, listOf(".php"))
                        if (hit >= 0) return one(hit)
                        parts.removeAt(0)
                    }
                    return emptyList()
                }
                var hit = exact(Paths.join(dir, spec))
                if (hit < 0) hit = exact(spec)
                return one(hit)
            }
            "Ruby" -> {
                if (spec.startsWith("./")) return one(probe(Paths.join(dir, spec.substring(2)), listOf("", ".rb")))
                var hit = probe("lib/$spec", listOf(".rb"))
                if (hit < 0 && spec.contains('/')) hit = suffix(spec, file.path, listOf(".rb"))
                return one(hit)
            }
            "Dart" -> {
                if (spec.startsWith("dart:")) return emptyList()
                if (spec.startsWith("package:")) {
                    val rest = spec.substring(8)
                    val name = QText.section(rest, "/", 0, 0)
                    val root = s.packageRoots["dart:$name"] ?: return emptyList()
                    return one(exact(Paths.join(Paths.join(root, "lib"), QText.section(rest, "/", 1))))
                }
                return one(exact(Paths.join(dir, spec)))
            }
            "Lua" -> {
                val path = spec.replace('.', '/')
                val extensions = listOf(".lua", "/init.lua")
                var hit = probe(path, extensions)
                if (hit < 0) hit = suffix(path, file.path, extensions)
                return one(hit)
            }
            "CSS", "SCSS", "Less" -> {
                if (spec.contains("://")) return emptyList()
                val base = Paths.join(dir, spec)
                val partial = Paths.join(Paths.dirOf(base), "_" + Paths.fileNameOf(base))
                var hit = probe(base, listOf("", ".scss", ".sass", ".css", ".less"))
                if (hit < 0) hit = probe(partial, listOf("", ".scss", ".sass"))
                return one(hit)
            }
            "QML" -> {
                if (!spec.startsWith("./")) return emptyList()
                val hits = ArrayList<Int>()
                for (candidate in dirFiles[Paths.normalized(Paths.join(dir, spec))].orEmpty()) {
                    if (files[candidate].language == "QML" && hits.size < 12) hits.add(candidate)
                }
                return hits
            }
            "Shell" -> {
                if (spec.contains('$')) return one(suffix(QText.section(spec, "/", -2), file.path, none))
                var hit = exact(Paths.join(dir, spec))
                if (hit < 0) hit = exact(spec)
                return one(hit)
            }
        }
        return emptyList()
    }

    private fun resolvePython(file: InsightFile, spec: String): Int {
        var dots = 0
        while (dots < spec.length && spec[dots] == '.') dots++
        val module = spec.substring(dots)
        val path = module.replace('.', '/')
        if (dots > 0) {
            var base = Paths.dirOf(file.path)
            for (up in 1 until dots) base = Paths.dirOf(base)
            if (path.isEmpty()) return exact(Paths.join(base, "__init__.py"))
            return probe(Paths.join(base, path), pythonExtensions)
        }
        if (path.isEmpty()) return -1
        var hit = probe(path, pythonExtensions)
        if (hit < 0) hit = probe(Paths.join(Paths.dirOf(file.path), path), pythonExtensions)
        if (hit < 0) {
            val matches = ArrayList<Int>()
            for (tail in listOf(path, "$path/__init__")) {
                val name = Paths.fileNameOf(tail)
                for (candidate in byName["$name.py"].orEmpty()) {
                    val noExt = Paths.withoutExtension(files[candidate].path)
                    if (noExt == tail || noExt.endsWith("/$tail")) matches.add(candidate)
                }
            }
            if (matches.isNotEmpty()) hit = best(matches, file.path)
        }
        return hit
    }

    private fun resolveRustPath(base: String, segments: List<String>): Int {
        for (k in segments.size downTo 1) {
            val hit = probe(Paths.join(base, segments.subList(0, k).joinToString("/")), rustExtensions)
            if (hit >= 0) return hit
        }
        return -1
    }

    private fun resolveRust(file: InsightFile, spec: String): Int {
        val dir = Paths.dirOf(file.path)
        val stem = Paths.stemOf(file.path)
        val rootLike = stem == "mod" || stem == "lib" || stem == "main"
        val moduleDir = if (rootLike) dir else Paths.join(dir, stem)
        if (spec.startsWith("mod ")) return probe(Paths.join(moduleDir, spec.substring(4)), rustExtensions)
        val all = spec.split("::")
        val head = all.first()
        val segments = all.drop(1)
        if (head == "crate") {
            var crate = dir
            while (true) {
                if (exact(Paths.join(crate, "lib.rs")) >= 0 || exact(Paths.join(crate, "main.rs")) >= 0) break
                if (crate.isEmpty()) return -1
                crate = Paths.dirOf(crate)
            }
            return resolveRustPath(crate, segments)
        }
        if (head == "super") return resolveRustPath(if (rootLike) Paths.dirOf(dir) else dir, segments)
        return resolveRustPath(moduleDir, segments)
    }
}

internal object Dependencies {
    fun resolve(snapshot: InsightSnapshot) {
        val resolver = Resolver(snapshot)
        val files = snapshot.files
        for ((i, file) in files.withIndex()) {
            val targets = HashSet<Int>()
            val shown = ArrayList<String>()
            val external = ArrayList<String>()
            for (spec in file.imports) {
                val optional = spec.startsWith('?')
                val hits = resolver.resolve(i, spec)
                for (hit in hits) if (hit != i) targets.add(hit)
                if (optional) continue
                shown.add(spec)
                if (hits.isEmpty()) {
                    if (file.language == "C#" && spec in resolver.namespaces) continue
                    if (Resolver.family(file.language) == "jvm" &&
                        (QText.section(spec, ".", 0, -2) in resolver.namespaces || spec.replace(".*", "") in resolver.namespaces)
                    ) continue
                    external.add(spec)
                }
            }
            val declaredTypes = resolver.types[Resolver.family(file.language)]
            if (declaredTypes != null) {
                for (reference in file.references) {
                    val declared = declaredTypes[reference] ?: continue
                    if (declared.size > 2) continue
                    for (hit in declared) if (hit != i) targets.add(hit)
                }
            }
            file.references = emptyList()
            file.imports = shown
            file.externalImports = external
            file.dependsOn = targets.sorted()
        }
        val reverse = Array(files.size) { ArrayList<Int>() }
        for ((i, file) in files.withIndex()) for (target in file.dependsOn) reverse[target].add(i)
        for ((i, file) in files.withIndex()) file.dependedOnBy = reverse[i]
    }
}
