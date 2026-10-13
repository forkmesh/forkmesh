package com.forkmesh.nativeapp.tools

data class AppBuild(
    val version: String,
    val platform: String,
    val osVersion: String,
    val mode: String,
) {
    companion object {
        val UNKNOWN = AppBuild(version = "", platform = "", osVersion = "", mode = "debug")

        fun version(versionName: String, versionCode: Long): String = "$versionName+$versionCode"

        fun mode(debuggable: Boolean): String = if (debuggable) "debug" else "release"

        fun androidOsVersion(release: String, sdkInt: Int): String = "Android $release (API $sdkInt)"
    }
}
