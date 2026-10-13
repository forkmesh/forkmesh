package com.forkmesh.nativeapp.api

class AppEnv(
    val rawEnvironment: String = "prod",
    val apiHostOverride: String = "",
    val serverUrlOverride: String = "",
    val worldSiteUrl: String = "https://world.forkmesh.com/",
) {
    val isDev: Boolean get() = rawEnvironment.lowercase() in DEV_MARKERS

    val environmentName: String get() = if (isDev) "dev" else "prod"

    val defaultServerUrl: String
        get() {
            if (serverUrlOverride.isNotEmpty()) return serverUrlOverride
            if (apiHostOverride.isNotEmpty()) return relayUrlForHost(apiHostOverride)
            if (!isDev) return PROD_SERVER_URL
            return ANDROID_EMULATOR_SERVER_URL
        }

    fun describe(serverUrl: String): String {
        val host = ServerUrl.parse(serverUrl).host
        return if (host.isEmpty()) environmentName else "$environmentName · $host"
    }

    companion object {
        const val PROD_HOST = "app.forkmesh.com"
        const val DEV_HOST = "localhost"
        const val ANDROID_EMULATOR_HOST = "10.0.2.2"
        const val DEV_PORT = 8787

        const val RELAY_PATH = "/api/repo/mainnode/forkmesh/rooms/world-general/ws"

        const val PROD_SERVER_URL = "wss://$PROD_HOST$RELAY_PATH"

        const val DEV_SERVER_URL = "ws://$DEV_HOST:$DEV_PORT$RELAY_PATH"

        const val ANDROID_EMULATOR_SERVER_URL = "ws://$ANDROID_EMULATOR_HOST:$DEV_PORT$RELAY_PATH"

        private val DEV_MARKERS = setOf("dev", "development", "local")

        val PROD = AppEnv()

        fun relayUrlForHost(host: String): String {
            val trimmed = host.trim()
            val scheme = if (isLoopbackHost(trimmed.split(':').first())) "ws" else "wss"
            return "$scheme://$trimmed$RELAY_PATH"
        }

        fun isLoopbackHost(host: String): Boolean =
            host.trim().lowercase() in setOf("localhost", "127.0.0.1", "::1", ANDROID_EMULATOR_HOST)

        fun isLoopbackUrl(url: String): Boolean {
            val host = ServerUrl.parse(url.trim()).host
            if (host.isEmpty()) return false
            return isLoopbackHost(host)
        }
    }
}
