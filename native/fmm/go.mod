module forkmesh.com/fmm-mobile-build

go 1.24.0

toolchain go1.24.4

require (
	forkmesh.com/mirror-node v0.0.0
	golang.org/x/mobile v0.0.0-20260209203831-923679eb55af
)

require (
	golang.org/x/mod v0.33.0 // indirect
	golang.org/x/sync v0.19.0 // indirect
	golang.org/x/tools v0.42.0 // indirect
)

replace forkmesh.com/mirror-node => ../../server
