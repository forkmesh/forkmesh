//go:build !windows

package ipc

import (
	"os"
	"syscall"
)

func receivedFile(oob []byte) *os.File {
	messages, err := syscall.ParseSocketControlMessage(oob)
	if err != nil {
		return nil
	}
	var file *os.File
	for _, message := range messages {
		fds, err := syscall.ParseUnixRights(&message)
		if err != nil {
			continue
		}
		for _, fd := range fds {
			if file == nil {
				syscall.CloseOnExec(fd)
				file = os.NewFile(uintptr(fd), "adopted")
			} else {
				syscall.Close(fd)
			}
		}
	}
	return file
}
