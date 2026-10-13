package com.forkmesh.nativeapp.model

import com.forkmesh.nativeapp.json.jsonMaps
import com.forkmesh.nativeapp.json.jsonText
import com.forkmesh.nativeapp.json.str

data class RepoReleaseAsset(
    val name: String,
    val sha256: String,
    val size: Long = 0,
    val os: String = "",
    val arch: String = "",
    val downloads: Int = 0,
) {
    val shortSha: String get() = if (sha256.length > 12) sha256.substring(0, 12) else sha256

    val platformLabel: String
        get() {
            val parts = listOf(os, arch).filter { it.trim().isNotEmpty() }
            return if (parts.isEmpty()) "artifact" else parts.joinToString(" / ")
        }

    val sizeLabel: String
        get() {
            if (size <= 0) return "unknown size"
            val units = listOf("B", "KB", "MB", "GB")
            var value = size.toDouble()
            var unit = 0
            while (value >= 1024 && unit < units.size - 1) {
                value /= 1024
                unit += 1
            }
            if (unit == 0) return "${dartToStringAsFixed(value, 0)} ${units[unit]}"
            return "${dartToStringAsFixed(value, 1)} ${units[unit]}"
        }

    val downloadLabel: String get() = "$downloads ${if (downloads == 1) "download" else "downloads"}"

    companion object {
        fun fromJson(json: Map<String, Any?>, downloads: Int = 0): RepoReleaseAsset = RepoReleaseAsset(
            name = jsonText(json["name"] ?: json["filename"]),
            sha256 = jsonText(json["sha256"] ?: json["blob_sha256"] ?: json["hash"]),
            size = modelInt(json["size"] ?: json["bytes"]),
            os = json.str("os"),
            arch = json.str("arch"),
            downloads = downloads,
        )
    }
}

data class RepoRelease(
    val tag: String,
    val channel: String = "",
    val tagCommit: String = "",
    val createdAtMs: Long = 0,
    val assets: List<RepoReleaseAsset> = emptyList(),
) {
    val shortCommit: String get() = if (tagCommit.length > 8) tagCommit.substring(0, 8) else tagCommit

    val createdDate: String get() = if (createdAtMs <= 0) "" else modelLocalDate(createdAtMs)

    val assetSummary: String
        get() = if (assets.isEmpty()) {
            "No published artifacts yet"
        } else {
            "${assets.size} ${if (assets.size == 1) "artifact" else "artifacts"}"
        }

    companion object {
        fun fromJson(json: Map<String, Any?>, downloads: Map<String, Int> = emptyMap()): RepoRelease {
            val assets = jsonMaps(json["assets"])
                .map { asset ->
                    val map = LinkedHashMap(asset)
                    val sha = jsonText(map["sha256"] ?: map["blob_sha256"] ?: map["hash"])
                    RepoReleaseAsset.fromJson(map, downloads = downloads[sha] ?: 0)
                }
                .filter { it.name.isNotEmpty() || it.sha256.isNotEmpty() }
            return RepoRelease(
                tag = jsonText(json["tag"] ?: json["name"]),
                channel = json.str("channel"),
                tagCommit = jsonText(json["tag_commit"] ?: json["tagCommit"] ?: json["commit"]),
                createdAtMs = modelInt(json["created_at"] ?: json["createdAt"] ?: json["ts"]),
                assets = assets,
            )
        }
    }
}
