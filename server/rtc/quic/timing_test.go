package quic

import (
	"fmt"
	"strings"
	"testing"
)

const timingAttempts = 3

func retryTiming(t *testing.T, run func(fail func(format string, args ...any))) {
	t.Helper()
	var problems []string
	for attempt := 1; attempt <= timingAttempts; attempt++ {
		problems = nil
		run(func(format string, args ...any) { problems = append(problems, fmt.Sprintf(format, args...)) })
		if len(problems) == 0 {
			if attempt > 1 {
				t.Logf("passed on attempt %d of %d", attempt, timingAttempts)
			}
			return
		}
		if attempt < timingAttempts {
			t.Logf("attempt %d of %d missed: %s", attempt, timingAttempts, strings.Join(problems, "; "))
		}
	}
	for _, p := range problems {
		t.Error(p)
	}
}
