package mirrornode

import (
	"fmt"
	"os"
	"syscall"
	"unsafe"
)

func openPTY() (*os.File, *os.File, error) {
	master, err := os.OpenFile("/dev/ptmx", os.O_RDWR|syscall.O_NOCTTY, 0)
	if err != nil {
		return nil, nil, err
	}
	unlock := int32(0)
	if err := ioctl(master.Fd(), syscall.TIOCSPTLCK,
		uintptr(unsafe.Pointer(&unlock))); err != nil {
		master.Close()
		return nil, nil, err
	}
	var index uint32
	if err := ioctl(master.Fd(), syscall.TIOCGPTN,
		uintptr(unsafe.Pointer(&index))); err != nil {
		master.Close()
		return nil, nil, err
	}
	follower, err := os.OpenFile(
		fmt.Sprintf("/dev/pts/%d", index), os.O_RDWR|syscall.O_NOCTTY, 0)
	if err != nil {
		master.Close()
		return nil, nil, err
	}
	return master, follower, nil
}
