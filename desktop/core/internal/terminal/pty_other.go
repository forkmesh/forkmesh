//go:build !linux && !darwin

package terminal

import "forkmesh.com/desktop-core/internal/rpc"

const supported = false

func startProcess(spec launch) (process, error) {
	return nil, rpc.Fail(errUnsupported)
}
