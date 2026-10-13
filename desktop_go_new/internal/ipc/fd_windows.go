//go:build windows

package ipc

import "os"

func receivedFile(oob []byte) *os.File { return nil }
