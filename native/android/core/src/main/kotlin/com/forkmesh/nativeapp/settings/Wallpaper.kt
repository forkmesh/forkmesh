package com.forkmesh.nativeapp.settings

data class Wallpaper(val image: String = "", val dim: Double = DEFAULT_DIM) {
    val active: Boolean get() = image.isNotEmpty()

    fun withDim(value: Double?): Wallpaper = copy(dim = clampDim(value))

    fun withImage(name: String?): Wallpaper = copy(image = cleanName(name))

    companion object {
        const val DEFAULT_DIM: Double = 0.4
        const val MAX_DIM: Double = 0.9
        const val DIM_STEP: Double = 0.05
        const val DIRECTORY: String = "wallpapers"

        fun clampDim(value: Double?): Double {
            if (value == null || value.isNaN()) return DEFAULT_DIM
            return value.coerceIn(0.0, MAX_DIM)
        }

        fun snapDim(value: Double): Double = clampDim(Math.round(value / DIM_STEP) * DIM_STEP)

        fun cleanName(name: String?): String = if (name == null || name.contains('/')) "" else name

        fun fileName(epochMillis: Long): String = "wallpaper-$epochMillis.jpg"

        fun percent(dim: Double): Int = Math.round(clampDim(dim) * 100).toInt()

        fun sampleSize(width: Int, height: Int, targetWidth: Int, targetHeight: Int): Int {
            if (width <= 0 || height <= 0 || targetWidth <= 0 || targetHeight <= 0) return 1
            var sample = 1
            while (width / (sample * 2) >= targetWidth && height / (sample * 2) >= targetHeight) sample *= 2
            return sample
        }

        fun fitSize(width: Int, height: Int, maxSide: Int): Pair<Int, Int> {
            if (width <= 0 || height <= 0 || maxSide <= 0) return width to height
            val longest = maxOf(width, height)
            if (longest <= maxSide) return width to height
            val scale = maxSide.toDouble() / longest
            return maxOf(1, Math.round(width * scale).toInt()) to maxOf(1, Math.round(height * scale).toInt())
        }

        fun coverSize(width: Int, height: Int, targetWidth: Int, targetHeight: Int): Pair<Int, Int> {
            if (width <= 0 || height <= 0 || targetWidth <= 0 || targetHeight <= 0) return width to height
            val scale = maxOf(targetWidth.toDouble() / width, targetHeight.toDouble() / height)
            if (scale >= 1.0) return width to height
            return maxOf(targetWidth, Math.ceil(width * scale).toInt()) to maxOf(targetHeight, Math.ceil(height * scale).toInt())
        }

        fun read(get: (String) -> String?, getDouble: (String) -> Double?): Wallpaper =
            Wallpaper(cleanName(get(AppearanceKeys.BACKGROUND_IMAGE)), clampDim(getDouble(AppearanceKeys.BACKGROUND_DIM)))
    }
}
