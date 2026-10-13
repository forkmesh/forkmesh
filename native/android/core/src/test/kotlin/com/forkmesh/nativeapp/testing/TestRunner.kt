package com.forkmesh.nativeapp.testing

import java.io.File
import java.lang.reflect.InvocationTargetException
import java.lang.reflect.Modifier
import kotlin.system.exitProcess

fun main(args: Array<String>) {
    val roots = args[0].split(File.pathSeparator).map(::File).filter { it.isDirectory }
    val filter = args.getOrElse(1) { "" }
    val classNames = roots.flatMap { root ->
        root.walkTopDown()
            .filter { it.isFile && it.name.endsWith("Test.class") && '$' !in it.name }
            .map { it.relativeTo(root).path.removeSuffix(".class").replace(File.separatorChar, '.') }
            .toList()
    }.distinct().sorted()

    var passed = 0
    var failed = 0
    val started = System.currentTimeMillis()
    for (className in classNames) {
        if (filter.isNotEmpty() && !className.contains(filter)) continue
        val type = Class.forName(className)
        val instance: Any? = runCatching { type.getField("INSTANCE").get(null) }.getOrNull()
            ?: runCatching { type.getDeclaredConstructor().also { it.isAccessible = true }.newInstance() }.getOrNull()
        val cases = type.declaredMethods
            .filter { it.name.startsWith("test") && it.parameterCount == 0 && Modifier.isPublic(it.modifiers) }
            .sortedBy { it.name }
        for (method in cases) {
            val name = "${type.simpleName}.${method.name}"
            val caseStarted = System.currentTimeMillis()
            try {
                method.invoke(if (Modifier.isStatic(method.modifiers)) null else instance)
                passed++
                println("PASS $name (${System.currentTimeMillis() - caseStarted} ms)")
            } catch (wrapped: InvocationTargetException) {
                failed++
                val cause = wrapped.cause ?: wrapped
                println("FAIL $name: $cause")
                cause.stackTrace.take(8).forEach { println("       at $it") }
            }
        }
    }
    println()
    println("$passed passed, $failed failed in ${System.currentTimeMillis() - started} ms")
    exitProcess(if (failed == 0 && passed > 0) 0 else 1)
}
