//go:build tools

package fmm

import (
	_ "forkmesh.com/mirror-node/rtc/fmmmobile"
	_ "golang.org/x/mobile/bind"
	_ "golang.org/x/mobile/cmd/gobind"
	_ "golang.org/x/mobile/cmd/gomobile"
)
