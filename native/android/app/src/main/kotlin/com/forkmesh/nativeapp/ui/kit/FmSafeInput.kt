package com.forkmesh.nativeapp.ui.kit

import android.text.InputFilter
import android.text.SpannableStringBuilder
import android.text.Spanned
import androidx.compose.ui.text.TextRange
import androidx.compose.ui.text.input.TextFieldValue
import com.forkmesh.nativeapp.security.HiddenUnicode

object FmSafeInput {
    fun clean(value: TextFieldValue): TextFieldValue {
        if (value.composition != null) return value
        val text = value.text
        val cleaned = HiddenUnicode.clean(text)
        if (cleaned == text) return value
        return TextFieldValue(
            cleaned,
            TextRange(
                HiddenUnicode.cleanedOffset(text, value.selection.start),
                HiddenUnicode.cleanedOffset(text, value.selection.end),
            ),
        )
    }

    @Suppress("UNCHECKED_CAST")
    fun <T> cleaned(value: T): T = when (value) {
        is String -> HiddenUnicode.clean(value) as T
        is TextFieldValue -> clean(value) as T
        else -> value
    }

    fun <T> text(change: (T) -> Unit): (T) -> Unit = { value -> change(cleaned(value)) }

    fun <T> typing(change: (T) -> Unit): (T) -> Unit = FmUserActivity.typing(text(change))

    object Filter : InputFilter {
        override fun filter(source: CharSequence, start: Int, end: Int, dest: Spanned, dstart: Int, dend: Int): CharSequence? {
            val inserted = source.subSequence(start, end).toString()
            val cleaned = HiddenUnicode.clean(inserted)
            if (cleaned == inserted) return null
            if (source !is Spanned) return cleaned
            val kept = SpannableStringBuilder(source, start, end)
            var i = inserted.length
            var j = cleaned.length
            while (i > 0) {
                val unit = inserted[i - 1]
                when {
                    j > 0 && unit == cleaned[j - 1] -> j--
                    j > 0 && cleaned[j - 1] == '\n' && (unit.code == 0x2028 || unit.code == 0x2029) -> {
                        kept.replace(i - 1, i, "\n")
                        j--
                    }
                    else -> kept.delete(i - 1, i)
                }
                i--
            }
            return kept
        }
    }
}
