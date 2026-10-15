package com.forkmesh.nativeapp.rssreader

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.maps
import java.io.File

class RssStore(val directory: File) {
    private val libraryFile get() = File(directory, "library.json")
    private val articleDir get() = File(directory, "articles")
    private val iconDir get() = File(directory, "icons")

    fun load(): RssLibrary {
        val data = runCatching { RssLibraryData.fromJson(Json.parseObject(libraryFile.readText(Charsets.UTF_8))) }.getOrNull() ?: RssLibraryData()
        val articles = HashMap<String, List<RssArticle>>()
        for (feed in data.feeds) {
            val file = File(articleDir, "${safe(feed.id)}.json")
            if (!file.isFile) continue
            articles[feed.id] = runCatching {
                Json.parseObject(file.readText(Charsets.UTF_8)).maps("articles").map(RssArticle::fromJson).filter { it.feedId == feed.id && it.id.isNotEmpty() }
            }.getOrDefault(emptyList())
        }
        return RssLibrary(data, articles)
    }

    @Synchronized
    fun save(library: RssLibrary) {
        val (meta, changed, removed) = library.takeChanges()
        if (meta != null) write(libraryFile, Json.stringify(meta.toJson()))
        for ((feedId, list) in changed) {
            write(File(articleDir, "${safe(feedId)}.json"), Json.stringify(linkedMapOf("articles" to list.map { it.toJson() })))
        }
        for (feedId in removed) {
            File(articleDir, "${safe(feedId)}.json").delete()
            iconFile(feedId).delete()
        }
    }

    fun iconFile(feedId: String): File = File(iconDir, safe(feedId))

    fun readIcon(feedId: String): ByteArray? = iconFile(feedId).takeIf { it.isFile }?.let { runCatching { it.readBytes() }.getOrNull() }

    @Synchronized
    fun writeIcon(feedId: String, bytes: ByteArray) {
        iconDir.mkdirs()
        val target = iconFile(feedId)
        val temp = File(iconDir, "${target.name}.tmp")
        temp.writeBytes(bytes)
        if (!temp.renameTo(target)) {
            target.delete()
            temp.renameTo(target)
        }
    }

    private fun safe(id: String): String = id.filter { it.isLetterOrDigit() }.ifEmpty { "feed" }

    private fun write(target: File, text: String) {
        target.parentFile?.mkdirs()
        val temp = File(target.parentFile, "${target.name}.tmp")
        temp.writeText(text, Charsets.UTF_8)
        if (!temp.renameTo(target)) {
            target.delete()
            temp.renameTo(target)
        }
    }
}
