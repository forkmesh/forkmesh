package netlog

import (
	"encoding/json"
	"math"
	"time"
)

const MaxTimelineDays = 30

func ClampDays(raw json.RawMessage) int {
	days := MaxTimelineDays
	var number float64
	if len(raw) > 0 && string(raw) != "null" && json.Unmarshal(raw, &number) == nil {
		switch {
		case math.IsNaN(number):
			days = 1
		case number >= MaxTimelineDays:
			days = MaxTimelineDays
		case number < 1:
			days = 1
		default:
			days = int(number)
		}
	}
	if days < 1 {
		days = 1
	}
	if days > MaxTimelineDays {
		days = MaxTimelineDays
	}
	return days
}

func localMidnight(year int, month time.Month, day int) int64 {
	return time.Date(year, month, day, 0, 0, 0, 0, time.Local).UnixMilli()
}

func LocalDayStartMs(ms int64) int64 {
	t := time.UnixMilli(ms).In(time.Local)
	return localMidnight(t.Year(), t.Month(), t.Day())
}

func Timeline(digest Digest, now time.Time, days int) map[string]any {
	if days < 1 {
		days = 1
	}
	if days > MaxTimelineDays {
		days = MaxTimelineDays
	}
	today := now.In(time.Local)
	year, month, day := today.Year(), today.Month(), today.Day()
	type row struct {
		start    int64
		errors   int
		warnings int
		info     int
	}
	rows := make([]row, 0, days)
	index := map[int64]int{}
	for i := days - 1; i >= 0; i-- {
		start := localMidnight(year, month, day-i)
		if _, ok := index[start]; !ok {
			index[start] = len(rows)
		}
		rows = append(rows, row{start: start})
	}
	windowStart := rows[0].start
	windowEnd := localMidnight(year, month, day+1)
	for bucket, categories := range digest {
		if bucket < windowStart || bucket >= windowEnd {
			continue
		}
		at, ok := index[LocalDayStartMs(bucket)]
		if !ok {
			continue
		}
		for badge, count := range categories {
			if count < 0 {
				count = 0
			}
			switch SeverityForBadge(badge) {
			case "error":
				rows[at].errors += count
			case "warning":
				rows[at].warnings += count
			default:
				rows[at].info += count
			}
		}
	}
	out := make([]any, 0, len(rows))
	for _, r := range rows {
		out = append(out, map[string]any{
			"dayTs":    float64(r.start),
			"errors":   r.errors,
			"warnings": r.warnings,
			"info":     r.info,
		})
	}
	return map[string]any{"days": out}
}
