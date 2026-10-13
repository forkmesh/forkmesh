package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"

	mirrornode "forkmesh.com/mirror-node"
)

func main() {
	configPath := flag.String("config", "", "absolute mirror-node configuration path")
	initConfig := flag.Bool("init", false, "initialize a fresh mirror-node configuration")
	stateDirectory := flag.String("state-dir", "", "absolute persistent state directory")
	node := flag.String("node", "", "mirror node name")
	owner := flag.String("owner", "forkmesh", "canonical repository owner")
	repository := flag.String("repository", "forkmesh", "repository name")
	upstream := flag.String("upstream", "https://forkmesh.com/forkmesh/forkmesh", "round-robin upstream URL")
	catalogURL := flag.String("catalog-url", "https://app.forkmesh.com/api/repositories", "catalog publication URL")
	publicOrigin := flag.String("public-origin", "", "public HTTPS mirror origin")
	mirrorURL := flag.String("mirror-url", "", "optional public HTTPS Git origin")
	routerPublicKey := flag.String("router-public-key", "", "mirror router Ed25519 public key")
	gatewayScript := flag.String("gateway-script", "/usr/local/share/forkmesh/tools/mirror_gateway.py", "mirror gateway script")
	python := flag.String("python", "/usr/bin/python3", "Python executable")
	cloudflared := flag.String("cloudflared", "/usr/local/bin/cloudflared", "cloudflared executable")
	disableCloudflared := flag.Bool("disable-cloudflared", false, "serve through an operator HTTPS proxy without Cloudflare Tunnel")
	version := flag.String("version", "dev", "reported ForkMesh version")
	setVersion := flag.String("set-version", "", "update the reported release version and exit")
	printPublicKey := flag.Bool("public-key", false, "print the configured node public key")
	registerLinkCode := flag.String("register-link-code", "", "register this node with a six-digit installer link code")
	accountsURL := flag.String("accounts-url", "https://app.forkmesh.com/api/accounts", "node account API base URL")
	signHealth := flag.Bool("sign-mirror-health", false, "run the bounded gateway health signer")
	verifyCapability := flag.Bool("verify-mirror-capability", false, "run the bounded gateway capability verifier")
	signManifest := flag.Bool("sign-mirror-manifest", false, "run the bounded mirror manifest signer")
	executeMerge := flag.Bool("execute-mirror-merge", false, "run the bounded owner-authorized pull merge/update executor")
	signCheck := flag.Bool("sign-check-attestation", false, "sign one forkmesh-check-v1 status check attestation read from stdin")
	check := flag.Bool("check", false, "validate configuration and runtime dependencies")
	preflight := flag.Bool("preflight", false, "run read-only deployment, capacity, compatibility, and recovery checks and exit")
	jsonOutput := flag.Bool("json", false, "print --preflight results as JSON")
	backup := flag.String("backup", "", "write a sealed node backup to this absolute path and exit")
	restore := flag.String("restore", "", "restore a sealed node backup and exit")
	restoreDrill := flag.String("restore-drill", "", "restore a sealed node backup into scratch space, verify it, record the result, and exit")
	restoreRoot := flag.String("restore-root", "/", "absolute directory that every restored path is placed under")
	workDirectory := flag.String("work-dir", "", "absolute scratch directory for restores (default TMPDIR)")
	recoveryObjective := flag.Duration("rto", 30*time.Minute, "recovery time objective a restore or drill must meet")
	exportIdentity := flag.String("export-identity", "", "write a sealed export of the node identity key to this absolute path and exit")
	importIdentity := flag.String("import-identity", "", "install the node identity key from a sealed export and exit")
	replaceIdentity := flag.Bool("replace-identity", false, "let --import-identity replace a different key (the current key is preserved beside it)")
	dryRun := flag.Bool("dry-run", false, "verify --restore or --import-identity without changing the node")
	passphraseFile := flag.String("passphrase-file", "", "absolute owner-only file holding the backup or identity-export passphrase")
	flag.Parse()
	if !filepath.IsAbs(*configPath) {
		fail("--config must be absolute")
	}
	operations := 0
	for _, selected := range []bool{*preflight, *backup != "", *restore != "", *restoreDrill != "", *exportIdentity != "", *importIdentity != ""} {
		if selected {
			operations++
		}
	}
	if operations > 1 {
		fail("choose one of --preflight, --backup, --restore, --restore-drill, --export-identity, or --import-identity")
	}
	if *dryRun && *restore == "" && *importIdentity == "" {
		fail("--dry-run applies to --restore and --import-identity")
	}
	if *preflight {
		report := mirrornode.RunNodePreflight(context.Background(), mirrornode.PreflightOptions{ConfigPath: *configPath})
		if *jsonOutput {
			printJSON(report)
		} else {
			fmt.Print(report.Text())
		}
		if !report.OK {
			os.Exit(2)
		}
		return
	}
	if *restore != "" || *restoreDrill != "" {
		archive, drill := *restore, *dryRun
		if *restoreDrill != "" {
			archive, drill = *restoreDrill, true
		}
		passphrase := readPassphrase(*passphraseFile)
		ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
		report, err := mirrornode.RestoreNodeBackup(ctx, mirrornode.NodeRestoreOptions{
			Archive: archive, Passphrase: passphrase, ConfigPath: *configPath,
			Root: *restoreRoot, Drill: drill, WorkDirectory: *workDirectory,
			RecoveryTimeObjective: *recoveryObjective,
		})
		stop()
		if err != nil {
			fail("restore: " + err.Error())
		}
		if drill {
			// Drills of this node's own backups are recorded on the live node.
			if cfg, err := mirrornode.LoadConfig(*configPath); err == nil {
				if err := mirrornode.RecordRestoreDrill(cfg, report); err != nil {
					fmt.Fprintln(os.Stderr, "forkmesh-mirror-node: restore drill not recorded:", err)
				}
			}
		}
		printJSON(report)
		if !report.WithinObjective {
			fmt.Fprintln(os.Stderr, "forkmesh-mirror-node: the restore exceeded its recovery time objective")
			os.Exit(3)
		}
		return
	}
	if *initConfig && *setVersion != "" {
		fail("--init and --set-version cannot be combined")
	}
	if *initConfig {
		publicKey, err := mirrornode.Bootstrap(mirrornode.BootstrapOptions{
			ConfigPath: *configPath, StateDirectory: *stateDirectory, Node: *node,
			Owner: *owner, Repository: *repository, Upstream: *upstream,
			CatalogURL: *catalogURL, PublicOrigin: *publicOrigin,
			MirrorURL:       *mirrorURL,
			RouterPublicKey: *routerPublicKey, GatewayScript: *gatewayScript,
			Python: *python, Cloudflared: *cloudflared,
			DisableCloudflared: *disableCloudflared, Version: *version,
		})
		if err != nil {
			fail(err.Error())
		}
		fmt.Println(publicKey)
		return
	}
	if *setVersion != "" {
		if err := mirrornode.UpdateConfigVersion(*configPath, *setVersion); err != nil {
			fail(err.Error())
		}
		fmt.Println("mirror-node version updated")
		return
	}
	cfg, err := mirrornode.LoadConfig(*configPath)
	if err != nil {
		fail(err.Error())
	}
	gateway, _, err := mirrornode.LoadGatewayConfigForCommand(cfg.GatewayConfig)
	if err != nil {
		fail(err.Error())
	}
	if *backup != "" {
		passphrase := readPassphrase(*passphraseFile)
		ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
		report, err := mirrornode.CreateNodeBackup(ctx, mirrornode.NodeBackupOptions{
			ConfigPath: *configPath, Output: *backup, Passphrase: passphrase,
		})
		stop()
		if err != nil {
			fail(err.Error())
		}
		printJSON(report)
		return
	}
	if *exportIdentity != "" {
		report, err := mirrornode.ExportNodeIdentity(cfg, gateway.Node.Name, *exportIdentity, readPassphrase(*passphraseFile), time.Now())
		if err != nil {
			fail(err.Error())
		}
		printJSON(report)
		return
	}
	if *importIdentity != "" {
		report, err := mirrornode.ImportNodeIdentity(cfg, mirrornode.IdentityImportOptions{
			Input: *importIdentity, Passphrase: readPassphrase(*passphraseFile),
			Replace: *replaceIdentity, DryRun: *dryRun, Now: time.Now(),
		})
		if err != nil {
			fail("import identity: " + err.Error())
		}
		printJSON(report)
		return
	}
	if *registerLinkCode != "" {
		identity, err := mirrornode.LoadIdentity(cfg.IdentityKey)
		if err != nil {
			fail(err.Error())
		}
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		if err := mirrornode.RegisterNodeAccount(ctx, *accountsURL, gateway.Node.Name, *registerLinkCode, identity); err != nil {
			fail("register node: " + err.Error())
		}
		fmt.Println("node account registered")
		return
	}
	if *signHealth || *verifyCapability || *signManifest || *printPublicKey || *executeMerge || *signCheck {
		identity, err := mirrornode.LoadIdentity(cfg.IdentityKey)
		if err != nil {
			fail(err.Error())
		}
		if *printPublicKey {
			fmt.Println(identity.PublicKey())
			return
		}
		if *executeMerge {
			if err := mirrornode.RunPullMutationExecutor(os.Stdin, os.Stdout, cfg, gateway, identity); err != nil {
				fail("merge executor: " + err.Error())
			}
			return
		}
		if *signCheck {
			if err := mirrornode.RunCheckSigner(os.Stdin, os.Stdout, identity); err != nil {
				fail("check signer: " + err.Error())
			}
			return
		}
		if *signManifest {
			if err := mirrornode.RunManifestSigner(os.Stdin, os.Stdout, identity); err != nil {
				fail("manifest signer: " + err.Error())
			}
			return
		}
		if *signHealth {
			if err := mirrornode.RunHealthSigner(os.Stdin, os.Stdout, identity, gateway.Node.Name); err != nil {
				fail("health signer: " + err.Error())
			}
			return
		}
		if err := mirrornode.RunCapabilityVerifier(os.Stdin, os.Stdout, gateway.Node.Name, gateway.RouterPublicKey); err != nil {
			fail("capability verifier: " + err.Error())
		}
		return
	}
	if err := mirrornode.ValidateRuntime(cfg); err != nil {
		fail(err.Error())
	}
	if *check {
		fmt.Println("mirror-node configuration OK")
		return
	}
	if err := os.Setenv("FORKMESH_MIRROR_NODE_CONFIG", *configPath); err != nil {
		fail(err.Error())
	}
	daemon, err := mirrornode.NewDaemon(cfg)
	if err != nil {
		fail(err.Error())
	}
	executable, err := os.Executable()
	if err != nil {
		fail(err.Error())
	}
	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()
	if err := daemon.Run(ctx, executable); err != nil {
		fail(err.Error())
	}
}

func readPassphrase(path string) []byte {
	if path == "" {
		fail("--passphrase-file is required")
	}
	passphrase, err := mirrornode.ReadPassphraseFile(path)
	if err != nil {
		fail(err.Error())
	}
	return passphrase
}

func printJSON(value any) {
	raw, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		fail(err.Error())
	}
	fmt.Println(string(raw))
}

func fail(message string) { fmt.Fprintln(os.Stderr, "forkmesh-mirror-node:", message); os.Exit(2) }
