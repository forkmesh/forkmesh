package com.forkmesh.nativeapp.format

import com.forkmesh.nativeapp.testing.assertEq

class FormatTest {
    private val now = 1_786_636_663_064L
    private val minute = 60_000L
    private val hour = 60 * minute
    private val day = 24 * hour

    fun testRelativeTimeSteps() {
        assertEq("just now", relativeTimeFrom(now - 59_999, now))
        assertEq("1m ago", relativeTimeFrom(now - minute, now))
        assertEq("59m ago", relativeTimeFrom(now - hour + 1, now))
        assertEq("1h ago", relativeTimeFrom(now - hour, now))
        assertEq("23h ago", relativeTimeFrom(now - day + 1, now))
        assertEq("1d ago", relativeTimeFrom(now - day, now))
        assertEq("29d ago", relativeTimeFrom(now - 29 * day, now))
        assertEq("1mo ago", relativeTimeFrom(now - 30 * day, now))
        assertEq("11mo ago", relativeTimeFrom(now - 359 * day, now))
        assertEq("0y ago", relativeTimeFrom(now - 360 * day, now))
        assertEq("1y ago", relativeTimeFrom(now - 365 * day, now))
        assertEq("2y ago", relativeTimeFrom(now - 800 * day, now))
    }

    fun testClockAheadReadsAsJustNow() {
        assertEq("just now", relativeTimeFrom(now + 5 * minute, now))
    }

    fun testZeroEpochIsNever() {
        assertEq("never", relativeTime(0, now))
        assertEq("never", relativeTime(-1, now))
        assertEq("3h ago", relativeTime(now - 3 * hour, now))
        assertEq("never", absoluteDate(0))
    }

    fun testAbsoluteDateIsUtc() {
        assertEq("2026-08-12", absoluteDate(1_786_577_400_000L))
        assertEq("1970-01-02", absoluteDate(86_400_000L))
    }

    fun testCountOrNullHidesZero() {
        assertEq(null, countOrNull(0))
        assertEq("3", countOrNull(3))
        assertEq("1 star", countOrNull(1, singular = "star"))
        assertEq("2 stars", countOrNull(2, singular = "star"))
        assertEq("2 forks", countOrNull(2, singular = "fork"))
        assertEq("2 replies", countOrNull(2, singular = "reply", plural = "replies"))
    }
}
