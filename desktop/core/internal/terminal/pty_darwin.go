//go:build darwin

package terminal

import (
	"os"
	"syscall"
	"unsafe"
)

func openPty() (*os.File, string, error) {
	master, err := os.OpenFile("/dev/ptmx", os.O_RDWR|syscall.O_NOCTTY|syscall.O_CLOEXEC, 0)
	if err != nil {
		return nil, "", err
	}
	if err := ioctl(master, syscall.TIOCPTYGRANT, nil); err != nil {
		master.Close()
		return nil, "", err
	}
	if err := ioctl(master, syscall.TIOCPTYUNLK, nil); err != nil {
		master.Close()
		return nil, "", err
	}
	name := make([]byte, 128)
	if err := ioctl(master, syscall.TIOCPTYGNAME, unsafe.Pointer(&name[0])); err != nil {
		master.Close()
		return nil, "", err
	}
	return master, cString(name), nil
}
