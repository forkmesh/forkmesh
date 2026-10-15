//go:build linux

package terminal

import (
	"fmt"
	"os"
	"syscall"
	"unsafe"
)

func openPty() (*os.File, string, error) {
	master, err := os.OpenFile("/dev/ptmx", os.O_RDWR|syscall.O_NOCTTY|syscall.O_CLOEXEC, 0)
	if err != nil {
		return nil, "", err
	}
	unlock := int32(0)
	if err := ioctl(master, syscall.TIOCSPTLCK, unsafe.Pointer(&unlock)); err != nil {
		master.Close()
		return nil, "", err
	}
	var index uint32
	if err := ioctl(master, syscall.TIOCGPTN, unsafe.Pointer(&index)); err != nil {
		master.Close()
		return nil, "", err
	}
	return master, fmt.Sprintf("/dev/pts/%d", index), nil
}
