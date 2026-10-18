package com.forkmesh.nativeapp.workflows

import java.util.IdentityHashMap

@Suppress("UNCHECKED_CAST")
private fun mlist(value: Any?): MutableList<Any?> = value as MutableList<Any?>

private fun jsList(items: Collection<Any?>): JsArray = JsArray(items)

private fun aPush(i: JsInterpreter, t: Any?, a: List<Any?>): Any? {
    val list = mlist(t)
    list.addAll(a)
    checkArray(list)
    return list.size.toDouble()
}

private fun aSlice(i: JsInterpreter, t: Any?, a: List<Any?>): Any? {
    val list = t as List<*>
    val start = relative(arg(a, 0), list.size, 0)
    val end = relative(arg(a, 1), list.size, list.size)
    return if (start < end) jsList(list.subList(start, end)) else JsArray()
}

private fun aSplice(i: JsInterpreter, t: Any?, a: List<Any?>): Any? {
    val list = mlist(t)
    val length = list.size
    val start = relative(arg(a, 0), length, 0)
    val count = when {
        a.isEmpty() -> 0
        a.size == 1 -> length - start
        else -> maxOf(0.0, minOf(toInteger(a[1]), (length - start).toDouble())).toInt()
    }
    val removed = jsList(list.subList(start, start + count))
    for (n in 0 until count) list.removeAt(start)
    list.addAll(start, if (a.size > 2) a.subList(2, a.size) else emptyList())
    checkArray(list)
    return removed
}

private fun aConcat(i: JsInterpreter, t: Any?, a: List<Any?>): Any? {
    val out = jsList(t as List<*>)
    for (item in a) {
        if (item is List<*>) out.addAll(item) else out.add(item)
        checkArray(out)
    }
    return out
}

private fun aJoin(i: JsInterpreter, t: Any?, a: List<Any?>): Any? {
    val separator = arg(a, 0).let { if (it === Undefined) "," else jsToString(it) }
    val seen = IdentityHashMap<Any, Boolean>()
    seen[t!!] = true
    return joinList(t as List<*>, separator, seen)
}

private fun defaultCompare(a: Any?, b: Any?): Int {
    if (a === Undefined) return if (b === Undefined) 0 else 1
    if (b === Undefined) return -1
    return cpCompare(jsToString(a), jsToString(b))
}

private fun stableSort(items: MutableList<Any?>, compare: (Any?, Any?) -> Int) {
    try {
        java.util.Collections.sort(items, Comparator { x, y -> compare(x, y) })
    } catch (_: IllegalArgumentException) {
        val copy = ArrayList(items)
        for (index in 1 until copy.size) {
            val value = copy[index]
            var j = index - 1
            while (j >= 0 && compare(copy[j], value) > 0) {
                copy[j + 1] = copy[j]
                j--
            }
            copy[j + 1] = value
        }
        items.clear()
        items.addAll(copy)
    }
}

private fun sortedItems(interp: JsInterpreter, items: MutableList<Any?>, comparator: Any?): MutableList<Any?> {
    if (comparator === Undefined) {
        stableSort(items, ::defaultCompare)
        return items
    }
    if (!isCallable(comparator)) throw typeError("The comparison function must be either a function or undefined")
    stableSort(items) { a, b ->
        if (a === Undefined) {
            if (b === Undefined) 0 else 1
        } else if (b === Undefined) {
            -1
        } else {
            val result = jsToNumber(interp.call(comparator, Undefined, listOf(a, b)))
            if (result.isNaN()) 0 else if (result > 0) 1 else if (result < 0) -1 else 0
        }
    }
    return items
}

private fun aSort(i: JsInterpreter, t: Any?, a: List<Any?>): Any? {
    val list = mlist(t)
    val sorted = sortedItems(i, ArrayList(list), arg(a, 0))
    list.clear()
    list.addAll(sorted)
    return t
}

private fun aIndexOf(i: JsInterpreter, t: Any?, a: List<Any?>): Any? {
    val list = t as List<*>
    val target = arg(a, 0)
    for (index in relative(arg(a, 1), list.size, 0) until list.size) if (strictEquals(list[index], target)) return index.toDouble()
    return -1.0
}

private fun aLastIndexOf(i: JsInterpreter, t: Any?, a: List<Any?>): Any? {
    val list = t as List<*>
    val target = arg(a, 0)
    val length = list.size
    val start = if (a.size > 1) {
        val s = toInteger(a[1])
        if (s >= 0) minOf(s, (length - 1).toDouble()) else length + s
    } else {
        (length - 1).toDouble()
    }
    var index = if (start == Double.NEGATIVE_INFINITY) -1 else start.toInt()
    while (index >= 0) {
        if (strictEquals(list[index], target)) return index.toDouble()
        index--
    }
    return -1.0
}

private fun aIncludes(i: JsInterpreter, t: Any?, a: List<Any?>): Any? {
    val list = t as List<*>
    val target = arg(a, 0)
    for (index in relative(arg(a, 1), list.size, 0) until list.size) if (sameValueZero(list[index], target)) return true
    return false
}

private inline fun eachCallback(interp: JsInterpreter, t: Any?, a: List<Any?>, visit: (Int, Any?, Any?) -> Boolean) {
    val list = t as List<*>
    val fn = arg(a, 0)
    if (!isCallable(fn)) throw typeError(interp.displayShort(fn) + " is not a function")
    val thisArg = arg(a, 1)
    val length = list.size
    var index = 0
    while (index < length && index < list.size) {
        val value = list[index]
        if (!visit(index, value, interp.call(fn, thisArg, listOf(value, index.toDouble(), t)))) return
        index++
    }
}

private fun aFind(i: JsInterpreter, t: Any?, a: List<Any?>): Any? {
    var found: Any? = Undefined
    eachCallback(i, t, a) { _, value, result ->
        if (jsTruthy(result)) {
            found = value
            false
        } else {
            true
        }
    }
    return found
}

private fun aFindIndex(i: JsInterpreter, t: Any?, a: List<Any?>): Any? {
    var found = -1.0
    eachCallback(i, t, a) { index, _, result ->
        if (jsTruthy(result)) {
            found = index.toDouble()
            false
        } else {
            true
        }
    }
    return found
}

private fun aFindLast(i: JsInterpreter, t: Any?, a: List<Any?>, wantIndex: Boolean): Any? {
    val list = t as List<*>
    val fn = arg(a, 0)
    if (!isCallable(fn)) throw typeError(i.displayShort(fn) + " is not a function")
    for (index in list.size - 1 downTo 0) {
        val value = if (index < list.size) list[index] else Undefined
        if (jsTruthy(i.call(fn, arg(a, 1), listOf(value, index.toDouble(), t)))) return if (wantIndex) index.toDouble() else value
    }
    return if (wantIndex) -1.0 else Undefined
}

private fun aFilter(i: JsInterpreter, t: Any?, a: List<Any?>): Any? {
    val out = JsArray()
    eachCallback(i, t, a) { _, value, result ->
        if (jsTruthy(result)) out.add(value)
        true
    }
    return out
}

private fun aMap(i: JsInterpreter, t: Any?, a: List<Any?>): JsArray {
    val out = JsArray()
    eachCallback(i, t, a) { _, _, result ->
        out.add(result)
        true
    }
    return out
}

private fun aForEach(i: JsInterpreter, t: Any?, a: List<Any?>): Any? {
    eachCallback(i, t, a) { _, _, _ -> true }
    return Undefined
}

private fun aSome(i: JsInterpreter, t: Any?, a: List<Any?>): Any? {
    var found = false
    eachCallback(i, t, a) { _, _, result ->
        if (jsTruthy(result)) {
            found = true
            false
        } else {
            true
        }
    }
    return found
}

private fun aEvery(i: JsInterpreter, t: Any?, a: List<Any?>): Any? {
    var all = true
    eachCallback(i, t, a) { _, _, result ->
        if (!jsTruthy(result)) {
            all = false
            false
        } else {
            true
        }
    }
    return all
}

private fun aReduce(i: JsInterpreter, t: Any?, a: List<Any?>, reverse: Boolean): Any? {
    val list = t as List<*>
    val fn = arg(a, 0)
    if (!isCallable(fn)) throw typeError(i.displayShort(fn) + " is not a function")
    val indexes = ArrayList((0 until list.size).toList())
    if (reverse) indexes.reverse()
    var acc: Any?
    if (a.size >= 2) {
        acc = a[1]
    } else {
        if (indexes.isEmpty()) throw typeError("Reduce of empty array with no initial value")
        acc = list[indexes.removeAt(0)]
    }
    for (index in indexes) {
        if (index >= list.size) continue
        acc = i.call(fn, Undefined, listOf(acc, list[index], index.toDouble(), t))
    }
    return acc
}

private fun flatten(items: List<*>, depth: Double, out: JsArray): JsArray {
    for (item in items) {
        if (item is List<*> && depth >= 1) flatten(item, depth - 1, out) else out.add(item)
        checkArray(out)
    }
    return out
}

private fun aFill(i: JsInterpreter, t: Any?, a: List<Any?>): Any? {
    val list = mlist(t)
    val start = relative(arg(a, 1), list.size, 0)
    val end = relative(arg(a, 2), list.size, list.size)
    for (index in start until end) list[index] = arg(a, 0)
    return t
}

private fun aAt(i: JsInterpreter, t: Any?, a: List<Any?>): Any? {
    val list = t as List<*>
    var index = toInteger(arg(a, 0))
    if (index < 0) index += list.size
    if (index < 0 || index >= list.size) return Undefined
    return list[index.toInt()]
}

internal val ARRAY_METHODS: Map<String, NativeFn> = mapOf(
    "push" to ::aPush,
    "pop" to { _, t, _ -> mlist(t).let { if (it.isEmpty()) Undefined else it.removeAt(it.size - 1) } },
    "shift" to { _, t, _ -> mlist(t).let { if (it.isEmpty()) Undefined else it.removeAt(0) } },
    "unshift" to { _, t, a ->
        val list = mlist(t)
        list.addAll(0, a)
        checkArray(list)
        list.size.toDouble()
    },
    "slice" to ::aSlice,
    "splice" to ::aSplice,
    "concat" to ::aConcat,
    "join" to ::aJoin,
    "reverse" to { _, t, _ ->
        mlist(t).reverse()
        t
    },
    "sort" to ::aSort,
    "toSorted" to { i, t, a -> jsList(sortedItems(i, ArrayList(t as List<*>), arg(a, 0))) },
    "toReversed" to { _, t, _ -> jsList((t as List<*>).reversed()) },
    "indexOf" to ::aIndexOf,
    "lastIndexOf" to ::aLastIndexOf,
    "includes" to ::aIncludes,
    "find" to ::aFind,
    "findIndex" to ::aFindIndex,
    "findLast" to { i, t, a -> aFindLast(i, t, a, false) },
    "findLastIndex" to { i, t, a -> aFindLast(i, t, a, true) },
    "filter" to ::aFilter,
    "map" to ::aMap,
    "forEach" to ::aForEach,
    "some" to ::aSome,
    "every" to ::aEvery,
    "reduce" to { i, t, a -> aReduce(i, t, a, false) },
    "reduceRight" to { i, t, a -> aReduce(i, t, a, true) },
    "flat" to { _, t, a -> flatten(t as List<*>, arg(a, 0).let { if (it === Undefined) 1.0 else toInteger(it) }, JsArray()) },
    "flatMap" to { i, t, a -> flatten(aMap(i, t, a), 1.0, JsArray()) },
    "fill" to ::aFill,
    "at" to ::aAt,
    "keys" to { _, t, _ -> jsList((t as List<*>).indices.map { it.toDouble() }) },
    "values" to { _, t, _ -> jsList(t as List<*>) },
    "entries" to { _, t, _ -> jsList((t as List<*>).mapIndexed { index, value -> JsArray(listOf(index.toDouble(), value)) }) },
    "toString" to { _, t, _ -> jsToString(t) },
)

private fun sCharAt(t: String, a: List<Any?>): Any? {
    val index = toInteger(arg(a, 0))
    return if (index >= 0 && index < cpLen(t)) cpAt(t, index.toInt()) else ""
}

private fun sCodePointAt(t: String, a: List<Any?>): Any? {
    val index = toInteger(arg(a, 0))
    return if (index >= 0 && index < cpLen(t)) cpAt(t, index.toInt()).codePointAt(0).toDouble() else Double.NaN
}

private fun sAt(t: String, a: List<Any?>): Any? {
    var index = toInteger(arg(a, 0))
    val length = cpLen(t)
    if (index < 0) index += length
    return if (index >= 0 && index < length) cpAt(t, index.toInt()) else Undefined
}

private fun rfindCp(text: String, needle: String, end: Int): Int {
    val cps = cpList(text)
    val needleCps = cpList(needle)
    val limit = minOf(end, cps.size)
    var start = limit - needleCps.size
    while (start >= 0) {
        var ok = true
        for (k in needleCps.indices) if (cps[start + k] != needleCps[k]) {
            ok = false
            break
        }
        if (ok) return start
        start--
    }
    return -1
}

private fun sLastIndexOf(t: String, a: List<Any?>): Any? {
    val needle = jsToString(arg(a, 0))
    val position = jsToNumber(arg(a, 1))
    val length = cpLen(t)
    val start = if (position.isNaN()) length else clampIndex(position, length)
    return rfindCp(t, needle, start + cpLen(needle)).toDouble()
}

private fun noRegex(value: Any?, name: String) {
    if (value is JsRegExp) throw typeError("First argument to String.prototype.$name must not be a regular expression")
}

private fun startsWithCp(text: String, needle: String, position: Int): Boolean = cpSub(text, position).startsWith(needle)

private fun sSlice(t: String, a: List<Any?>): Any? {
    val length = cpLen(t)
    val start = relative(arg(a, 0), length, 0)
    val end = relative(arg(a, 1), length, length)
    return if (start < end) cpSub(t, start, end) else ""
}

private fun sSubstring(t: String, a: List<Any?>): Any? {
    val length = cpLen(t)
    var start = clampIndex(arg(a, 0), length)
    var end = if (arg(a, 1) === Undefined) length else clampIndex(a[1], length)
    if (start > end) {
        val swap = start
        start = end
        end = swap
    }
    return cpSub(t, start, end)
}

private fun sSubstr(t: String, a: List<Any?>): Any? {
    val length = cpLen(t)
    val start = relative(arg(a, 0), length, 0)
    val raw = if (arg(a, 1) === Undefined) (length - start).toDouble() else toInteger(a[1])
    val count = if (raw != Double.POSITIVE_INFINITY) maxOf(0.0, minOf(raw, (length - start).toDouble())).toInt() else length - start
    return cpSub(t, start, start + count)
}

private fun pad(t: String, a: List<Any?>, atStart: Boolean): Any? {
    val target = toInteger(arg(a, 0))
    val filler = arg(a, 1).let { if (it === Undefined) " " else jsToString(it) }
    val length = cpLen(t)
    if (target <= length || filler.isEmpty()) return t
    if (target > JsLimits.MAX_STRING) throw rangeError("Invalid string length")
    val needed = target.toInt() - length
    val fillerLen = cpLen(filler)
    val padText = cpSub(filler.repeat(needed / fillerLen + 1), 0, needed)
    return if (atStart) padText + t else t + padText
}

private fun sRepeat(t: String, a: List<Any?>): Any? {
    val count = toInteger(arg(a, 0))
    if (count < 0 || count == Double.POSITIVE_INFINITY) throw rangeError("Invalid count value: " + jsToString(jsToNumber(arg(a, 0))))
    if (cpLen(t) * count > JsLimits.MAX_STRING) throw rangeError("Invalid string length")
    return t.repeat(count.toInt())
}

private fun splitPlain(text: String, separator: String): List<String> {
    val out = ArrayList<String>()
    var start = 0
    while (true) {
        val found = text.indexOf(separator, start)
        if (found < 0) break
        out.add(text.substring(start, found))
        start = found + separator.length
    }
    out.add(text.substring(start))
    return out
}

private fun sSplit(t: String, a: List<Any?>): Any? {
    val separator = arg(a, 0)
    val rawLimit = arg(a, 1)
    val limit = if (rawLimit === Undefined) 4294967295L else toUint32(rawLimit)
    if (limit == 0L) return JsArray()
    if (separator === Undefined) return JsArray(listOf(t))
    if (separator is JsRegExp) {
        val size = cpLen(t)
        if (size == 0) return if (regexSearch(separator.compiled, t, 0, true) != null) JsArray() else JsArray(listOf(t))
        val out = JsArray()
        var p = 0
        var q = 0
        while (q < size) {
            val match = regexSearch(separator.compiled, t, q, true)
            if (match == null) {
                q++
                continue
            }
            val end = minOf(match.end, size)
            if (end == p) {
                q++
                continue
            }
            out.add(cpSub(t, p, q))
            if (out.size >= limit) return out
            p = end
            for (group in match.groups) {
                out.add(group ?: Undefined)
                if (out.size >= limit) return out
            }
            checkArray(out)
            q = p
        }
        out.add(cpSub(t, p))
        return out
    }
    val sep = jsToString(separator)
    val parts: List<String> = if (sep.isEmpty()) cpList(t) else splitPlain(t, sep)
    checkArray(ArrayList(parts))
    return JsArray(parts.take(minOf(limit, Int.MAX_VALUE.toLong()).toInt()))
}

private fun replaceOne(
    interp: JsInterpreter, subject: String, matched: String, position: Int, groups: List<Any?>, named: Map<String, Any?>?, replacement: Any?,
): String {
    if (isCallable(replacement)) {
        val callArgs = ArrayList<Any?>()
        callArgs.add(matched)
        callArgs.addAll(groups)
        callArgs.add(position.toDouble())
        callArgs.add(subject)
        if (named != null) callArgs.add(JsObject().also { it.putAll(named) })
        return jsToString(interp.call(replacement, Undefined, callArgs))
    }
    return expandReplacement(jsToString(replacement), matched, position, subject, groups, named)
}

private fun sReplace(interp: JsInterpreter, t: String, a: List<Any?>, replaceAll: Boolean): Any? {
    val pattern = arg(a, 0)
    val replacement = arg(a, 1)
    if (pattern is JsRegExp) {
        if (replaceAll && !pattern.flags.contains('g')) throw typeError("replaceAll must be called with a global RegExp")
        val matches = if (pattern.flags.contains('g')) {
            regexAll(pattern.compiled, t).also { pattern.lastIndex = 0 }
        } else {
            listOfNotNull(regexSearch(pattern.compiled, t, 0))
        }
        val out = StringBuilder()
        var last = 0
        for (match in matches) {
            val groups = match.groups.map { it ?: Undefined }
            val named: Map<String, Any?>? = if (pattern.names.isNotEmpty()) pattern.names.associateWith { match.named[it] ?: Undefined } else null
            out.append(cpSub(t, last, match.start))
            out.append(replaceOne(interp, t, match.group0, match.start, groups, named, replacement))
            last = match.end
        }
        out.append(cpSub(t, last))
        return checkString(out.toString())
    }
    val needle = jsToString(pattern)
    val positions = ArrayList<Int>()
    val length = cpLen(t)
    if (replaceAll) {
        var start = 0
        val step = maxOf(1, cpLen(needle))
        while (true) {
            val found = cpFind(t, needle, start)
            if (found < 0) break
            positions.add(found)
            start = found + step
            if (start > length) break
        }
    } else {
        val found = cpFind(t, needle, 0)
        if (found >= 0) positions.add(found)
    }
    val out = StringBuilder()
    var last = 0
    for (position in positions) {
        out.append(cpSub(t, last, position))
        out.append(replaceOne(interp, t, needle, position, emptyList(), null, replacement))
        last = position + cpLen(needle)
    }
    out.append(cpSub(t, last))
    return checkString(out.toString())
}

private fun asRegExp(interp: JsInterpreter, value: Any?, flags: String = ""): JsRegExp {
    if (value is JsRegExp) return value
    val source = if (value === Undefined) "(?:)" else jsToString(value)
    val compiled = try {
        compileRegex(source, flags, interp.line)
    } catch (error: ScriptError) {
        throw Thrown(errorObject("SyntaxError", error.text))
    }
    return JsRegExp(source, flags, compiled)
}

private fun sMatch(interp: JsInterpreter, t: String, a: List<Any?>): Any? {
    val regex = asRegExp(interp, arg(a, 0))
    if (!regex.flags.contains('g')) return regexExec(regex, t)
    val matches = regexAll(regex.compiled, t)
    regex.lastIndex = 0
    if (matches.isEmpty()) return null
    return JsArray(matches.map { it.group0 })
}

private fun sMatchAll(interp: JsInterpreter, t: String, a: List<Any?>): Any? {
    val raw = arg(a, 0)
    if (raw is JsRegExp && !raw.flags.contains('g')) throw typeError("matchAll must be called with a global RegExp")
    val regex = asRegExp(interp, raw, "g")
    return JsArray(regexAll(regex.compiled, t).map { matchArray(regex, it, t) })
}

private fun normalize(t: String, a: List<Any?>): Any? {
    val form = arg(a, 0).let { if (it === Undefined) "NFC" else jsToString(it) }
    val kind = when (form) {
        "NFC" -> java.text.Normalizer.Form.NFC
        "NFD" -> java.text.Normalizer.Form.NFD
        "NFKC" -> java.text.Normalizer.Form.NFKC
        "NFKD" -> java.text.Normalizer.Form.NFKD
        else -> throw rangeError("The normalization form should be one of NFC, NFD, NFKC, NFKD.")
    }
    return java.text.Normalizer.normalize(t, kind)
}

private fun stringMethod(fn: (JsInterpreter, String, List<Any?>) -> Any?): NativeFn = { interp, t, a ->
    if (t == null || t === Undefined) throw typeError("String.prototype method called on null or undefined")
    fn(interp, if (t is String) t else jsToString(t), a)
}

internal val STRING_METHODS: Map<String, NativeFn> = mapOf<String, (JsInterpreter, String, List<Any?>) -> Any?>(
    "charAt" to { _, t, a -> sCharAt(t, a) },
    "charCodeAt" to { _, t, a -> sCodePointAt(t, a) },
    "codePointAt" to { _, t, a -> sCodePointAt(t, a) },
    "at" to { _, t, a -> sAt(t, a) },
    "indexOf" to { _, t, a -> cpFind(t, jsToString(arg(a, 0)), clampIndex(arg(a, 1), cpLen(t))).toDouble() },
    "lastIndexOf" to { _, t, a -> sLastIndexOf(t, a) },
    "includes" to { _, t, a ->
        noRegex(arg(a, 0), "includes")
        cpFind(t, jsToString(arg(a, 0)), clampIndex(arg(a, 1), cpLen(t))) >= 0
    },
    "startsWith" to { _, t, a ->
        noRegex(arg(a, 0), "startsWith")
        startsWithCp(t, jsToString(arg(a, 0)), clampIndex(arg(a, 1), cpLen(t)))
    },
    "endsWith" to { _, t, a ->
        noRegex(arg(a, 0), "endsWith")
        val end = if (arg(a, 1) === Undefined) cpLen(t) else clampIndex(a[1], cpLen(t))
        cpSub(t, 0, end).endsWith(jsToString(arg(a, 0)))
    },
    "slice" to { _, t, a -> sSlice(t, a) },
    "substring" to { _, t, a -> sSubstring(t, a) },
    "substr" to { _, t, a -> sSubstr(t, a) },
    "toUpperCase" to { _, t, _ -> checkString(t.uppercase()) },
    "toLowerCase" to { _, t, _ -> t.lowercase() },
    "toLocaleUpperCase" to { _, t, _ -> checkString(t.uppercase()) },
    "toLocaleLowerCase" to { _, t, _ -> t.lowercase() },
    "trim" to { _, t, _ -> jsStrip(t) },
    "trimStart" to { _, t, _ -> jsStrip(t, start = true, end = false) },
    "trimEnd" to { _, t, _ -> jsStrip(t, start = false, end = true) },
    "padStart" to { _, t, a -> pad(t, a, true) },
    "padEnd" to { _, t, a -> pad(t, a, false) },
    "repeat" to { _, t, a -> sRepeat(t, a) },
    "split" to { _, t, a -> sSplit(t, a) },
    "replace" to { i, t, a -> sReplace(i, t, a, false) },
    "replaceAll" to { i, t, a -> sReplace(i, t, a, true) },
    "match" to ::sMatch,
    "matchAll" to ::sMatchAll,
    "search" to { i, t, a -> regexSearch(asRegExp(i, arg(a, 0)).compiled, t, 0)?.start?.toDouble() ?: -1.0 },
    "concat" to { _, t, a -> checkString(t + a.joinToString("") { jsToString(it) }) },
    "localeCompare" to { _, t, a -> cpCompare(t, jsToString(arg(a, 0))).toDouble() },
    "normalize" to { _, t, a -> normalize(t, a) },
    "toString" to { _, t, _ -> t },
    "valueOf" to { _, t, _ -> t },
).mapValues { stringMethod(it.value) }

private fun nToString(t: Any?, a: List<Any?>): Any? {
    val value = t as Double
    val radix = arg(a, 0).let { if (it === Undefined) 10.0 else toInteger(it) }
    if (radix < 2 || radix > 36) throw rangeError("toString() radix must be between 2 and 36")
    if (radix == 10.0 || value.isNaN() || value.isInfinite()) return numberToString(value)
    if (value != Math.rint(value)) throw rangeError("toString() with a radix needs a whole number")
    val text = java.math.BigDecimal(Math.abs(value)).toBigInteger().toString(radix.toInt())
    return (if (value < 0) "-" else "") + text
}

internal val NUMBER_METHODS: Map<String, NativeFn> = mapOf(
    "toFixed" to { _, t, a ->
        val digits = toInteger(arg(a, 0))
        if (digits < 0 || digits > 100) throw rangeError("toFixed() digits argument must be between 0 and 100")
        toFixed(t as Double, digits.toInt())
    },
    "toString" to { _, t, a -> nToString(t, a) },
    "valueOf" to { _, t, _ -> t },
)

private fun civilParts(ms: Double): List<Long> {
    val c = civilFromMs(ms.toLong())
    return listOf(c.year, c.month.toLong(), c.day.toLong(), c.hours.toLong(), c.minutes.toLong(), c.seconds.toLong(), c.millis.toLong(), c.weekday.toLong())
}

private fun dateField(index: Int): NativeFn = { _, t, _ ->
    val date = t as JsDate
    if (date.ms.isNaN()) Double.NaN else (civilParts(date.ms)[index] - if (index == 1) 1 else 0).toDouble()
}

private fun dateSetter(index: Int): NativeFn = { _, t, a ->
    val date = t as JsDate
    if (date.ms.isNaN() && index != 0) {
        Double.NaN
    } else {
        val parts = civilParts(if (date.ms.isNaN()) 0.0 else date.ms).take(7).map { it.toDouble() }.toMutableList()
        parts[1] -= 1
        val values = a.map { jsToNumber(it) }
        if (values.isEmpty()) {
            date.ms = Double.NaN
            Double.NaN
        } else {
            val limit = mapOf(0 to 3, 1 to 2, 2 to 1, 3 to 4, 4 to 3, 5 to 2, 6 to 1).getValue(index)
            for ((offset, value) in values.take(limit).withIndex()) parts[index + offset] = value
            date.ms = timeClip(dateFromParts(parts[0], parts[1], parts[2], parts[3], parts[4], parts[5], parts[6]))
            date.ms
        }
    }
}

internal val DATE_METHODS: Map<String, NativeFn> = LinkedHashMap<String, NativeFn>().apply {
    put("getTime") { _, t, _ -> (t as JsDate).ms }
    put("valueOf") { _, t, _ -> (t as JsDate).ms }
    put("toISOString") { _, t, _ ->
        val ms = (t as JsDate).ms
        if (ms.isNaN()) throw rangeError("Invalid time value")
        isoString(ms)
    }
    put("toJSON") { _, t, _ -> (t as JsDate).ms.let { if (it.isNaN()) null else isoString(it) } }
    put("toString") { _, t, _ -> dateToString((t as JsDate).ms) }
    put("getTimezoneOffset") { _, t, _ -> if ((t as JsDate).ms.isNaN()) Double.NaN else 0.0 }
    for (prefix in listOf("get", "getUTC")) {
        put(prefix + "FullYear", dateField(0))
        put(prefix + "Month", dateField(1))
        put(prefix + "Date", dateField(2))
        put(prefix + "Hours", dateField(3))
        put(prefix + "Minutes", dateField(4))
        put(prefix + "Seconds", dateField(5))
        put(prefix + "Milliseconds", dateField(6))
        put(prefix + "Day", dateField(7))
    }
    for (prefix in listOf("set", "setUTC")) {
        put(prefix + "FullYear", dateSetter(0))
        put(prefix + "Month", dateSetter(1))
        put(prefix + "Date", dateSetter(2))
        put(prefix + "Hours", dateSetter(3))
        put(prefix + "Minutes", dateSetter(4))
        put(prefix + "Seconds", dateSetter(5))
        put(prefix + "Milliseconds", dateSetter(6))
    }
    put("setTime") { _, t, a ->
        val date = t as JsDate
        date.ms = timeClip(jsToNumber(arg(a, 0)))
        date.ms
    }
}

internal val REGEXP_METHODS: Map<String, NativeFn> = mapOf(
    "test" to { _, t, a -> regexExec(t as JsRegExp, jsToString(arg(a, 0))) != null },
    "exec" to { _, t, a -> regexExec(t as JsRegExp, jsToString(arg(a, 0))) },
    "toString" to { _, t, _ -> "/" + (t as JsRegExp).source + "/" + t.flags },
)

private fun callableArg(interp: JsInterpreter, a: List<Any?>): Any? {
    val fn = arg(a, 0)
    if (!isCallable(fn)) throw typeError(interp.displayShort(fn) + " is not a function")
    return fn
}

private val collectionForEach: NativeFn = { i, t, a ->
    val c = t as JsCollection
    val fn = callableArg(i, a)
    for ((key, value) in c.keys.toList().zip(c.values.toList())) i.call(fn, arg(a, 1), listOf(if (c.kind == "Map") value else key, key, c))
    Undefined
}

private val collectionClear: NativeFn = { _, t, _ ->
    val c = t as JsCollection
    c.keys = ArrayList()
    c.values = ArrayList()
    c.index = HashMap()
    Undefined
}

internal val SET_METHODS: Map<String, NativeFn> = mapOf(
    "add" to { _, t, a ->
        (t as JsCollection).put(arg(a, 0), arg(a, 0))
        t
    },
    "has" to { _, t, a -> (t as JsCollection).find(arg(a, 0)) != null },
    "delete" to { _, t, a -> (t as JsCollection).remove(arg(a, 0)) },
    "clear" to collectionClear,
    "forEach" to collectionForEach,
    "values" to { _, t, _ -> JsArray((t as JsCollection).keys) },
    "keys" to { _, t, _ -> JsArray((t as JsCollection).keys) },
    "entries" to { _, t, _ -> JsArray((t as JsCollection).keys.map { JsArray(listOf(it, it)) }) },
)

internal val MAP_METHODS: Map<String, NativeFn> = mapOf(
    "get" to { _, t, a ->
        val c = t as JsCollection
        val position = c.find(arg(a, 0))
        if (position == null) Undefined else c.values[position]
    },
    "set" to { _, t, a ->
        (t as JsCollection).put(arg(a, 0), arg(a, 1))
        t
    },
    "has" to { _, t, a -> (t as JsCollection).find(arg(a, 0)) != null },
    "delete" to { _, t, a -> (t as JsCollection).remove(arg(a, 0)) },
    "clear" to collectionClear,
    "forEach" to collectionForEach,
    "values" to { _, t, _ -> JsArray((t as JsCollection).values) },
    "keys" to { _, t, _ -> JsArray((t as JsCollection).keys) },
    "entries" to { _, t, _ -> (t as JsCollection).entries() },
)

private fun makeCollection(kind: String): Native = Native(kind, construct = { interp, args ->
    val out = JsCollection(kind)
    val source = arg(args, 0)
    if (source !== Undefined && source != null) {
        for (entry in interp.iterate(source, N("ident", 0, "iterable"))) {
            if (kind == "Set") {
                out.put(entry, entry)
            } else {
                if (entry !is List<*>) throw typeError("Iterator value " + jsToString(entry) + " is not an entry object")
                out.put(arg(entry, 0), arg(entry, 1))
            }
        }
    }
    out
}) { _, _, _ -> throw typeError("Constructor $kind requires 'new'") }

internal val OBJECT_METHODS: Map<String, NativeFn> = mapOf(
    "hasOwnProperty" to { _, t, a -> (t as Map<*, *>).containsKey(propertyKey(arg(a, 0))) },
    "toString" to { _, _, _ -> "[object Object]" },
)

internal val FUNCTION_METHODS: Map<String, NativeFn> = mapOf(
    "call" to { i, t, a -> i.call(t, arg(a, 0), if (a.size > 1) a.subList(1, a.size) else emptyList()) },
    "apply" to { i, t, a ->
        var extra = arg(a, 1)
        if (extra === Undefined || extra == null) extra = emptyList<Any?>()
        if (extra !is List<*>) throw typeError("CreateListFromArrayLike called on non-object")
        i.call(t, arg(a, 0), ArrayList(extra))
    },
)

private fun unaryMath(fn: (Double) -> Double): Native = Native("math") { _, _, a ->
    val value = jsToNumber(arg(a, 0))
    if (value.isNaN()) Double.NaN else fn(value)
}

internal fun jsRound(value: Double): Double {
    if (value.isNaN() || value.isInfinite() || value == Math.rint(value)) return value
    if (value > 0 && value < 0.5) return 0.0
    if (value >= -0.5 && value < 0) return -0.0
    val floor = Math.floor(value)
    return if (value - floor >= 0.5) floor + 1 else floor
}

private fun named(name: String, native: Native): Native = Native(name, native.props, native.construct, native.fn)

private fun makeMath(): JsObject {
    val props = JsObject()
    props["PI"] = Math.PI
    props["E"] = Math.E
    props["LN2"] = Math.log(2.0)
    props["LN10"] = Math.log(10.0)
    props["LOG2E"] = 1 / Math.log(2.0)
    props["LOG10E"] = 1 / Math.log(10.0)
    props["SQRT2"] = Math.sqrt(2.0)
    props["SQRT1_2"] = Math.sqrt(0.5)
    props["abs"] = named("abs", unaryMath { Math.abs(it) })
    props["floor"] = named("floor", unaryMath { if (it.isInfinite()) it else Math.floor(it) })
    props["ceil"] = named("ceil", unaryMath { if (it.isInfinite()) it else Math.ceil(it).let { r -> if (r == 0.0 && it < 0) -0.0 else r } })
    props["round"] = named("round", unaryMath(::jsRound))
    props["trunc"] = named("trunc", unaryMath { if (it.isInfinite() || it == 0.0) it else if (it < 0) Math.ceil(it) + 0.0 else Math.floor(it) })
    props["sign"] = named("sign", unaryMath { if (it == 0.0) it else if (it > 0) 1.0 else -1.0 })
    props["sqrt"] = named("sqrt", unaryMath { if (it < 0) Double.NaN else Math.sqrt(it) })
    props["cbrt"] = named("cbrt", unaryMath { value ->
        if (value.isInfinite() || value == 0.0) {
            value
        } else {
            val result = Math.cbrt(value)
            val whole = Math.rint(result)
            if (whole * whole * whole == value) whole else result
        }
    })
    props["exp"] = named("exp", unaryMath { Math.exp(it) })
    props["log"] = named("log", unaryMath { Math.log(it) })
    props["log10"] = named("log10", unaryMath { Math.log10(it) })
    props["log2"] = named("log2", unaryMath { if (it == Double.POSITIVE_INFINITY) it else if (it == 0.0) Double.NEGATIVE_INFINITY else if (it < 0) Double.NaN else Math.log(it) / Math.log(2.0) })
    props["sin"] = named("sin", unaryMath { Math.sin(it) })
    props["cos"] = named("cos", unaryMath { Math.cos(it) })
    props["tan"] = named("tan", unaryMath { Math.tan(it) })
    props["asin"] = named("asin", unaryMath { Math.asin(it) })
    props["acos"] = named("acos", unaryMath { Math.acos(it) })
    props["atan"] = named("atan", unaryMath { Math.atan(it) })
    props["atan2"] = Native("atan2") { _, _, a -> Math.atan2(jsToNumber(arg(a, 0)), jsToNumber(arg(a, 1))) }
    props["hypot"] = Native("hypot") { _, _, a ->
        val values = a.map { jsToNumber(it) }
        if (values.any { it.isInfinite() }) {
            Double.POSITIVE_INFINITY
        } else if (values.any { it.isNaN() }) {
            Double.NaN
        } else {
            var result = 0.0
            for (v in values) result = Math.hypot(result, v)
            result
        }
    }
    props["pow"] = Native("pow") { _, _, a -> jsPow(jsToNumber(arg(a, 0)), jsToNumber(arg(a, 1))) }
    props["min"] = Native("min") { _, _, a ->
        var result = Double.POSITIVE_INFINITY
        var nan = false
        for (item in a) {
            val value = jsToNumber(item)
            if (value.isNaN()) {
                nan = true
                break
            }
            if (value < result || (value == 0.0 && result == 0.0 && 1 / value < 0)) result = value
        }
        if (nan) Double.NaN else result
    }
    props["max"] = Native("max") { _, _, a ->
        var result = Double.NEGATIVE_INFINITY
        var nan = false
        for (item in a) {
            val value = jsToNumber(item)
            if (value.isNaN()) {
                nan = true
                break
            }
            if (value > result || (value == 0.0 && result == 0.0 && 1 / value > 0)) result = value
        }
        if (nan) Double.NaN else result
    }
    props["random"] = Native("random") { i, _, _ -> i.random() }
    return props
}

private fun jsonStringifyNative(a: List<Any?>): Any? {
    val space = arg(a, 2)
    var indent = ""
    if (space is Double) indent = " ".repeat(maxOf(0.0, minOf(10.0, toInteger(space))).toInt())
    if (space is String) indent = cpSub(space, 0, 10)
    return jsonStringify(arg(a, 0), indent)
}

private fun nullCheck(value: Any?) {
    if (value == null || value === Undefined) throw typeError("Cannot convert undefined or null to object")
}

@Suppress("UNCHECKED_CAST")
private fun makeObject(): Native {
    val props = LinkedHashMap<String, Any?>()
    props["keys"] = Native("keys") { _, _, a ->
        when (val value = arg(a, 0)) {
            is Map<*, *> -> JsArray(ownKeys(value as Map<String, Any?>))
            is List<*> -> JsArray(value.indices.map { it.toString() })
            is String -> JsArray((0 until cpLen(value)).map { it.toString() })
            else -> {
                nullCheck(value)
                JsArray()
            }
        }
    }
    props["values"] = Native("values") { _, _, a ->
        when (val value = arg(a, 0)) {
            is Map<*, *> -> JsArray(ownKeys(value as Map<String, Any?>).map { value[it] })
            is List<*> -> JsArray(value)
            is String -> JsArray(cpList(value))
            else -> {
                nullCheck(value)
                JsArray()
            }
        }
    }
    props["entries"] = Native("entries") { _, _, a ->
        when (val value = arg(a, 0)) {
            is Map<*, *> -> JsArray(ownKeys(value as Map<String, Any?>).map { JsArray(listOf(it, value[it])) })
            is List<*> -> JsArray(value.mapIndexed { index, item -> JsArray(listOf(index.toString(), item)) })
            is String -> JsArray(cpList(value).mapIndexed { index, item -> JsArray(listOf(index.toString(), item)) })
            else -> {
                nullCheck(value)
                JsArray()
            }
        }
    }
    props["assign"] = Native("assign") { _, _, a ->
        val target = arg(a, 0)
        if (target !is MutableMap<*, *>) {
            nullCheck(target)
            target
        } else {
            val out = target as MutableMap<String, Any?>
            for (source in a.drop(1)) {
                when (source) {
                    is Map<*, *> -> for (key in ownKeys(source as Map<String, Any?>)) out[key] = source[key]
                    is List<*> -> for ((index, item) in source.withIndex()) out[index.toString()] = item
                    is String -> for ((index, item) in cpList(source).withIndex()) out[index.toString()] = item
                }
            }
            target
        }
    }
    props["fromEntries"] = Native("fromEntries") { i, _, a ->
        val out = JsObject()
        for (entry in i.iterate(arg(a, 0), N("ident", 0, "entries"))) {
            if (entry !is List<*>) throw typeError("Iterator value " + jsToString(entry) + " is not an entry object")
            out[propertyKey(arg(entry, 0))] = arg(entry, 1)
        }
        out
    }
    props["freeze"] = Native("freeze") { _, _, a -> arg(a, 0) }
    props["hasOwn"] = Native("hasOwn") { _, _, a -> val v = arg(a, 0); v is Map<*, *> && v.containsKey(propertyKey(arg(a, 1))) }
    val construct: (JsInterpreter, List<Any?>) -> Any? = { _, args -> arg(args, 0).let { if (isJsObject(it)) it else JsObject() } }
    return Native("Object", props, construct) { i, _, a -> construct(i, a) }
}

private fun newArray(a: List<Any?>): Any? {
    if (a.size == 1 && a[0] is Double) {
        val length = a[0] as Double
        if (length < 0 || length != Math.rint(length) || length > JsLimits.MAX_ARRAY) throw rangeError("Invalid array length")
        return JsArray(List(length.toInt()) { Undefined })
    }
    return JsArray(a)
}

@Suppress("UNCHECKED_CAST")
private fun arrayFrom(i: JsInterpreter, a: List<Any?>): Any? {
    val source = arg(a, 0)
    val fn = arg(a, 1)
    var items: List<Any?> = when (source) {
        is List<*> -> ArrayList(source)
        is String -> cpList(source)
        is JsCollection -> source.entries()
        is Map<*, *> -> if (source.containsKey("length")) {
            val length = toInteger(source["length"])
            if (length > JsLimits.MAX_ARRAY) throw rangeError("Invalid array length")
            (0 until maxOf(0.0, length).toInt()).map { if ((source as Map<String, Any?>).containsKey(it.toString())) source[it.toString()] else Undefined }
        } else {
            emptyList()
        }
        null, Undefined -> throw typeError(jsToString(source) + " is not iterable")
        else -> emptyList()
    }
    if (fn !== Undefined) {
        if (!isCallable(fn)) throw typeError(i.displayShort(fn) + " is not a function")
        items = items.mapIndexed { index, item -> i.call(fn, Undefined, listOf(item, index.toDouble())) }
    }
    return JsArray(items)
}

private fun makeArray(): Native {
    val props = LinkedHashMap<String, Any?>()
    props["isArray"] = Native("isArray") { _, _, a -> arg(a, 0) is List<*> }
    props["from"] = Native("from") { i, _, a -> arrayFrom(i, a) }
    props["of"] = Native("of") { _, _, a -> JsArray(a) }
    return Native("Array", props, { _, args -> newArray(args) }) { _, _, a -> newArray(a) }
}

internal fun parseFloatJs(value: Any?): Double {
    val text = jsStrip(jsToString(value), start = true, end = false)
    val match = FLOAT_PREFIX_RE.find(text) ?: return Double.NaN
    val token = match.value
    if (token.trimStart('+', '-') == "Infinity") return if (token.startsWith("-")) Double.NEGATIVE_INFINITY else Double.POSITIVE_INFINITY
    return token.toDouble()
}

internal fun parseIntJs(value: Any?, radixValue: Any?): Double {
    var text = jsStrip(jsToString(value))
    var radix = toInt32(radixValue).toInt()
    var sign = 1
    if (text.startsWith("+") || text.startsWith("-")) {
        sign = if (text[0] == '-') -1 else 1
        text = text.substring(1)
    }
    var stripPrefix = true
    if (radix != 0) {
        if (radix < 2 || radix > 36) return Double.NaN
        if (radix != 16) stripPrefix = false
    } else {
        radix = 10
    }
    if (stripPrefix && text.take(2).lowercase() == "0x") {
        text = text.substring(2)
        radix = 16
    }
    val digits = "0123456789abcdefghijklmnopqrstuvwxyz".substring(0, radix)
    var end = 0
    while (end < text.length && digits.indexOf(text[end].lowercaseChar()) >= 0) end++
    if (end == 0) return Double.NaN
    val number = java.math.BigInteger(text.substring(0, end), radix).toDouble()
    return if (sign < 0) -number else number
}

private fun isWhole(value: Any?): Boolean = value is Double && !value.isNaN() && !value.isInfinite() && value == Math.rint(value)

private fun makeNumber(): Native {
    val props = LinkedHashMap<String, Any?>()
    props["isInteger"] = Native("isInteger") { _, _, a -> isWhole(arg(a, 0)) }
    props["isSafeInteger"] = Native("isSafeInteger") { _, _, a -> isWhole(arg(a, 0)) && Math.abs(a[0] as Double) <= 9007199254740991.0 }
    props["isFinite"] = Native("isFinite") { _, _, a -> val v = arg(a, 0); v is Double && !v.isNaN() && !v.isInfinite() }
    props["isNaN"] = Native("isNaN") { _, _, a -> val v = arg(a, 0); v is Double && v.isNaN() }
    props["parseFloat"] = Native("parseFloat") { _, _, a -> parseFloatJs(arg(a, 0)) }
    props["parseInt"] = Native("parseInt") { _, _, a -> parseIntJs(arg(a, 0), arg(a, 1)) }
    props["MAX_SAFE_INTEGER"] = 9007199254740991.0
    props["MIN_SAFE_INTEGER"] = -9007199254740991.0
    props["EPSILON"] = Math.pow(2.0, -52.0)
    props["MAX_VALUE"] = Double.MAX_VALUE
    props["MIN_VALUE"] = java.lang.Double.MIN_VALUE
    props["POSITIVE_INFINITY"] = Double.POSITIVE_INFINITY
    props["NEGATIVE_INFINITY"] = Double.NEGATIVE_INFINITY
    props["NaN"] = Double.NaN
    return Native("Number", props) { _, _, a -> if (a.isNotEmpty()) jsToNumber(a[0]) else 0.0 }
}

private fun makeString(): Native {
    val props = LinkedHashMap<String, Any?>()
    props["fromCharCode"] = Native("fromCharCode") { _, _, a ->
        val out = StringBuilder()
        for (item in a) out.append((toUint32(item) % 65536).toInt().toChar())
        out.toString()
    }
    props["fromCodePoint"] = Native("fromCodePoint") { _, _, a ->
        val out = StringBuilder()
        for (item in a) {
            val value = jsToNumber(item)
            if (value.isNaN() || value != Math.rint(value) || value < 0 || value > 0x10FFFF) throw rangeError("Invalid code point " + jsToString(item))
            out.appendCodePoint(value.toInt())
        }
        out.toString()
    }
    return Native("String", props) { _, _, a -> if (a.isNotEmpty()) jsToString(a[0]) else "" }
}

private fun makeError(name: String): Native {
    val construct: (JsInterpreter, List<Any?>) -> Any? = { _, args ->
        val message = arg(args, 0)
        errorObject(name, if (message === Undefined) "" else jsToString(message))
    }
    return Native(name, LinkedHashMap(), construct) { i, _, a -> construct(i, a) }
}

private fun newDate(interp: JsInterpreter, args: List<Any?>): Any? {
    if (args.isEmpty()) return JsDate(interp.nowMs)
    if (args.size == 1) {
        val value = args[0]
        if (value is JsDate) return JsDate(value.ms)
        val primitive = toPrimitive(value)
        if (primitive is String) return JsDate(parseDateString(primitive))
        return JsDate(jsToNumber(primitive))
    }
    val numbers = args.take(7).map { jsToNumber(it) }.toMutableList()
    val year = numbers[0]
    if (!year.isNaN()) {
        val whole = if (year < 0) Math.ceil(year) else Math.floor(year)
        if (whole in 0.0..99.0) numbers[0] = 1900 + whole
    }
    return JsDate(datePartsOf(numbers))
}

private fun datePartsOf(numbers: List<Double>): Double {
    fun at(i: Int, default: Double) = if (i < numbers.size) numbers[i] else default
    return dateFromParts(at(0, Double.NaN), at(1, 0.0), at(2, 1.0), at(3, 0.0), at(4, 0.0), at(5, 0.0), at(6, 0.0))
}

private fun makeDate(): Native {
    val props = LinkedHashMap<String, Any?>()
    props["now"] = Native("now") { i, _, _ -> i.nowMs }
    props["parse"] = Native("parse") { _, _, a -> parseDateString(jsToString(arg(a, 0))) }
    props["UTC"] = Native("UTC") { _, _, a ->
        val numbers = a.take(7).map { jsToNumber(it) }.toMutableList()
        if (numbers.isEmpty()) numbers.add(Double.NaN)
        val year = numbers[0]
        if (!year.isNaN()) {
            val whole = if (year < 0) Math.ceil(year) else Math.floor(year)
            if (whole in 0.0..99.0) numbers[0] = 1900 + whole
        }
        if (numbers.size == 1) numbers.add(0.0)
        timeClip(datePartsOf(numbers))
    }
    return Native("Date", props, ::newDate) { i, _, _ -> dateToString(i.nowMs) }
}

private fun newRegExp(interp: JsInterpreter, args: List<Any?>): Any? {
    val pattern = arg(args, 0)
    val rawFlags = arg(args, 1)
    val source: String
    val flags: String
    if (pattern is JsRegExp) {
        source = pattern.source
        flags = if (rawFlags === Undefined) pattern.flags else jsToString(rawFlags)
    } else {
        source = if (pattern === Undefined) "(?:)" else jsToString(pattern)
        flags = if (rawFlags === Undefined) "" else jsToString(rawFlags)
    }
    val compiled = try {
        compileRegex(source, flags, interp.line)
    } catch (error: ScriptError) {
        throw Thrown(errorObject("SyntaxError", error.text))
    }
    return JsRegExp(source, flags, compiled)
}

internal fun makeGlobals(interp: JsInterpreter): Map<String, Any?> {
    val console = JsObject()
    for (name in listOf("log", "info", "warn", "error", "debug")) console[name] = Native(name) { i, _, a ->
        i.log(a)
        Undefined
    }
    val json = JsObject()
    json["parse"] = Native("parse") { _, _, a -> jsonParse(jsToString(arg(a, 0))) }
    json["stringify"] = Native("stringify") { _, _, a -> jsonStringifyNative(a) }
    return linkedMapOf(
        "undefined" to Undefined,
        "NaN" to Double.NaN,
        "Infinity" to Double.POSITIVE_INFINITY,
        "Math" to makeMath(),
        "JSON" to json,
        "Object" to makeObject(),
        "Array" to makeArray(),
        "Number" to makeNumber(),
        "String" to makeString(),
        "Boolean" to Native("Boolean") { _, _, a -> jsTruthy(arg(a, 0)) },
        "Date" to makeDate(),
        "RegExp" to Native("RegExp", LinkedHashMap(), ::newRegExp) { i, _, a -> newRegExp(i, a) },
        "Set" to makeCollection("Set"),
        "Map" to makeCollection("Map"),
        "Function" to Native("Function") { _, _, _ -> throw typeError("Function is not a function") },
        "Error" to makeError("Error"),
        "TypeError" to makeError("TypeError"),
        "RangeError" to makeError("RangeError"),
        "SyntaxError" to makeError("SyntaxError"),
        "ReferenceError" to makeError("ReferenceError"),
        "URIError" to makeError("URIError"),
        "parseInt" to Native("parseInt") { _, _, a -> parseIntJs(arg(a, 0), arg(a, 1)) },
        "parseFloat" to Native("parseFloat") { _, _, a -> parseFloatJs(arg(a, 0)) },
        "isNaN" to Native("isNaN") { _, _, a -> jsToNumber(arg(a, 0)).isNaN() },
        "isFinite" to Native("isFinite") { _, _, a -> jsToNumber(arg(a, 0)).let { !it.isNaN() && !it.isInfinite() } },
        "encodeURIComponent" to Native("encodeURIComponent") { _, _, a -> uriEncode(jsToString(arg(a, 0))) },
        "decodeURIComponent" to Native("decodeURIComponent") { _, _, a -> uriDecode(jsToString(arg(a, 0))) },
        "console" to console,
    )
}
