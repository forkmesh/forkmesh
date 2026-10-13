package com.forkmesh.nativeapp.api

import com.forkmesh.nativeapp.net.HttpClient

fun interface HttpTransport {
    fun send(request: HttpClient.Request): HttpClient.Response

    companion object {
        val DEFAULT: HttpTransport = HttpTransport { request -> HttpClient.send(request) }
    }
}

interface ClientSettings {
    val serverUrl: String

    val room: String
}

open class DartException(message: String, cause: Throwable? = null) : RuntimeException(message, cause) {
    override fun toString(): String = "Exception: $message"
}
