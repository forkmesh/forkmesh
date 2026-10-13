package com.forkmesh.nativeapp.testing

fun expect(condition: Boolean, message: String = "expectation failed") {
    if (!condition) throw AssertionError(message)
}

fun assertEq(expected: Any?, actual: Any?, message: String = "") {
    if (expected != actual) {
        throw AssertionError("${if (message.isEmpty()) "" else "$message: "}expected <$expected> but was <$actual>")
    }
}

fun assertBytes(expected: ByteArray, actual: ByteArray) {
    if (!expected.contentEquals(actual)) {
        throw AssertionError("expected ${hexOf(expected)} but was ${hexOf(actual)}")
    }
}

fun assertHex(expectedHex: String, actual: ByteArray) = assertEq(expectedHex.lowercase(), hexOf(actual))

inline fun <reified T : Throwable> assertFails(block: () -> Unit): T {
    try {
        block()
    } catch (thrown: Throwable) {
        if (thrown is T) return thrown
        throw AssertionError("expected ${T::class.java.simpleName} but got $thrown", thrown)
    }
    throw AssertionError("expected ${T::class.java.simpleName} but nothing was thrown")
}

fun hexOf(bytes: ByteArray): String = bytes.joinToString("") { "%02x".format(it.toInt() and 0xff) }

fun bytesOfHex(hex: String): ByteArray = ByteArray(hex.length / 2) {
    hex.substring(2 * it, 2 * it + 2).toInt(16).toByte()
}

fun resourceText(path: String): String =
    (Thread.currentThread().contextClassLoader.getResourceAsStream(path)
        ?: throw AssertionError("missing test resource $path")).use { String(it.readBytes(), Charsets.UTF_8) }

fun resourceBytes(path: String): ByteArray =
    (Thread.currentThread().contextClassLoader.getResourceAsStream(path)
        ?: throw AssertionError("missing test resource $path")).use { it.readBytes() }
