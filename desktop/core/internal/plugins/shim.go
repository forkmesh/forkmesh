package plugins

import (
	"flag"
	"fmt"
	"os"
	"runtime"
)

func UsesShim() bool { return runtime.GOOS == "linux" || runtime.GOOS == "darwin" }

func ShimArgs(memoryMB int, argv []string) []string {
	return append([]string{"plugin-shim", "--nofile", "256", "--memory-mb", fmt.Sprint(memoryMB), "--"}, argv...)
}

func ShimMain(args []string) int {
	flags := flag.NewFlagSet("plugin-shim", flag.ContinueOnError)
	nofile := flags.Uint64("nofile", 256, "open file limit")
	memoryMB := flags.Uint64("memory-mb", 512, "data segment limit")
	if err := flags.Parse(args); err != nil {
		return 2
	}
	argv := flags.Args()
	if len(argv) == 0 {
		fmt.Fprintln(os.Stderr, "plugin-shim: no command")
		return 2
	}
	if err := applyLimits(*nofile, *memoryMB<<20); err != nil {
		fmt.Fprintln(os.Stderr, "plugin-shim: limits:", err)
		return 126
	}
	if err := execRestricted(argv, os.Environ()); err != nil {
		fmt.Fprintln(os.Stderr, "plugin-shim: exec:", err)
		return 127
	}
	return 0
}
