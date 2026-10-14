package mirrornode

import (
	"bytes"
	"errors"
	"os"
	"syscall"
	"unsafe"
)

func openPTY() (*os.File, *os.File, error) {
	master, err := os.OpenFile("/dev/ptmx", os.O_RDWR|syscall.O_NOCTTY, 0)
	if err != nil {
		return nil, nil, err
	}
	if err := ioctl(master.Fd(), syscall.TIOCPTYGRANT, 0); err != nil {
		master.Close()
		return nil, nil, err
	}
	if err := ioctl(master.Fd(), syscall.TIOCPTYUNLK, 0); err != nil {
		master.Close()
		return nil, nil, err
	}
	name := make([]byte, 128)
	if err := ioctl(master.Fd(), syscall.TIOCPTYGNAME,
		uintptr(unsafe.Pointer(&name[0]))); err != nil {
		master.Close()
		return nil, nil, err
	}
	end := bytes.IndexByte(name, 0)
	if end <= 0 {
		master.Close()
		return nil, nil, errors.New("the pty has no follower name")
	}
	follower, err := os.OpenFile(string(name[:end]), os.O_RDWR|syscall.O_NOCTTY, 0)
	if err != nil {
		master.Close()
		return nil, nil, err
	}
	return master, follower, nil
}
