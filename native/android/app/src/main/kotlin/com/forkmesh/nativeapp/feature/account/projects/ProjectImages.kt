package com.forkmesh.nativeapp.feature.account.projects

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import androidx.compose.runtime.mutableStateMapOf
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import com.forkmesh.nativeapp.account.projects.Project
import com.forkmesh.nativeapp.account.projects.ProjectDefaults
import com.forkmesh.nativeapp.account.projects.ProjectScope
import com.forkmesh.nativeapp.net.HttpClient
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.ByteArrayOutputStream
import java.io.File

class ProjectLogoStore(private val dir: File) {
    fun file(name: String): File? {
        if (name.isEmpty() || name.contains('/') || name.contains('\\')) return null
        return File(dir, name)
    }

    fun save(png: ByteArray, projectKey: String): String? = runCatching {
        dir.mkdirs()
        val slug = ProjectScope.slugFor(projectKey.ifEmpty { "project" }).take(48)
        val name = "$slug-${System.currentTimeMillis()}.png"
        File(dir, name).writeBytes(png)
        name
    }.getOrNull()

    fun delete(name: String) {
        file(name)?.takeIf { it.isFile }?.delete()
    }

    companion object {
        const val MAX_PIXELS = 256
        private const val MAX_SOURCE_BYTES = 32 * 1024 * 1024

        fun read(context: Context, uri: Uri): ByteArray? = runCatching {
            context.contentResolver.openInputStream(uri)?.use { input ->
                val out = ByteArrayOutputStream()
                val buffer = ByteArray(64 * 1024)
                while (true) {
                    val n = input.read(buffer)
                    if (n < 0) break
                    out.write(buffer, 0, n)
                    if (out.size() > MAX_SOURCE_BYTES) return@use null
                }
                out.toByteArray()
            }
        }.getOrNull()

        fun shrink(bytes: ByteArray): Bitmap? {
            val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
            if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null
            var sample = 1
            while (bounds.outWidth / (sample * 2) >= MAX_PIXELS && bounds.outHeight / (sample * 2) >= MAX_PIXELS) sample *= 2
            val decoded = BitmapFactory.decodeByteArray(bytes, 0, bytes.size, BitmapFactory.Options().apply { inSampleSize = sample })
                ?: return null
            val longest = maxOf(decoded.width, decoded.height)
            if (longest <= MAX_PIXELS) return decoded
            val scale = MAX_PIXELS.toFloat() / longest
            return Bitmap.createScaledBitmap(
                decoded,
                (decoded.width * scale).toInt().coerceAtLeast(1),
                (decoded.height * scale).toInt().coerceAtLeast(1),
                true,
            )
        }

        fun png(bitmap: Bitmap): ByteArray {
            val out = ByteArrayOutputStream()
            bitmap.compress(Bitmap.CompressFormat.PNG, 100, out)
            return out.toByteArray()
        }
    }
}

object ProjectImages {
    private val logos = mutableStateMapOf<String, ImageBitmap>()
    private val favicons = mutableStateMapOf<String, ImageBitmap>()
    private val missing = java.util.Collections.synchronizedSet(HashSet<String>())
    private val fetching = java.util.Collections.synchronizedSet(HashSet<String>())

    fun image(project: Project): ImageBitmap? {
        if (project.logo.isNotEmpty()) return logos[project.logo]
        if (!project.hasInstance || ProjectDefaults.isFixed(project.id)) return null
        return favicons[faviconKey(project)]
    }

    fun remember(logo: String, bitmap: Bitmap) {
        logos[logo] = bitmap.asImageBitmap()
    }

    suspend fun load(project: Project, store: ProjectLogoStore, cacheDir: File) {
        if (project.logo.isNotEmpty()) {
            loadLogo(project.logo, store)
        } else if (project.hasInstance && !ProjectDefaults.isFixed(project.id)) {
            loadFavicon(project, cacheDir)
        }
    }

    private suspend fun loadLogo(name: String, store: ProjectLogoStore) {
        if (logos.containsKey(name) || missing.contains("logo:$name")) return
        val bitmap = withContext(Dispatchers.IO) {
            store.file(name)?.takeIf { it.isFile }?.let { BitmapFactory.decodeFile(it.path) }
        }
        if (bitmap == null) {
            missing.add("logo:$name")
            return
        }
        logos[name] = bitmap.asImageBitmap()
    }

    private fun faviconKey(project: Project): String = project.host.ifEmpty { project.id }

    private suspend fun loadFavicon(project: Project, cacheDir: File) {
        val key = faviconKey(project)
        if (key.isEmpty() || favicons.containsKey(key) || missing.contains(key) || !fetching.add(key)) return
        try {
            val file = File(File(cacheDir, "project-favicons"), ProjectScope.slugFor(key) + ".png")
            val bitmap = withContext(Dispatchers.IO) {
                file.takeIf { it.isFile }?.let { BitmapFactory.decodeFile(it.path) } ?: fetch(project.httpOrigin)?.also { fetched ->
                    runCatching {
                        file.parentFile?.mkdirs()
                        file.writeBytes(ProjectLogoStore.png(fetched))
                    }
                }
            }
            if (bitmap == null) {
                missing.add(key)
            } else {
                favicons[key] = bitmap.asImageBitmap()
            }
        } finally {
            fetching.remove(key)
        }
    }

    fun fetch(origin: String, logoUrl: String = "", favicon: Boolean = true): Bitmap? {
        val favicons = if (favicon) listOf("/favicon.ico", "/favicon/favicon.ico").map { origin + it } else emptyList()
        return instanceLogo(listOf(logoUrl).filter { it.isNotEmpty() } + favicons)
    }

    private val directoryLogos = java.util.concurrent.ConcurrentHashMap<String, Bitmap>()

    fun directoryLogo(url: String): Bitmap? {
        if (url.isEmpty()) return null
        directoryLogos[url]?.let { return it }
        return instanceLogo(listOf(url))?.also { directoryLogos[url] = it }
    }

    fun instanceLogo(urls: List<String>): Bitmap? {
        for (url in urls) {
            val response = runCatching { HttpClient.Request.get(url).timeouts(8_000, 10_000).send() }.getOrNull() ?: continue
            if (response.status != 200 || response.body.isEmpty()) continue
            ProjectLogoStore.shrink(response.body)?.let { return it }
        }
        return null
    }
}
