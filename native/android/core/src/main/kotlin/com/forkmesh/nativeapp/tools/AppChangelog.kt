package com.forkmesh.nativeapp.tools

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.json.strings
import java.time.LocalDate
import java.time.YearMonth
import java.time.format.DateTimeFormatter
import java.time.format.DateTimeParseException
import java.time.format.FormatStyle
import java.util.Locale

data class AppChangelog(val entries: List<Entry> = emptyList()) {
    data class Section(val title: String, val items: List<String>)

    data class Entry(
        val version: String,
        val build: String,
        val date: String = "",
        val sections: List<Section> = emptyList(),
        val title: String = "",
        val pitch: String = "",
    ) {
        val heading: String get() = when {
            title.isNotEmpty() -> title
            version.isNotEmpty() || build.isNotEmpty() -> label(version, build)
            else -> formattedDate()
        }

        fun detail(locale: Locale = Locale.getDefault()): String = listOfNotNull(
            label(version, build).takeIf { title.isNotEmpty() && version.isNotEmpty() },
            formattedDate(locale).takeIf { date.isNotEmpty() && (title.isNotEmpty() || version.isNotEmpty() || build.isNotEmpty()) },
        ).joinToString(" · ")

        fun formattedDate(locale: Locale = Locale.getDefault()): String = try {
            LocalDate.parse(date).format(DateTimeFormatter.ofLocalizedDate(FormatStyle.LONG).withLocale(locale))
        } catch (_: DateTimeParseException) {
            try {
                YearMonth.parse(date).format(DateTimeFormatter.ofPattern("LLLL yyyy", locale))
            } catch (_: DateTimeParseException) {
                date
            }
        }
    }

    fun forRunningBuild(version: String, build: String): AppChangelog {
        val first = entries.firstOrNull()
        if (first != null && build.isNotEmpty() && first.build == build) {
            return copy(entries = listOf(first.copy(version = version.ifEmpty { first.version })) + entries.drop(1))
        }
        return if (entries.isEmpty()) AppChangelog(listOf(Entry(version, build))) else this
    }

    companion object {
        const val ASSET = "changelog.json"

        fun label(version: String, build: String): String {
            val shown = version.ifEmpty { "0.0.0" }
            return if (build.isEmpty()) "Version $shown" else "Version $shown ($build)"
        }

        fun parse(text: String?): AppChangelog {
            val root = Json.parseObject(text ?: "")
            return AppChangelog(root.maps("entries").map { entry ->
                Entry(
                    version = entry.str("version"),
                    build = entry.str("build"),
                    date = entry.str("date"),
                    title = entry.str("title"),
                    pitch = entry.str("pitch"),
                    sections = entry.maps("sections")
                        .map { Section(it.str("title"), it.strings("items").filter(String::isNotBlank)) }
                        .filter { it.title.isNotEmpty() && it.items.isNotEmpty() },
                )
            })
        }

        fun load(text: String?, version: String, build: String): AppChangelog = parse(text).forRunningBuild(version, build)
    }
}
