package store

import "testing"

func TestPut(t *testing.T) {
	s := New()
	if err := s.Put("a", 1); err != nil {
		t.Fatal(err)
	}
}
