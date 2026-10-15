package main

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"

	"forkmesh.com/desktop-core/internal/identity"
	"forkmesh.com/desktop-core/internal/paths"
	"forkmesh.com/desktop-core/internal/plugins"
)

func pluginShim(args []string) int { return plugins.ShimMain(args) }

func pluginsCommand(args []string) int {
	flags, project, stateDir, _, err := projectFlags("plugins", args)
	if err != nil || flags.NArg() < 1 {
		usage()
		return 2
	}
	layout := paths.Resolve(*project, nil, "")
	switch flags.Arg(0) {
	case "list":
		result, err := callCore(*project, *stateDir, "core.plugins.list", nil)
		if err != nil {
			fmt.Fprintln(os.Stderr, "forkmesh-core:", err)
			return 1
		}
		var value any
		_ = json.Unmarshal(result, &value)
		printJSON(value)
		return 0
	case "sign":
		if flags.NArg() != 2 {
			usage()
			return 2
		}
		signer, err := identity.Load(layout.IdentityKey())
		if err != nil {
			fmt.Fprintln(os.Stderr, "forkmesh-core: no node identity to sign with:", err)
			return 1
		}
		signature, err := plugins.Sign(flags.Arg(1), signer)
		if err != nil {
			fmt.Fprintln(os.Stderr, "forkmesh-core:", err)
			return 1
		}
		printJSON(map[string]any{"package": signature.Package, "version": signature.Version,
			"signer": signature.Signer, "files": len(signature.Files)})
		return 0
	case "verify":
		if flags.NArg() != 2 {
			usage()
			return 2
		}
		dir := flags.Arg(1)
		manifest, err := plugins.LoadManifest(dir)
		if err != nil && !errors.Is(err, plugins.ErrNoService) {
			fmt.Fprintln(os.Stderr, "forkmesh-core:", err)
			return 1
		}
		signature, err := plugins.Verify(dir, manifest)
		if err != nil {
			fmt.Fprintln(os.Stderr, "forkmesh-core:", err)
			return 1
		}
		printJSON(map[string]any{"package": signature.Package, "version": signature.Version,
			"signer": signature.Signer, "files": len(signature.Files), "verified": true})
		return 0
	case "trust":
		if flags.NArg() < 2 {
			usage()
			return 2
		}
		key := flags.Arg(1)
		if _, err := identity.Decode(key, 32); err != nil {
			fmt.Fprintln(os.Stderr, "forkmesh-core: not an Ed25519 public key")
			return 1
		}
		name := key[:8]
		if flags.NArg() > 2 {
			name = flags.Arg(2)
		}
		path := filepath.Join(layout.Config, "marketplace", "trusted-signers.json")
		var file struct {
			Signers []map[string]string `json:"signers"`
		}
		if raw, err := os.ReadFile(path); err == nil {
			_ = json.Unmarshal(raw, &file)
		}
		for _, signer := range file.Signers {
			if signer["key"] == key {
				fmt.Println("already trusted")
				return 0
			}
		}
		file.Signers = append(file.Signers, map[string]string{"key": key, "name": name})
		raw, _ := json.MarshalIndent(file, "", "  ")
		if err := os.MkdirAll(filepath.Dir(path), 0o700); err == nil {
			err = os.WriteFile(path+".tmp", append(raw, '\n'), 0o600)
			if err == nil {
				err = os.Rename(path+".tmp", path)
			}
		}
		if err != nil {
			fmt.Fprintln(os.Stderr, "forkmesh-core:", err)
			return 1
		}
		fmt.Printf("trusted %s (%s)\n", key, name)
		return 0
	}
	usage()
	return 2
}
