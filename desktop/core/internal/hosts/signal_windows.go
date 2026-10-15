//go:build windows

package hosts

import "os"

func signalNumber(state *os.ProcessState) int { return -1 }
