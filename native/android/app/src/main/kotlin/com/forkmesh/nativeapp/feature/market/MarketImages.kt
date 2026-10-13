package com.forkmesh.nativeapp.feature.market

import android.graphics.BitmapFactory
import android.util.LruCache
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.market.PackageScreenshot
import com.forkmesh.nativeapp.net.HttpClient
import com.forkmesh.nativeapp.ui.kit.FmCircularProgressIndicator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

internal object MarketImages {
    const val THUMBNAIL_WIDTH = 320
    const val FULL_WIDTH = 1280
    private const val MAX_BYTES = 8 * 1024 * 1024

    private val cache = object : LruCache<String, ImageBitmap>(32 * 1024 * 1024) {
        override fun sizeOf(key: String, value: ImageBitmap): Int = value.width * value.height * 4
    }

    private fun key(url: String, maxWidth: Int) = "$maxWidth $url"

    fun cached(url: String, maxWidth: Int): ImageBitmap? = cache.get(key(url, maxWidth))

    fun load(url: String, maxWidth: Int): ImageBitmap? {
        cached(url, maxWidth)?.let { return it }
        val bytes = runCatching { HttpClient.Request.get(url).timeouts(10_000, 20_000).send() }
            .getOrNull()?.takeIf { it.ok && it.body.size in 1..MAX_BYTES }?.body ?: return null
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) return null
        var sample = 1
        while (bounds.outWidth / (sample * 2) >= maxWidth) sample *= 2
        val image = BitmapFactory.decodeByteArray(bytes, 0, bytes.size, BitmapFactory.Options().apply { inSampleSize = sample })
            ?.asImageBitmap() ?: return null
        cache.put(key(url, maxWidth), image)
        return image
    }
}

@Composable
internal fun MarketScreenshotImage(
    screenshot: PackageScreenshot,
    modifier: Modifier = Modifier,
    maxWidth: Int = MarketImages.FULL_WIDTH,
    radius: Dp = FmRadius.lg,
    describe: Boolean = true,
    contentScale: ContentScale = ContentScale.Crop,
) {
    var image by remember(screenshot.url, maxWidth) { mutableStateOf(MarketImages.cached(screenshot.url, maxWidth)) }
    var failed by remember(screenshot.url, maxWidth) { mutableStateOf(false) }
    LaunchedEffect(screenshot.url, maxWidth) {
        if (image == null) {
            image = withContext(Dispatchers.IO) { MarketImages.load(screenshot.url, maxWidth) }
            failed = image == null
        }
    }
    Box(
        modifier
            .aspectRatio(16f / 9f)
            .clip(RoundedCornerShape(radius))
            .background(Fm.colors.bgOverlay),
        contentAlignment = Alignment.Center,
    ) {
        val bitmap = image
        when {
            bitmap != null -> Image(
                bitmap = bitmap,
                contentDescription = if (describe) screenshot.alt.ifEmpty { "Screenshot" } else null,
                modifier = Modifier.fillMaxSize(),
                contentScale = contentScale,
            )
            failed -> FmIcon("broken_image", tint = Fm.colors.textTertiary)
            else -> FmCircularProgressIndicator(Modifier.size(20.dp), strokeWidth = 2.dp)
        }
    }
}

@Composable
internal fun MarketScreenshotStrip(screenshots: List<PackageScreenshot>, modifier: Modifier = Modifier) {
    if (screenshots.size == 1) {
        MarketScreenshotImage(screenshots.first(), modifier.fillMaxWidth())
        return
    }
    LazyRow(modifier.fillMaxWidth(), horizontalArrangement = Arrangement.spacedBy(12.dp)) {
        items(screenshots, key = { it.url }) { screenshot ->
            MarketScreenshotImage(screenshot, Modifier.fillParentMaxWidth(.85f))
        }
    }
}
