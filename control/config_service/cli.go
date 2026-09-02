package configservice

import (
	"context"
	"crypto/ed25519"
	"encoding/base64"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"strings"
	"time"
)

// RunCLI executes the administrative CLI. Key bytes and full configuration
// content are never written to stderr.
func RunCLI(ctx context.Context, args []string, stdin io.Reader, stdout io.Writer, stderr io.Writer) int {
	if len(args) == 0 {
		fmt.Fprintln(stderr, "error: command required")
		writeUsage(stderr)
		return 2
	}
	if args[0] == "help" || args[0] == "-h" || args[0] == "--help" {
		writeUsage(stdout)
		return 0
	}
	var result any
	var err error
	switch args[0] {
	case "version":
		result = CurrentBuildInfo()
	case "dry-run":
		result, err = cliDryRun(args[1:], stdin, stderr)
	case "health":
		result, err = cliHealth(args[1:], stderr)
	case "metrics":
		result, err = cliMetrics(ctx, args[1:], stderr)
	case "propose":
		result, err = cliPropose(ctx, args[1:], stdin, stderr)
	case "approve":
		result, err = cliApprove(ctx, args[1:], stderr)
	case "activate":
		result, err = cliActivate(ctx, args[1:], stderr)
	case "inspect":
		result, err = cliInspect(ctx, args[1:], stderr)
	case "audit":
		result, err = cliAudit(ctx, args[1:], stderr)
	case "rollback":
		result, err = cliRollback(ctx, args[1:], stderr)
	case "emergency-kill":
		result, err = cliKill(ctx, args[1:], stderr)
	default:
		err = fmt.Errorf("unknown command %q", args[0])
	}
	if err != nil {
		fmt.Fprintf(stderr, "error: %v\n", err)
		return 1
	}
	encoder := json.NewEncoder(stdout)
	encoder.SetIndent("", "  ")
	if err := encoder.Encode(result); err != nil {
		fmt.Fprintf(stderr, "error: write output: %v\n", err)
		return 1
	}
	return 0
}

type cliCommon struct {
	root, keyID, privatePath, publicPath, rolesPath string
	epoch                                           uint64
}

type cliAuth struct {
	actor, requestID string
}

func addServiceFlags(flags *flag.FlagSet, common *cliCommon) {
	flags.StringVar(&common.root, "root", "", "control state directory")
	flags.StringVar(&common.keyID, "key-id", "", "Ed25519 key identifier")
	flags.StringVar(&common.privatePath, "private-key", "", "base64 Ed25519 private-key file")
	flags.StringVar(&common.publicPath, "public-key", "", "base64 Ed25519 public-key file")
	flags.StringVar(&common.rolesPath, "bootstrap-roles", "", "bootstrap operator-role JSON file")
	flags.Uint64Var(&common.epoch, "authority-epoch", 0, "control authority epoch")
}

func addAuthFlags(flags *flag.FlagSet, auth *cliAuth) {
	flags.StringVar(&auth.actor, "actor", "", "authenticated operator identity")
	flags.StringVar(&auth.requestID, "request-id", "", "unique authorization request identifier")
}

func cliDryRun(args []string, stdin io.Reader, stderr io.Writer) (ValidationReport, error) {
	flags := flag.NewFlagSet("dry-run", flag.ContinueOnError)
	flags.SetOutput(stderr)
	path := flags.String("config", "-", "configuration JSON path or - for stdin")
	if err := flags.Parse(args); err != nil {
		return ValidationReport{}, err
	}
	var configuration Configuration
	if err := readCLIJSON(*path, stdin, &configuration); err != nil {
		return ValidationReport{}, err
	}
	return dryRunConfiguration(configuration), nil
}

func cliHealth(args []string, stderr io.Writer) (ServiceHealth, error) {
	flags := flag.NewFlagSet("health", flag.ContinueOnError)
	flags.SetOutput(stderr)
	var common cliCommon
	addServiceFlags(flags, &common)
	if err := flags.Parse(args); err != nil {
		return ServiceHealth{}, err
	}
	service, _, err := openCLIService(common, false)
	if err != nil {
		return ServiceHealth{}, err
	}
	return service.Health(), nil
}

func cliMetrics(ctx context.Context, args []string, stderr io.Writer) (ControlMetricsSnapshot, error) {
	flags := flag.NewFlagSet("metrics", flag.ContinueOnError)
	flags.SetOutput(stderr)
	var common cliCommon
	addServiceFlags(flags, &common)
	if err := flags.Parse(args); err != nil {
		return ControlMetricsSnapshot{}, err
	}
	service, _, err := openCLIService(common, false)
	if err != nil {
		return ControlMetricsSnapshot{}, err
	}
	return service.Metrics(ctx)
}

func cliPropose(ctx context.Context, args []string, stdin io.Reader, stderr io.Writer) (Proposal, error) {
	flags := flag.NewFlagSet("propose", flag.ContinueOnError)
	flags.SetOutput(stderr)
	var common cliCommon
	var auth cliAuth
	addServiceFlags(flags, &common)
	addAuthFlags(flags, &auth)
	path := flags.String("config", "-", "unsigned configuration JSON path or - for stdin")
	activation := flags.Int64("activation-wall-utc-ns", 0, "staged activation wall UTC nanoseconds")
	reason := flags.String("reason", "", "auditable change reason")
	if err := flags.Parse(args); err != nil {
		return Proposal{}, err
	}
	service, privateKey, err := openCLIService(common, true)
	if err != nil {
		return Proposal{}, err
	}
	var configuration Configuration
	if err := readCLIJSON(*path, stdin, &configuration); err != nil {
		return Proposal{}, err
	}
	finalized, err := FinalizeConfiguration(configuration)
	if err != nil {
		return Proposal{}, err
	}
	signed, err := SignConfiguration(finalized, common.keyID, privateKey)
	if err != nil {
		return Proposal{}, err
	}
	return service.Propose(ctx, ProposeRequest{Authorization: cliAuthorization(auth, common.epoch), Configuration: signed, ActivationWallUTCNS: *activation, Reason: *reason})
}

func cliApprove(ctx context.Context, args []string, stderr io.Writer) (Proposal, error) {
	flags := flag.NewFlagSet("approve", flag.ContinueOnError)
	flags.SetOutput(stderr)
	var common cliCommon
	var auth cliAuth
	addServiceFlags(flags, &common)
	addAuthFlags(flags, &auth)
	proposal := flags.String("proposal", "", "proposal identifier")
	reason := flags.String("reason", "", "auditable approval reason")
	if err := flags.Parse(args); err != nil {
		return Proposal{}, err
	}
	service, _, err := openCLIService(common, true)
	if err != nil {
		return Proposal{}, err
	}
	return service.Approve(ctx, ApprovalRequest{Authorization: cliAuthorization(auth, common.epoch), ProposalID: *proposal, Reason: *reason})
}

func cliActivate(ctx context.Context, args []string, stderr io.Writer) (SignedConfiguration, error) {
	flags := flag.NewFlagSet("activate", flag.ContinueOnError)
	flags.SetOutput(stderr)
	var common cliCommon
	var auth cliAuth
	addServiceFlags(flags, &common)
	addAuthFlags(flags, &auth)
	proposal := flags.String("proposal", "", "approved proposal identifier")
	reason := flags.String("reason", "", "auditable activation reason")
	if err := flags.Parse(args); err != nil {
		return SignedConfiguration{}, err
	}
	service, _, err := openCLIService(common, true)
	if err != nil {
		return SignedConfiguration{}, err
	}
	return service.Activate(ctx, ActivationRequest{Authorization: cliAuthorization(auth, common.epoch), ProposalID: *proposal, Reason: *reason})
}

func cliInspect(ctx context.Context, args []string, stderr io.Writer) (SignedConfiguration, error) {
	flags := flag.NewFlagSet("inspect", flag.ContinueOnError)
	flags.SetOutput(stderr)
	var common cliCommon
	var auth cliAuth
	addServiceFlags(flags, &common)
	addAuthFlags(flags, &auth)
	digest := flags.String("sha256", "", "configuration digest; empty selects active")
	if err := flags.Parse(args); err != nil {
		return SignedConfiguration{}, err
	}
	service, _, err := openCLIService(common, false)
	if err != nil {
		return SignedConfiguration{}, err
	}
	if *digest == "" {
		return service.GetActive(ctx, cliAuthorization(auth, common.epoch))
	}
	return service.Inspect(ctx, *digest, cliAuthorization(auth, common.epoch))
}

func cliAudit(ctx context.Context, args []string, stderr io.Writer) ([]AuditEntry, error) {
	flags := flag.NewFlagSet("audit", flag.ContinueOnError)
	flags.SetOutput(stderr)
	var common cliCommon
	var auth cliAuth
	addServiceFlags(flags, &common)
	addAuthFlags(flags, &auth)
	if err := flags.Parse(args); err != nil {
		return nil, err
	}
	service, _, err := openCLIService(common, false)
	if err != nil {
		return nil, err
	}
	return service.AuditTrail(ctx, cliAuthorization(auth, common.epoch))
}

func cliRollback(ctx context.Context, args []string, stderr io.Writer) (SignedConfiguration, error) {
	flags := flag.NewFlagSet("rollback", flag.ContinueOnError)
	flags.SetOutput(stderr)
	var common cliCommon
	var initiator cliAuth
	addServiceFlags(flags, &common)
	addAuthFlags(flags, &initiator)
	approver := flags.String("approver", "", "distinct authenticated approver")
	approverRequest := flags.String("approver-request-id", "", "distinct approver request identifier")
	expected := flags.String("expected-active-sha256", "", "compare-and-swap active digest")
	target := flags.String("target-sha256", "", "older immutable target digest")
	reason := flags.String("reason", "", "auditable rollback reason")
	if err := flags.Parse(args); err != nil {
		return SignedConfiguration{}, err
	}
	service, _, err := openCLIService(common, true)
	if err != nil {
		return SignedConfiguration{}, err
	}
	return service.Rollback(ctx, RollbackRequest{Initiator: cliAuthorization(initiator, common.epoch), Approver: cliAuthorization(cliAuth{actor: *approver, requestID: *approverRequest}, common.epoch), ExpectedActiveSHA256: *expected, TargetSHA256: *target, Reason: *reason})
}

func cliKill(ctx context.Context, args []string, stderr io.Writer) (SignedKillCommand, error) {
	flags := flag.NewFlagSet("emergency-kill", flag.ContinueOnError)
	flags.SetOutput(stderr)
	var common cliCommon
	var auth cliAuth
	addServiceFlags(flags, &common)
	addAuthFlags(flags, &auth)
	commandID := flags.String("command-id", "", "unique kill command identifier")
	scope := flags.String("scope", "", "FIRM, ACCOUNT, VENUE, STRATEGY, or SYMBOL")
	target := flags.String("target", "", "scope target; empty for FIRM")
	sequence := flags.Uint64("sequence", 0, "strictly increasing kill sequence")
	reasonCode := flags.String("reason-code", "", "bounded machine-readable reason")
	if err := flags.Parse(args); err != nil {
		return SignedKillCommand{}, err
	}
	service, _, err := openCLIService(common, true)
	if err != nil {
		return SignedKillCommand{}, err
	}
	return service.EmergencyKill(ctx, EmergencyKillRequest{Authorization: cliAuthorization(auth, common.epoch), CommandID: *commandID, Scope: KillScope(strings.ToUpper(*scope)), TargetID: *target, CommandSequence: *sequence, ReasonCode: *reasonCode})
}

func openCLIService(common cliCommon, mutable bool) (*Service, ed25519.PrivateKey, error) {
	if common.root == "" || common.keyID == "" || common.rolesPath == "" || common.epoch == 0 {
		return nil, nil, errors.New("root, key-id, bootstrap-roles, and authority-epoch are required")
	}
	var roles []OperatorRoleBinding
	if err := readCLIJSON(common.rolesPath, nil, &roles); err != nil {
		return nil, nil, fmt.Errorf("read bootstrap roles: %w", err)
	}
	var privateKey ed25519.PrivateKey
	trusted := make(map[string]ed25519.PublicKey)
	if mutable {
		decoded, err := readBase64Key(common.privatePath, ed25519.PrivateKeySize, true)
		if err != nil {
			return nil, nil, err
		}
		privateKey = ed25519.PrivateKey(decoded)
		trusted[common.keyID] = append(ed25519.PublicKey(nil), privateKey.Public().(ed25519.PublicKey)...)
	} else {
		decoded, err := readBase64Key(common.publicPath, ed25519.PublicKeySize, false)
		if err != nil {
			return nil, nil, err
		}
		trusted[common.keyID] = ed25519.PublicKey(decoded)
	}
	service, err := Open(ServiceConfig{Root: common.root, TrustedKeys: trusted, SignerKeyID: common.keyID, Signer: privateKey, BootstrapRoles: roles, AuthorityEpoch: common.epoch})
	return service, privateKey, err
}

func readBase64Key(path string, expected int, private bool) ([]byte, error) {
	if path == "" {
		return nil, errors.New("key path is required")
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	if private {
		info, statErr := file.Stat()
		if statErr != nil {
			return nil, statErr
		}
		if info.Mode().Perm()&0o077 != 0 {
			return nil, errors.New("private key file must not be group/world accessible")
		}
	}
	payload, err := io.ReadAll(io.LimitReader(file, 1024))
	if err != nil {
		return nil, err
	}
	decoded, err := base64.StdEncoding.Strict().DecodeString(strings.TrimSpace(string(payload)))
	if err != nil || len(decoded) != expected {
		return nil, errors.New("invalid base64 Ed25519 key file")
	}
	return decoded, nil
}

func readCLIJSON(path string, stdin io.Reader, destination any) error {
	var payload []byte
	var err error
	if path == "-" {
		if stdin == nil {
			return errors.New("stdin is unavailable")
		}
		payload, err = io.ReadAll(io.LimitReader(stdin, maximumConfigurationBytes+1))
	} else {
		payload, err = os.ReadFile(path)
	}
	if err != nil {
		return err
	}
	if int64(len(payload)) > maximumConfigurationBytes {
		return errors.New("JSON input exceeds size bound")
	}
	return decodeStrict(payload, destination)
}

func cliAuthorization(auth cliAuth, epoch uint64) AuthContext {
	now := time.Now().UTC().UnixNano()
	return AuthContext{Authenticated: true, Principal: auth.actor, RequestID: auth.requestID, AuthorityEpoch: epoch, AuthenticatedUTCNS: now, ExpiresUTCNS: now + int64(5*time.Minute)}
}

func writeUsage(writer io.Writer) {
	fmt.Fprintln(writer, "usage: config-service <version|health|metrics|dry-run|propose|approve|activate|inspect|audit|rollback|emergency-kill> [flags]")
}
