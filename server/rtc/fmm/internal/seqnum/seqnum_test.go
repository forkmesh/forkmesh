package seqnum

import "testing"

func TestUnwrapCrossesTheWrapBothWays(t *testing.T) {
	var u Unwrapper16
	if got := u.Unwrap(65534); got != 65534 {
		t.Fatal(got)
	}
	if got := u.Unwrap(1); got != 65537 {
		t.Fatal(got)
	}
	if got := u.Unwrap(65535); got != 65535 {
		t.Fatal(got)
	}
	if last, _ := u.Last(); last != 65537 {
		t.Fatal(last)
	}
	var w Unwrapper32
	w.Unwrap(0xFFFFFFF0)
	if got := w.Unwrap(0x10); got != 0x100000010 {
		t.Fatal(got)
	}
}
