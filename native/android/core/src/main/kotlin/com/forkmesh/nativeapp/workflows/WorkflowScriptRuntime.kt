package com.forkmesh.nativeapp.workflows

import java.util.IdentityHashMap

internal typealias NativeFn = (JsInterpreter, Any?, List<Any?>) -> Any?

internal fun isCallable(value: Any?): Boolean = value is JsFunction || value is Native

internal fun typeOf(value: Any?): String = when {
    value === Undefined -> "undefined"
    value == null -> "object"
    value is Boolean -> "boolean"
    value is Double -> "number"
    value is String -> "string"
    isCallable(value) -> "function"
    else -> "object"
}

internal fun jsTruthy(value: Any?): Boolean = when (value) {
    null, Undefined -> false
    is Boolean -> value
    is Double -> !(value == 0.0 || value.isNaN())
    is String -> value.isNotEmpty()
    else -> true
}

internal fun jsToNumber(value: Any?): Double = when (value) {
    is Double -> value
    is Boolean -> if (value) 1.0 else 0.0
    null -> 0.0
    Undefined -> Double.NaN
    is String -> stringToNumber(value)
    is JsDate -> value.ms
    is List<*> -> stringToNumber(jsToString(value))
    else -> Double.NaN
}

internal fun toInteger(value: Any?): Double {
    val number = jsToNumber(value)
    if (number.isNaN()) return 0.0
    if (number.isInfinite()) return number
    return if (number < 0) Math.ceil(number) + 0.0 else Math.floor(number)
}

internal fun wrap32(n: Long): Long {
    val m = Math.floorMod(n, 4294967296L)
    return if (m >= 2147483648L) m - 4294967296L else m
}

private fun mod32(number: Double): Long {
    val t = if (number < 0) Math.ceil(number) else Math.floor(number)
    var r = t % 4294967296.0
    if (r < 0) r += 4294967296.0
    return r.toLong()
}

internal fun toInt32(value: Any?): Long {
    val number = jsToNumber(value)
    if (number.isNaN() || number.isInfinite()) return 0
    return wrap32(mod32(number))
}

internal fun toUint32(value: Any?): Long {
    val number = jsToNumber(value)
    if (number.isNaN() || number.isInfinite()) return 0
    return mod32(number)
}

internal fun jsToString(value: Any?, seen: IdentityHashMap<Any, Boolean>? = null): String = when (value) {
    is String -> value
    is Double -> numberToString(value)
    is Boolean -> if (value) "true" else "false"
    null -> "null"
    Undefined -> "undefined"
    is List<*> -> {
        val marks = seen ?: IdentityHashMap()
        if (marks.containsKey(value)) {
            ""
        } else {
            marks[value] = true
            try {
                joinList(value, ",", marks)
            } finally {
                marks.remove(value)
            }
        }
    }
    is JsDate -> dateToString(value.ms)
    is JsRegExp -> "/" + value.source + "/" + value.flags
    is JsFunction -> "function " + value.name + "() { [code] }"
    is Native -> "function " + value.name + "() { [native code] }"
    is JsCollection -> "[object " + value.kind + "]"
    else -> "[object Object]"
}

internal fun joinList(items: List<*>, separator: String, seen: IdentityHashMap<Any, Boolean>? = null): String {
    val out = StringBuilder()
    var total = 0L
    val sepLen = cpLen(separator)
    for ((i, item) in items.withIndex()) {
        val text = if (item == null || item === Undefined) "" else jsToString(item, seen)
        total += cpLen(text) + sepLen
        if (total > JsLimits.MAX_STRING + sepLen) throw rangeError("Invalid string length")
        if (i > 0) out.append(separator)
        out.append(text)
    }
    return out.toString()
}

internal fun toPrimitive(value: Any?, hint: String = "default"): Any? = when (value) {
    is JsDate -> if (hint == "number") value.ms else dateToString(value.ms)
    is List<*>, is Map<*, *>, is JsFunction, is Native, is JsRegExp, is JsCollection -> jsToString(value)
    else -> value
}

internal fun propertyKey(value: Any?): String = if (value is String) value else jsToString(value)

internal fun arrayIndex(key: Any?): Int? {
    if (key is Double) {
        if (key == Math.rint(key) && key >= 0 && key < 4294967295.0) return if (key > Int.MAX_VALUE) Int.MAX_VALUE else key.toInt()
        return null
    }
    if (key is String && isIndexKey(key)) {
        val number = key.toLong()
        return if (number > Int.MAX_VALUE) Int.MAX_VALUE else number.toInt()
    }
    return null
}

internal fun strictEquals(a: Any?, b: Any?): Boolean {
    if (a is Double && b is Double) return a == b
    if (a is String && b is String) return a == b
    if (a is Boolean && b is Boolean) return a == b
    return a === b
}

internal fun sameValueZero(a: Any?, b: Any?): Boolean {
    if (a is Double && b is Double) return a == b || (a.isNaN() && b.isNaN())
    return strictEquals(a, b)
}

internal fun isJsObject(value: Any?): Boolean =
    value is List<*> || value is Map<*, *> || value is JsDate || value is JsRegExp || value is JsFunction || value is Native || value is JsCollection

internal fun looseEquals(a: Any?, b: Any?): Boolean {
    val aNull = a == null || a === Undefined
    val bNull = b == null || b === Undefined
    if (aNull || bNull) return aNull && bNull
    if (typeOf(a) == typeOf(b)) return strictEquals(a, b)
    if (a is Double && b is String) return a == stringToNumber(b)
    if (a is String && b is Double) return stringToNumber(a) == b
    if (a is Boolean) return looseEquals(if (a) 1.0 else 0.0, b)
    if (b is Boolean) return looseEquals(a, if (b) 1.0 else 0.0)
    if (isJsObject(a) && !isJsObject(b)) return looseEquals(toPrimitive(a), b)
    if (isJsObject(b) && !isJsObject(a)) return looseEquals(a, toPrimitive(b))
    return false
}

internal fun jsPow(a: Double, b: Double): Double {
    if (b.isNaN()) return Double.NaN
    if (b == 0.0) return 1.0
    if (a.isNaN()) return Double.NaN
    if (Math.abs(a) == 1.0 && b.isInfinite()) return Double.NaN
    return StrictMath.pow(a, b)
}

internal fun jsMod(a: Double, b: Double): Double {
    if (b == 0.0 || a.isNaN() || b.isNaN() || a.isInfinite()) return Double.NaN
    if (b.isInfinite()) return a
    if (a == 0.0) return a
    return a % b
}

internal fun checkString(text: String): String {
    if (text.length > JsLimits.MAX_STRING && cpLen(text) > JsLimits.MAX_STRING) throw rangeError("Invalid string length")
    return text
}

internal fun <T : MutableList<*>> checkArray(items: T): T {
    if (items.size > JsLimits.MAX_ARRAY) throw rangeError("Invalid array length")
    return items
}

internal fun relative(value: Any?, length: Int, default: Int): Int {
    if (value === Undefined) return default
    val number = toInteger(value)
    if (number == Double.NEGATIVE_INFINITY) return 0
    if (number == Double.POSITIVE_INFINITY) return length
    if (number < 0) return maxOf(length + number, 0.0).toInt()
    return minOf(number, length.toDouble()).toInt()
}

internal fun clampIndex(value: Any?, length: Int): Int {
    val number = toInteger(value)
    if (number == Double.POSITIVE_INFINITY) return length
    if (number == Double.NEGATIVE_INFINITY) return 0
    return maxOf(0.0, minOf(number, length.toDouble())).toInt()
}

internal fun jsonStringify(value: Any?, indent: String = ""): Any? {
    val seen = ArrayList<Any>()
    var budget = 0L
    fun emit(text: String): String {
        budget += cpLen(text)
        if (budget > JsLimits.MAX_STRING) throw rangeError("Invalid string length")
        return text
    }
    fun serialize(raw: Any?, current: String): String? {
        var item = raw
        if (item is JsDate) item = if (item.ms.isNaN()) null else isoString(item.ms)
        when {
            item == null -> return emit("null")
            item == true -> return emit("true")
            item == false -> return emit("false")
            item is String -> return emit(jsJsonQuote(item))
            item is Double -> return if (item.isNaN() || item.isInfinite()) emit("null") else emit(numberToString(item))
            item === Undefined || isCallable(item) -> return null
            item is JsRegExp || item is JsCollection -> return emit("{}")
        }
        if (seen.any { it === item }) throw typeError("Converting circular structure to JSON")
        seen.add(item!!)
        val inner = current + indent
        try {
            if (item is List<*>) {
                val parts = item.map { serialize(it, inner) ?: "null" }
                if (parts.isEmpty()) return "[]"
                if (indent.isNotEmpty()) return "[\n" + inner + parts.joinToString(",\n$inner") + "\n" + current + "]"
                return "[" + parts.joinToString(",") + "]"
            }
            if (item is Map<*, *>) {
                @Suppress("UNCHECKED_CAST")
                val obj = item as Map<String, Any?>
                val parts = ArrayList<String>()
                for (key in ownKeys(obj)) {
                    val text = serialize(obj[key], inner) ?: continue
                    parts.add(jsJsonQuote(key) + (if (indent.isNotEmpty()) ": " else ":") + text)
                }
                if (parts.isEmpty()) return "{}"
                if (indent.isNotEmpty()) return "{\n" + inner + parts.joinToString(",\n$inner") + "\n" + current + "}"
                return "{" + parts.joinToString(",") + "}"
            }
            return "{}"
        } finally {
            seen.removeAt(seen.size - 1)
        }
    }
    return serialize(value, "") ?: Undefined
}

internal fun jsonParse(text: String): Any? = try {
    fromJson(PyJson.loads(text))
} catch (_: PyJson.Bad) {
    throw Thrown(errorObject("SyntaxError", "JSON.parse: the text is not valid JSON"))
}

internal fun arg(args: List<Any?>, index: Int): Any? = if (index < args.size) args[index] else Undefined

internal fun native(name: String, fn: (JsInterpreter, List<Any?>) -> Any?): Native = Native(name) { interp, _, args -> fn(interp, args) }

internal fun unwrapItems(raw: Any?, fallback: Any?): List<Map<String, Any?>> {
    var value = if (raw === Undefined) fallback else raw
    if (value == null) return emptyList()
    if (value is Map<*, *>) value = listOf(value)
    if (value !is List<*>) return listOf(linkedMapOf("value" to toJsonValue(value)))
    val out = ArrayList<Map<String, Any?>>()
    for (entry in value) {
        var item = entry
        if (item is Map<*, *> && item["json"] is Map<*, *> && item.keys.all { it in setOf("json", "binary", "pairedItem", "index") }) item = item["json"]
        val converted = toJsonValue(item)
        @Suppress("UNCHECKED_CAST")
        out.add(if (converted is Map<*, *>) converted as Map<String, Any?> else linkedMapOf("value" to converted))
    }
    return out
}

@Suppress("UNCHECKED_CAST")
internal class JsInterpreter(
    nowMs: Long = 0,
    val random: () -> Double = { 0.5 },
    val clock: (() -> Long)? = null,
    val deadline: Long? = null,
) {
    val nowMs: Double = nowMs.toDouble()
    var steps = 0
    var depth = 0
    var line = 1
    val logs = ArrayList<String>()
    val globals = Scope()

    init {
        for ((name, value) in makeGlobals(this)) globals.declare(name, value, "const")
    }

    fun tick() {
        steps++
        if (steps > JsLimits.MAX_STEPS) throw ScriptLimit("The script ran more than ${JsLimits.MAX_STEPS} steps")
        if (steps % 1000 == 0 && clock != null && deadline != null && clock.invoke() > deadline) throw ScriptLimit("This run ran out of time")
    }

    fun run(program: List<N>, values: Map<String, Any?>): Any? {
        val scope = Scope(globals)
        for ((name, value) in values) scope.declare(name, value, if (name.startsWith("$")) "const" else "let")
        val body = Scope(scope)
        body.declare("this", Undefined, "const")
        line = 1
        val signal = try {
            execBlock(program, body)
        } catch (thrown: Thrown) {
            throw ScriptError(if (thrown.line != 0) thrown.line else line, describeError(thrown.value))
        } catch (limit: ScriptLimit) {
            throw ScriptError(line, limit.message ?: "")
        } catch (_: StackOverflowError) {
            throw ScriptError(line, "Maximum call stack size exceeded")
        }
        if (signal is ReturnSignal) return signal.value
        return Undefined
    }

    fun describeError(value: Any?): String {
        if (value is Map<*, *> && value.containsKey("message")) {
            val name = value["name"]
            val message = jsToString(value["message"])
            if (name is String && name.isNotEmpty() && name != "Error") return if (message.isNotEmpty()) "$name: $message" else name
            return message.ifEmpty { "Error" }
        }
        return "Uncaught " + display(value)
    }

    fun display(value: Any?): String {
        if (value is String) return value
        if (value is List<*> || value is Map<*, *>) {
            val text = try {
                jsonStringify(value)
            } catch (_: Thrown) {
                return "[object]"
            }
            return if (text === Undefined) "undefined" else text as String
        }
        return jsToString(value)
    }

    fun log(args: List<Any?>) {
        if (logs.size >= JsLimits.MAX_LOGS) return
        var text = args.joinToString(" ") { display(it) }
        if (cpLen(text) > JsLimits.MAX_LOG_CHARS) text = cpSub(text, 0, JsLimits.MAX_LOG_CHARS) + "…"
        logs.add(text)
    }

    fun execBlock(statements: List<N>, scope: Scope): Any? {
        for (statement in statements) {
            if (statement.k == "funcdecl") scope.declare(statement.a as String, makeFunction(statement.b as N, scope), "var")
        }
        for (statement in statements) {
            val signal = exec(statement, scope)
            if (signal != null) return signal
        }
        return null
    }

    fun exec(statement: N, scope: Scope): Any? {
        line = statement.line
        tick()
        try {
            return when (statement.k) {
                "expr" -> {
                    eval(statement.a as N, scope)
                    null
                }
                "var" -> sVar(statement, scope)
                "if" -> if (jsTruthy(eval(statement.a as N, scope))) exec(statement.b as N, scope) else statement.c?.let { exec(it as N, scope) }
                "block" -> execBlock(statement.a as List<N>, Scope(scope))
                "for" -> sFor(statement, scope)
                "forof" -> sForOf(statement, scope)
                "forin" -> sForIn(statement, scope)
                "while" -> sWhile(statement, scope)
                "dowhile" -> sDoWhile(statement, scope)
                "return" -> ReturnSignal(if (statement.a == null) Undefined else eval(statement.a as N, scope))
                "break" -> BreakSignal
                "continue" -> ContinueSignal
                "throw" -> throw Thrown(eval(statement.a as N, scope), statement.line)
                "try" -> sTry(statement, scope)
                "switch" -> sSwitch(statement, scope)
                else -> null
            }
        } catch (thrown: Thrown) {
            if (thrown.line == 0) thrown.line = statement.line
            throw thrown
        }
    }

    private fun sVar(statement: N, scope: Scope): Any? {
        val kind = statement.a as String
        for ((target, init) in statement.b as List<Pair<N, N?>>) {
            val value = if (init == null) Undefined else eval(init, scope)
            if (init != null && init.k == "func" && target.k == "pid" && value is JsFunction && value.name.isEmpty()) value.name = target.a as String
            bind(target, value, scope, kind)
        }
        return null
    }

    private fun sWhile(statement: N, scope: Scope): Any? {
        while (jsTruthy(eval(statement.a as N, scope))) {
            val signal = exec(statement.b as N, scope)
            if (signal === BreakSignal) break
            if (signal === ContinueSignal || signal == null) {
                tick()
                continue
            }
            return signal
        }
        return null
    }

    private fun sDoWhile(statement: N, scope: Scope): Any? {
        while (true) {
            val signal = exec(statement.a as N, scope)
            if (signal === BreakSignal) break
            if (signal != null && signal !== ContinueSignal) return signal
            tick()
            if (!jsTruthy(eval(statement.b as N, scope))) break
        }
        return null
    }

    private fun sFor(statement: N, scope: Scope): Any? {
        val init = statement.a as N?
        val test = statement.b as N?
        val update = statement.c as N?
        val body = statement.d as N
        var loop = Scope(scope)
        var perIteration = false
        if (init != null) {
            if (init.k == "var") {
                sVar(init, loop)
                perIteration = init.a == "let" || init.a == "const"
            } else {
                eval(init.a as N, loop)
            }
        }
        while (true) {
            if (test != null && !jsTruthy(eval(test, loop))) break
            val signal = exec(body, loop)
            if (signal === BreakSignal) break
            if (signal != null && signal !== ContinueSignal) return signal
            tick()
            if (perIteration) {
                val fresh = Scope(scope)
                fresh.vars = HashMap(loop.vars)
                fresh.consts = HashSet(loop.consts)
                loop = fresh
            }
            if (update != null) eval(update, loop)
        }
        return null
    }

    fun iterate(value: Any?, node: N?): List<Any?> = when (value) {
        is List<*> -> value
        is String -> cpList(value)
        is JsCollection -> value.entries()
        else -> throw typeError(describeNode(node!!) + " is not iterable")
    }

    private fun sForOf(statement: N, scope: Scope): Any? {
        val kind = statement.a as String?
        val target = statement.b as N
        val subject = statement.c as N
        val sequence = iterate(eval(subject, scope), subject)
        var index = 0
        while (index < sequence.size) {
            val item = sequence[index]
            index++
            val inner = Scope(scope)
            bind(target, item, inner, kind)
            val signal = exec(statement.d as N, inner)
            if (signal === BreakSignal) break
            if (signal != null && signal !== ContinueSignal) return signal
            tick()
        }
        return null
    }

    private fun sForIn(statement: N, scope: Scope): Any? {
        val kind = statement.a as String?
        val target = statement.b as N
        val value = eval(statement.c as N, scope)
        val keys = when (value) {
            is Map<*, *> -> ownKeys(value as Map<String, Any?>)
            is List<*> -> value.indices.map { it.toString() }
            is String -> (0 until cpLen(value)).map { it.toString() }
            else -> emptyList()
        }
        for (key in keys) {
            if (value is Map<*, *> && !value.containsKey(key)) continue
            val inner = Scope(scope)
            bind(target, key, inner, kind)
            val signal = exec(statement.d as N, inner)
            if (signal === BreakSignal) break
            if (signal != null && signal !== ContinueSignal) return signal
            tick()
        }
        return null
    }

    private fun sTry(statement: N, scope: Scope): Any? {
        val block = statement.a as List<N>
        val param = statement.b as N?
        val handler = statement.c as List<N>?
        val final = statement.d as List<N>?
        var signal: Any?
        try {
            signal = execBlock(block, Scope(scope))
        } catch (thrown: Thrown) {
            if (handler == null) {
                if (final != null) {
                    val extra = execBlock(final, Scope(scope))
                    if (extra != null) return extra
                }
                throw thrown
            }
            val inner = Scope(scope)
            if (param != null) bind(param, thrown.value, inner, "let")
            try {
                signal = execBlock(handler, inner)
            } catch (again: Thrown) {
                if (final != null) {
                    val extra = execBlock(final, Scope(scope))
                    if (extra != null) return extra
                }
                throw again
            }
        }
        if (final != null) {
            val extra = execBlock(final, Scope(scope))
            if (extra != null) return extra
        }
        return signal
    }

    private fun sSwitch(statement: N, scope: Scope): Any? {
        val value = eval(statement.a as N, scope)
        val cases = statement.b as List<Pair<N?, List<N>>>
        var start: Int? = null
        for ((index, case) in cases.withIndex()) {
            if (case.first != null && strictEquals(value, eval(case.first!!, scope))) {
                start = index
                break
            }
        }
        if (start == null) {
            for ((index, case) in cases.withIndex()) {
                if (case.first == null) {
                    start = index
                    break
                }
            }
        }
        if (start == null) return null
        val statements = cases.subList(start, cases.size).flatMap { it.second }
        val signal = execBlock(statements, Scope(scope))
        if (signal === BreakSignal) return null
        return signal
    }

    fun bind(target: N, value: Any?, scope: Scope, kind: String?) {
        when (target.k) {
            "pid" -> {
                if (kind == null) assignName(target.a as String, value, scope) else scope.declare(target.a as String, value, kind)
            }
            "pexpr" -> assignTarget(target.a as N, value, scope)
            "parr" -> {
                if (value !is List<*> && value !is String && value !is JsCollection) throw typeError(displayShort(value) + " is not iterable")
                val sequence = iterate(value, null)
                val elements = target.a as List<Pair<N, N?>?>
                for ((index, element) in elements.withIndex()) {
                    if (element == null) continue
                    var item = if (index < sequence.size) sequence[index] else Undefined
                    if (item === Undefined && element.second != null) item = eval(element.second!!, scope)
                    bind(element.first, item, scope, kind)
                }
                if (target.b != null) bind(target.b as N, JsArray(if (elements.size < sequence.size) sequence.subList(elements.size, sequence.size) else emptyList()), scope, kind)
            }
            "pobj" -> {
                if (value == null || value === Undefined) throw typeError("Cannot destructure " + jsToString(value))
                val used = ArrayList<String>()
                for ((key, pattern, default) in target.a as List<Triple<String, N, N?>>) {
                    used.add(key)
                    var item = get(value, key)
                    if (item === Undefined && default != null) {
                        item = eval(default, scope)
                        if (default.k == "func" && item is JsFunction && item.name.isEmpty() && pattern.k == "pid") item.name = pattern.a as String
                    }
                    bind(pattern, item, scope, kind)
                }
                if (target.b != null) {
                    val rest = JsObject()
                    if (value is Map<*, *>) {
                        for (key in ownKeys(value as Map<String, Any?>)) if (key !in used) rest[key] = value[key]
                    }
                    bind(target.b as N, rest, scope, kind)
                }
            }
            else -> throw typeError("Invalid assignment target")
        }
    }

    fun displayShort(value: Any?): String = when {
        value === Undefined -> "undefined"
        value == null -> "null"
        else -> typeOf(value)
    }

    private fun assignName(name: String, value: Any?, scope: Scope) {
        val owner = scope.lookup(name) ?: throw Thrown(errorObject("ReferenceError", "$name is not defined"))
        if (name in owner.consts) throw typeError("Assignment to constant variable.")
        owner.vars[name] = value
    }

    private fun assignTarget(node: N, value: Any?, scope: Scope) {
        when (node.k) {
            "ident" -> assignName(node.a as String, value, scope)
            "member" -> put(eval(node.a as N, scope), node.b, value)
            "index" -> {
                val obj = eval(node.a as N, scope)
                val key = eval(node.b as N, scope)
                put(obj, key, value)
            }
            else -> throw typeError("Invalid assignment target")
        }
    }

    fun eval(node: N, scope: Scope): Any? = when (node.k) {
        "lit" -> node.a
        "ident" -> eIdent(node, scope)
        "tmpl" -> eTmpl(node, scope)
        "regex" -> JsRegExp(node.a as String, node.b as String, node.c as CompiledRegex)
        "array" -> eArray(node, scope)
        "object" -> eObject(node, scope)
        "member" -> {
            val obj = eval(node.a as N, scope)
            if (node.c == true && (obj == null || obj === Undefined)) throw ShortCircuit
            get(obj, node.b, node.a as N)
        }
        "index" -> {
            val obj = eval(node.a as N, scope)
            if (node.c == true && (obj == null || obj === Undefined)) throw ShortCircuit
            get(obj, eval(node.b as N, scope), node.a as N)
        }
        "call" -> eCall(node, scope)
        "chain" -> try {
            eval(node.a as N, scope)
        } catch (_: ShortCircuit) {
            Undefined
        }
        "new" -> eNew(node, scope)
        "unary" -> eUnary(node, scope)
        "update" -> eUpdate(node, scope)
        "bin" -> binary(node.a as String, eval(node.b as N, scope), eval(node.c as N, scope))
        "logic" -> eLogic(node, scope)
        "cond" -> if (jsTruthy(eval(node.a as N, scope))) eval(node.b as N, scope) else eval(node.c as N, scope)
        "assign" -> eAssign(node, scope)
        "func" -> makeFunction(node, scope)
        "seq" -> {
            var value: Any? = Undefined
            for (item in node.a as List<N>) value = eval(item, scope)
            value
        }
        "this" -> scope.lookup("this")?.vars?.get("this") ?: if (scope.lookup("this") != null) null else Undefined
        else -> throw typeError("Unknown expression")
    }

    private fun eIdent(node: N, scope: Scope): Any? {
        val name = node.a as String
        var current: Scope? = scope
        while (current != null) {
            val values = current.vars
            if (values.containsKey(name)) return values[name]
            current = current.parent
        }
        throw Thrown(errorObject("ReferenceError", "$name is not defined"), node.line)
    }

    private fun eTmpl(node: N, scope: Scope): String {
        val strings = node.a as List<String>
        val out = StringBuilder(strings[0])
        var total = cpLen(strings[0]).toLong()
        for ((index, part) in (node.b as List<N>).withIndex()) {
            val text = jsToString(toPrimitive(eval(part, scope)))
            out.append(text).append(strings[index + 1])
            total += cpLen(text) + cpLen(strings[index + 1])
            if (total > JsLimits.MAX_STRING) throw rangeError("Invalid string length")
        }
        return out.toString()
    }

    private fun eArray(node: N, scope: Scope): JsArray {
        val out = JsArray()
        for (element in node.a as List<N?>) {
            if (element == null) {
                out.add(Undefined)
            } else if (element.k == "spread") {
                out.addAll(iterate(eval(element.a as N, scope), element.a as N))
                checkArray(out)
            } else {
                out.add(eval(element, scope))
            }
        }
        return out
    }

    private fun eObject(node: N, scope: Scope): JsObject {
        val out = JsObject()
        for (prop in node.a as List<N>) {
            if (prop.k == "spread") {
                when (val value = eval(prop.a as N, scope)) {
                    is Map<*, *> -> for (key in ownKeys(value as Map<String, Any?>)) out[key] = value[key]
                    is List<*> -> for ((index, item) in value.withIndex()) out[index.toString()] = item
                    is String -> for ((index, ch) in cpList(value).withIndex()) out[index.toString()] = ch
                }
                continue
            }
            val key = if (prop.c == true) propertyKey(eval(prop.a as N, scope)) else prop.a as String
            val valueNode = prop.b as N
            val value = eval(valueNode, scope)
            if (valueNode.k == "func" && value is JsFunction && value.name.isEmpty()) value.name = key
            out[key] = value
        }
        return out
    }

    private fun arguments(nodes: List<N>, scope: Scope): List<Any?> {
        val args = ArrayList<Any?>()
        for (item in nodes) {
            if (item.k == "spread") {
                args.addAll(iterate(eval(item.a as N, scope), item.a as N))
                checkArray(args)
            } else {
                args.add(eval(item, scope))
            }
        }
        return args
    }

    private fun eCall(node: N, scope: Scope): Any? {
        val callee = node.a as N
        var thisValue: Any? = Undefined
        val fn: Any?
        if (callee.k == "member" || callee.k == "index") {
            val obj = eval(callee.a as N, scope)
            if (callee.c == true && (obj == null || obj === Undefined)) throw ShortCircuit
            val key = if (callee.k == "member") callee.b else eval(callee.b as N, scope)
            fn = get(obj, key, callee.a as N)
            thisValue = obj
        } else {
            fn = eval(callee, scope)
        }
        if (node.c == true && (fn == null || fn === Undefined)) throw ShortCircuit
        val args = arguments(node.b as List<N>, scope)
        if (!isCallable(fn)) throw Thrown(errorObject("TypeError", describeNode(callee) + " is not a function"), node.line)
        return call(fn, thisValue, args)
    }

    fun describeNode(node: N): String = when (node.k) {
        "ident" -> node.a as String
        "member" -> describeNode(node.a as N) + "." + node.b
        "index" -> describeNode(node.a as N) + "[...]"
        "call" -> describeNode(node.a as N) + "(...)"
        "this" -> "this"
        "chain" -> describeNode(node.a as N)
        "lit" -> node.a.let { if (it is String) jsJsonQuote(it) else jsToString(it) }
        else -> "expression"
    }

    fun call(fn: Any?, thisValue: Any?, args: List<Any?>): Any? {
        if (fn is Native) return fn.fn(this, thisValue, args)
        fn as JsFunction
        depth++
        if (depth > JsLimits.MAX_DEPTH) {
            depth--
            throw ScriptLimit("Maximum call stack size exceeded")
        }
        try {
            tick()
            val scope = Scope(fn.scope)
            if (!fn.arrow) scope.declare("this", thisValue, "let")
            for ((index, param) in fn.params.withIndex()) {
                var value = if (index < args.size) args[index] else Undefined
                if (value === Undefined && param.second != null) value = eval(param.second!!, scope)
                bind(param.first, value, scope, "let")
            }
            if (fn.rest != null) bind(fn.rest, JsArray(if (fn.params.size < args.size) args.subList(fn.params.size, args.size) else emptyList()), scope, "let")
            if (fn.expression) return eval(fn.body as N, scope)
            val saved = line
            val signal = execBlock(fn.body as List<N>, scope)
            line = saved
            if (signal is ReturnSignal) return signal.value
            return Undefined
        } finally {
            depth--
        }
    }

    fun makeFunction(node: N, scope: Scope): JsFunction =
        JsFunction(node.a as String, node.b as List<Pair<N, N?>>, node.c as N?, node.d, scope, node.e == true, node.f == true)

    private fun eNew(node: N, scope: Scope): Any? {
        val callee = eval(node.a as N, scope)
        val args = arguments(node.b as List<N>, scope)
        if (callee is Native && callee.construct != null) return callee.construct.invoke(this, args)
        throw Thrown(errorObject("TypeError", describeNode(node.a as N) + " is not a constructor"), node.line)
    }

    private fun eLogic(node: N, scope: Scope): Any? {
        val op = node.a as String
        val left = eval(node.b as N, scope)
        return when (op) {
            "&&" -> if (jsTruthy(left)) eval(node.c as N, scope) else left
            "||" -> if (jsTruthy(left)) left else eval(node.c as N, scope)
            else -> if (left == null || left === Undefined) eval(node.c as N, scope) else left
        }
    }

    private fun eUnary(node: N, scope: Scope): Any? {
        val op = node.a as String
        val operand = node.b as N
        if (op == "typeof") {
            if (operand.k == "ident" && scope.lookup(operand.a as String) == null) return "undefined"
            return typeOf(eval(operand, scope))
        }
        if (op == "delete") {
            if (operand.k == "member" || operand.k == "index") {
                val obj = eval(operand.a as N, scope)
                val key = if (operand.k == "member") operand.b as String else propertyKey(eval(operand.b as N, scope))
                if (obj == null || obj === Undefined) throw typeError("Cannot convert undefined or null to object")
                if (obj is MutableMap<*, *>) {
                    (obj as MutableMap<String, Any?>).remove(key)
                    return true
                }
                if (obj is JsArray) {
                    val index = arrayIndex(key)
                    if (index != null && index < obj.size) obj[index] = Undefined
                    return true
                }
                return true
            }
            return true
        }
        val value = eval(operand, scope)
        return when (op) {
            "!" -> !jsTruthy(value)
            "-" -> -jsToNumber(value)
            "+" -> jsToNumber(value)
            "~" -> toInt32(value).inv().toDouble()
            "void" -> Undefined
            else -> throw typeError("Unknown operator $op")
        }
    }

    private class Target(val obj: Any?, val key: Any?, val value: Any?)

    private fun readTarget(target: N, scope: Scope): Target {
        if (target.k == "ident") return Target(null, null, eIdent(target, scope))
        val obj = eval(target.a as N, scope)
        val key = if (target.k == "member") target.b else eval(target.b as N, scope)
        return Target(obj, key, get(obj, key, target.a as N))
    }

    private fun writeTarget(target: N, read: Target, value: Any?, scope: Scope) {
        if (target.k == "ident") assignName(target.a as String, value, scope) else put(read.obj, read.key, value)
    }

    private fun eUpdate(node: N, scope: Scope): Any? {
        val op = node.a as String
        val prefix = node.b == true
        val target = node.c as N
        val read = readTarget(target, scope)
        val old = jsToNumber(read.value)
        val new = if (op == "++") old + 1 else old - 1
        writeTarget(target, read, new, scope)
        return if (prefix) new else old
    }

    private fun eAssign(node: N, scope: Scope): Any? {
        val op = node.a as String
        val target = node.b as N
        val valueNode = node.c as N
        if (op == "=") {
            val value = eval(valueNode, scope)
            if (valueNode.k == "func" && target.k == "pid" && value is JsFunction && value.name.isEmpty()) value.name = target.a as String
            bind(target, value, scope, null)
            return value
        }
        val read = readTarget(target, scope)
        val old = read.value
        val value: Any?
        if (op == "&&=" || op == "||=" || op == "??=") {
            if (op == "&&=" && !jsTruthy(old)) return old
            if (op == "||=" && jsTruthy(old)) return old
            if (op == "??=" && old != null && old !== Undefined) return old
            value = eval(valueNode, scope)
        } else {
            value = binary(op.dropLast(1), old, eval(valueNode, scope))
        }
        writeTarget(target, read, value, scope)
        return value
    }

    fun binary(op: String, a: Any?, b: Any?): Any? {
        when (op) {
            "+" -> {
                if (a is Double && b is Double) return a + b
                val pa = toPrimitive(a)
                val pb = toPrimitive(b)
                if (pa is String || pb is String) return checkString(jsToString(pa) + jsToString(pb))
                return jsToNumber(pa) + jsToNumber(pb)
            }
            "-" -> return jsToNumber(a) - jsToNumber(b)
            "*" -> return jsToNumber(a) * jsToNumber(b)
            "/" -> return jsToNumber(a) / jsToNumber(b)
            "%" -> return jsMod(jsToNumber(a), jsToNumber(b))
            "**" -> return jsPow(jsToNumber(a), jsToNumber(b))
            "===" -> return strictEquals(a, b)
            "!==" -> return !strictEquals(a, b)
            "==" -> return looseEquals(a, b)
            "!=" -> return !looseEquals(a, b)
            "<", ">", "<=", ">=" -> {
                val pa = toPrimitive(a, "number")
                val pb = toPrimitive(b, "number")
                if (pa is String && pb is String) {
                    val c = cpCompare(pa, pb)
                    return when (op) {
                        "<" -> c < 0
                        ">" -> c > 0
                        "<=" -> c <= 0
                        else -> c >= 0
                    }
                }
                val x = jsToNumber(pa)
                val y = jsToNumber(pb)
                return when (op) {
                    "<" -> x < y
                    ">" -> x > y
                    "<=" -> x <= y
                    else -> x >= y
                }
            }
            "&" -> return wrap32(toInt32(a) and toInt32(b)).toDouble()
            "|" -> return wrap32(toInt32(a) or toInt32(b)).toDouble()
            "^" -> return wrap32(toInt32(a) xor toInt32(b)).toDouble()
            "<<" -> return wrap32(toInt32(a) shl (toUint32(b) and 31).toInt()).toDouble()
            ">>" -> return (toInt32(a) shr (toUint32(b) and 31).toInt()).toDouble()
            ">>>" -> return (toUint32(a) ushr (toUint32(b) and 31).toInt()).toDouble()
            "in" -> {
                if (b is Map<*, *>) return b.containsKey(propertyKey(a))
                if (b is List<*>) {
                    val key = propertyKey(a)
                    val index = arrayIndex(key)
                    return key == "length" || (index != null && index < b.size)
                }
                throw typeError("Cannot use 'in' operator to search for '" + jsToString(a) + "' in " + jsToString(b))
            }
            "instanceof" -> return instanceOf(a, b)
        }
        throw typeError("Unknown operator $op")
    }

    private fun instanceOf(value: Any?, kind: Any?): Boolean {
        if (!isCallable(kind)) throw typeError("Right-hand side of 'instanceof' is not callable")
        val name = if (kind is Native) kind.name else ""
        return when (name) {
            "Array" -> value is List<*>
            "Object" -> isJsObject(value)
            "Date" -> value is JsDate
            "RegExp" -> value is JsRegExp
            "Set", "Map" -> value is JsCollection && value.kind == name
            "Function" -> isCallable(value)
            "Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError", "URIError" -> {
                if (!(value is Map<*, *> && value["name"] is String && value.containsKey("message"))) false
                else name == "Error" || value["name"] == name
            }
            else -> false
        }
    }

    fun get(obj: Any?, key: Any?, node: N? = null): Any? {
        if (obj is Map<*, *>) {
            val name = if (key is String) key else propertyKey(key)
            if (obj.containsKey(name)) return obj[name]
            OBJECT_METHODS[name]?.let { return Native(name, fn = it) }
            return Undefined
        }
        if (obj is List<*>) {
            val index = arrayIndex(key)
            if (index != null) return if (index < obj.size) obj[index] else Undefined
            val name = propertyKey(key)
            if (name == "length") return obj.size.toDouble()
            if (obj is JsArray && obj.isMatch && name in setOf("index", "input", "groups")) {
                return when (name) {
                    "index" -> obj.index
                    "input" -> obj.input
                    else -> obj.groups
                }
            }
            ARRAY_METHODS[name]?.let { return Native(name, fn = it) }
            return Undefined
        }
        if (obj is String) {
            val index = arrayIndex(key)
            if (index != null) return if (index < cpLen(obj)) cpAt(obj, index) else Undefined
            val name = propertyKey(key)
            if (name == "length") return cpLen(obj).toDouble()
            STRING_METHODS[name]?.let { return Native(name, fn = it) }
            return Undefined
        }
        if (obj == null || obj === Undefined) {
            throw typeError("Cannot read properties of " + jsToString(obj) + " (reading '" + propertyKey(key) + "')")
        }
        val name = propertyKey(key)
        return when (obj) {
            is Double -> NUMBER_METHODS[name]?.let { Native(name, fn = it) } ?: Undefined
            is Boolean -> if (name == "toString") Native(name) { _, t, _ -> jsToString(t) } else Undefined
            is JsDate -> DATE_METHODS[name]?.let { Native(name, fn = it) } ?: Undefined
            is JsCollection -> {
                if (name == "size") return obj.keys.size.toDouble()
                (if (obj.kind == "Set") SET_METHODS else MAP_METHODS)[name]?.let { Native(name, fn = it) } ?: Undefined
            }
            is JsRegExp -> when (name) {
                "source" -> obj.source
                "flags" -> obj.flags.toList().sortedBy { "dgimsuy".indexOf(it) }.joinToString("")
                "global" -> obj.flags.contains('g')
                "ignoreCase" -> obj.flags.contains('i')
                "multiline" -> obj.flags.contains('m')
                "lastIndex" -> obj.lastIndex.toDouble()
                else -> REGEXP_METHODS[name]?.let { Native(name, fn = it) } ?: Undefined
            }
            is Native -> {
                if (obj.props.containsKey(name)) return obj.props[name]
                if (name == "name") return obj.name
                FUNCTION_METHODS[name]?.let { Native(name, fn = it) } ?: Undefined
            }
            is JsFunction -> when (name) {
                "name" -> obj.name
                "length" -> obj.params.size.toDouble()
                else -> FUNCTION_METHODS[name]?.let { Native(name, fn = it) } ?: Undefined
            }
            else -> Undefined
        }
    }

    fun put(obj: Any?, key: Any?, value: Any?) {
        if (obj is MutableMap<*, *>) {
            (obj as MutableMap<String, Any?>)[propertyKey(key)] = value
            return
        }
        if (obj is MutableList<*>) {
            val list = obj as MutableList<Any?>
            val index = arrayIndex(key)
            if (index != null) {
                if (index < list.size) {
                    list[index] = value
                    return
                }
                if (index >= JsLimits.MAX_ARRAY) throw rangeError("Invalid array length")
                while (list.size < index) list.add(Undefined)
                list.add(value)
                return
            }
            val name = propertyKey(key)
            if (name == "length") {
                val length = jsToNumber(value)
                if (length.isNaN() || length < 0 || length != Math.rint(length) || length > 4294967295.0) throw rangeError("Invalid array length")
                if (length > JsLimits.MAX_ARRAY) throw rangeError("Invalid array length")
                val size = length.toInt()
                while (list.size > size) list.removeAt(list.size - 1)
                while (list.size < size) list.add(Undefined)
                return
            }
            throw typeError("Cannot add property $name to a list")
        }
        if (obj == null || obj === Undefined) {
            throw typeError("Cannot set properties of " + jsToString(obj) + " (setting '" + propertyKey(key) + "')")
        }
        if (obj is JsRegExp && propertyKey(key) == "lastIndex") {
            val number = toInteger(value)
            obj.lastIndex = if (number != Double.POSITIVE_INFINITY) maxOf(0.0, number).coerceAtMost(Int.MAX_VALUE.toDouble()).toInt() else JsLimits.MAX_STRING + 1
            return
        }
        throw typeError("Cannot create property '" + propertyKey(key) + "' on " + typeOf(obj) + " '" + jsToString(obj) + "'")
    }
}
