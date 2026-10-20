package com.forkmesh.nativeapp.health

import java.time.LocalDate
import java.time.temporal.ChronoUnit
import kotlin.math.max
import kotlin.math.min

data class SugarContractLine(val id: Int, val name: String, val caption: String)

data class SugarContract(val startDay: String, val endDay: String, val names: List<String>) {
    val signers: Int get() = names.size

    fun withSigners(count: Int): SugarContract {
        val next = clamp(count)
        val resized = when {
            next < names.size -> names.take(next)
            next > names.size -> names + List(next - names.size) { "" }
            else -> names
        }
        return copy(names = resized)
    }

    fun withName(index: Int, name: String): SugarContract {
        if (index !in names.indices) return this
        return copy(names = names.toMutableList().also { it[index] = name.take(MAX_NAME_LENGTH) })
    }

    fun withStart(day: String): SugarContract = copy(startDay = day, endDay = if (endDay < day) day else endDay)

    fun withEnd(day: String): SugarContract = copy(endDay = if (day < startDay) startDay else day)

    val isOneDay: Boolean get() = startDay == endDay

    val days: Int
        get() {
            val start = HealthDates.parseDay(startDay) ?: return 1
            val end = HealthDates.parseDay(endDay) ?: return 1
            return max(1, ChronoUnit.DAYS.between(start, end).toInt() + 1)
        }

    val dateLine: String
        get() {
            if (isOneDay) return SugarContractText.full(startDay)
            if (SugarContractText.year(startDay) == SugarContractText.year(endDay)) {
                return "${SugarBreakText.monthDay(startDay)} – ${SugarContractText.full(endDay)}"
            }
            return "${SugarContractText.full(startDay)} – ${SugarContractText.full(endDay)}"
        }

    val lengthLine: String get() = SugarBreakText.days(days)

    val terms: String
        get() {
            val who = if (signers == 1) "I, the undersigned, promise" else "We, the undersigned, promise"
            if (isOneDay) return "$who to eat no added sugar on ${SugarContractText.full(startDay)}."
            return "$who to eat no added sugar from ${SugarBreakText.monthDay(startDay)} through ${SugarContractText.full(endDay)}."
        }

    val lines: List<SugarContractLine>
        get() = names.mapIndexed { index, raw ->
            val name = raw.trim()
            SugarContractLine(index, name, name.ifEmpty { "Signature" })
        }

    val message: String
        get() {
            val whenText = if (isOneDay) "on $dateLine" else "for $dateLine ($lengthLine)"
            val ask = if (signers == 1) "Hold me to it." else "Sign the contract and go sugar-free with me."
            return "No sugar $whenText. $ask"
        }

    val fileName: String get() = "no-sugar-contract-$startDay.png"

    companion object {
        const val TITLE = "No Sugar Contract"
        const val BRAND = "SUGAR BREAK"
        val SIGNER_RANGE = 1..12
        const val DEFAULT_SIGNERS = 2
        const val MAX_NAME_LENGTH = 40
        const val FINEPRINT = "No sweets · No soda · No desserts"

        fun clamp(signers: Int): Int = min(SIGNER_RANGE.last, max(SIGNER_RANGE.first, signers))
    }
}

fun SugarContract(startDay: String, endDay: String, signers: Int = SugarContract.DEFAULT_SIGNERS): SugarContract =
    SugarContract(startDay, if (endDay < startDay) startDay else endDay, List(SugarContract.clamp(signers)) { "" })

object SugarContractText {
    fun year(day: String): Int = day.split("-").firstOrNull()?.toIntOrNull() ?: 0

    fun full(day: String): String {
        val year = year(day)
        val label = SugarBreakText.monthDay(day)
        return if (year > 0 && label != day) "$label, $year" else day
    }
}

fun SugarBreakModel.contract(signers: Int = SugarContract.DEFAULT_SIGNERS): SugarContract {
    val current = today
    if (isActive && endDay.isNotEmpty() && endDay >= current) {
        return SugarContract(maxOf(startDay, current), endDay, signers)
    }
    val length = min(365, max(1, selectedLength))
    val end = (HealthDates.parseDay(current) ?: LocalDate.now()).plusDays((length - 1).toLong()).toString()
    return SugarContract(current, end, signers)
}
