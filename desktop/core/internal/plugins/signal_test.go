package plugins

import (
	"os"
	"syscall"
)

func nilSignal() os.Signal { return syscall.Signal(0) }
