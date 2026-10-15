package com.forkmesh.nativeapp.rssreader

import java.time.LocalDate
import java.time.LocalDateTime
import java.time.ZoneOffset

object RssDates {
    private val MONTHS = listOf("jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec")
    private val DAYS = listOf("mon", "tue", "wed", "thu", "fri", "sat", "sun")
    private val ZONES = mapOf(
        "ut" to 0, "utc" to 0, "gmt" to 0, "z" to 0, "ast" to -400, "adt" to -300, "est" to -500, "edt" to -400,
        "cst" to -600, "cdt" to -500, "mst" to -700, "mdt" to -600, "pst" to -800, "pdt" to -700,
    )
    private val ISO = Regex(
        "^(\\d{4})-(\\d{2})-(\\d{2})(?:[Tt ](\\d{2}):(\\d{2})(?::(\\d{2})(?:[.,](\\d+))?)?)?\\s*([Zz]|[+-]\\d{2}(?::?\\d{2})?)?$",
    )
    private val DATE_PREFIX = Regex("^(\\d{4})-(\\d{2})-(\\d{2})")

    fun parse(value: String?): Long {
        val text = RssText.clean(value, 80)
        if (text.isEmpty()) return 0
        val ms = rfc822(text) ?: iso(text) ?: prefix(text) ?: return 0
        return maxOf(0, ms)
    }

    fun rfc822(text: String): Long? {
        val tokens = text.replace(",", " ").split(' ').filter { it.isNotEmpty() }.toMutableList()
        if (tokens.isEmpty()) return null
        if (tokens[0].lowercase().take(3) in DAYS || tokens[0].endsWith(".")) tokens.removeAt(0)
        if (tokens.size < 3) return null
        var day = tokens[0]
        var month = tokens[1].lowercase()
        var year = tokens[2]
        if (month.take(3) !in MONTHS) {
            if (day.lowercase().take(3) in MONTHS) {
                val swap = day
                day = month
                month = swap.lowercase()
            } else {
                return null
            }
        }
        if (tokens.size < 4) return null
        var time = tokens[3]
        var zone = tokens.getOrNull(4) ?: ""
        if (year.contains(':')) {
            val swap = year
            year = time
            time = swap
            zone = tokens.getOrNull(4) ?: ""
        }
        val monthIndex = MONTHS.indexOf(month.take(3))
        if (monthIndex < 0) return null
        val dayNumber = day.trimEnd('.').toIntOrNull() ?: return null
        var yearNumber = year.trimEnd('.').toIntOrNull() ?: return null
        if (yearNumber < 100) yearNumber += if (yearNumber > 68) 1900 else 2000
        val clock = time.split(':')
        if (clock.size < 2 || clock.size > 3) return null
        val hour = clock[0].toIntOrNull() ?: return null
        val minute = clock[1].toIntOrNull() ?: return null
        val second = clock.getOrNull(2)?.toIntOrNull() ?: if (clock.size == 3) return null else 0
        val offset = zoneMinutes(zone)
        return runCatching {
            val local = LocalDateTime.of(yearNumber, monthIndex + 1, dayNumber, hour, minute, second)
            local.toInstant(ZoneOffset.UTC).toEpochMilli() - offset * 60_000L
        }.getOrNull()
    }

    private fun zoneMinutes(zone: String): Int {
        val lower = zone.lowercase()
        ZONES[lower]?.let { return toMinutes(it) }
        if ((zone.startsWith("+") || zone.startsWith("-")) && zone.length >= 5) {
            val number = zone.substring(1, 5).toIntOrNull() ?: return 0
            val minutes = toMinutes(number)
            return if (zone.startsWith("-")) -minutes else minutes
        }
        return 0
    }

    private fun toMinutes(hhmm: Int): Int {
        val sign = if (hhmm < 0) -1 else 1
        val value = kotlin.math.abs(hhmm)
        return sign * ((value / 100) * 60 + value % 100)
    }

    fun iso(text: String): Long? {
        val match = ISO.matchEntire(text) ?: return null
        val g = match.groupValues
        return runCatching {
            val hour = g[4].toIntOrNull() ?: 0
            val minute = g[5].toIntOrNull() ?: 0
            val second = g[6].toIntOrNull() ?: 0
            val nanos = g[7].takeIf { it.isNotEmpty() }?.padEnd(9, '0')?.take(9)?.toInt() ?: 0
            val local = LocalDateTime.of(g[1].toInt(), g[2].toInt(), g[3].toInt(), hour, minute, second, nanos)
            val zone = g[8]
            val offsetMinutes = when {
                zone.isEmpty() || zone.equals("z", true) -> 0
                else -> {
                    val digits = zone.substring(1).replace(":", "")
                    val h = digits.take(2).toInt()
                    val m = digits.drop(2).toIntOrNull() ?: 0
                    (h * 60 + m) * if (zone.startsWith("-")) -1 else 1
                }
            }
            local.toInstant(ZoneOffset.UTC).toEpochMilli() - offsetMinutes * 60_000L
        }.getOrNull()
    }

    private fun prefix(text: String): Long? {
        val match = DATE_PREFIX.find(text) ?: return null
        return runCatching {
            LocalDate.of(match.groupValues[1].toInt(), match.groupValues[2].toInt(), match.groupValues[3].toInt())
                .atStartOfDay().toInstant(ZoneOffset.UTC).toEpochMilli()
        }.getOrNull()
    }
}
