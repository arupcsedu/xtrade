package modelregistry

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
)

// RunCLI executes one model-registry command and returns a process exit code.
// It never prints key material or artifact contents.
func RunCLI(ctx context.Context, args []string, stdin io.Reader, stdout io.Writer, stderr io.Writer) int {
	if len(args) == 0 {
		writeCLIError(stderr, errors.New("a command is required"))
		writeCLIUsage(stderr)
		return 2
	}
	var result any
	var err error
	switch args[0] {
	case "version":
		result = CurrentBuildInfo()
	case "health":
		result, err = runHealth(args[1:], stderr)
	case "register":
		result, err = runRegister(ctx, args[1:], stdin, stderr)
	case "validate":
		result, err = runValidate(ctx, args[1:], stdin, stderr)
	case "approve":
		result, err = runApprove(ctx, args[1:], stdin, stderr)
	case "deploy-shadow":
		result, err = runDeploy(ctx, args[1:], stdin, stderr, false)
	case "promote-canary":
		result, err = runDeploy(ctx, args[1:], stdin, stderr, true)
	case "promote":
		result, err = runPromote(ctx, args[1:], stdin, stderr)
	case "rollback":
		result, err = runRollback(ctx, args[1:], stdin, stderr)
	case "disable":
		result, err = runDisable(ctx, args[1:], stderr)
	case "inspect-lineage":
		result, err = runInspect(ctx, args[1:], stderr)
	default:
		writeCLIError(stderr, fmt.Errorf("unknown command %q", args[0]))
		writeCLIUsage(stderr)
		return 2
	}
	if err != nil {
		writeCLIError(stderr, err)
		return 1
	}
	encoder := json.NewEncoder(stdout)
	encoder.SetIndent("", "  ")
	if err := encoder.Encode(result); err != nil {
		writeCLIError(stderr, fmt.Errorf("write output: %w", err))
		return 1
	}
	return 0
}

type commonCLIFlags struct {
	root           string
	keyID          string
	privateKeyPath string
	publicKeyPath  string
	actor          string
	reason         string
}

func addCommonFlags(flags *flag.FlagSet, common *commonCLIFlags, actorRequired bool) {
	flags.StringVar(&common.root, "root", "", "registry root directory")
	flags.StringVar(&common.keyID, "key-id", "", "trusted Ed25519 key identifier")
	flags.StringVar(&common.privateKeyPath, "private-key", "", "base64 Ed25519 private-key file for mutations")
	flags.StringVar(&common.publicKeyPath, "public-key", "", "base64 Ed25519 public-key file for read-only inspection")
	if actorRequired {
		flags.StringVar(&common.actor, "actor", "", "authenticated operator or service identity")
		flags.StringVar(&common.reason, "reason", "", "auditable reason")
	}
}

func runHealth(args []string, stderr io.Writer) (HealthState, error) {
	flags := flag.NewFlagSet("health", flag.ContinueOnError)
	flags.SetOutput(stderr)
	var common commonCLIFlags
	addCommonFlags(flags, &common, false)
	if err := flags.Parse(args); err != nil {
		return HealthState{}, err
	}
	registry, err := openCLIRegistry(common, false)
	if err != nil {
		return HealthState{}, err
	}
	return registry.Health(), nil
}

func runRegister(ctx context.Context, args []string, stdin io.Reader, stderr io.Writer) (ModelStatus, error) {
	flags := flag.NewFlagSet("register", flag.ContinueOnError)
	flags.SetOutput(stderr)
	var common commonCLIFlags
	addCommonFlags(flags, &common, true)
	manifestPath := flags.String("manifest", "", "unsigned artifact manifest JSON")
	artifactPath := flags.String("artifact", "", "model artifact path")
	if err := flags.Parse(args); err != nil {
		return ModelStatus{}, err
	}
	registry, privateKey, err := openMutableCLIRegistry(common)
	if err != nil {
		return ModelStatus{}, err
	}
	var manifest ArtifactManifest
	if err := readJSONPath(*manifestPath, stdin, &manifest); err != nil {
		return ModelStatus{}, fmt.Errorf("read manifest: %w", err)
	}
	signed, err := SignManifest(manifest, common.keyID, privateKey)
	if err != nil {
		return ModelStatus{}, err
	}
	return registry.Register(ctx, RegisterRequest{SignedManifest: signed, ArtifactPath: *artifactPath, Actor: common.actor, Reason: common.reason})
}

func runValidate(ctx context.Context, args []string, stdin io.Reader, stderr io.Writer) (ModelStatus, error) {
	flags := flag.NewFlagSet("validate", flag.ContinueOnError)
	flags.SetOutput(stderr)
	var common commonCLIFlags
	addCommonFlags(flags, &common, true)
	modelID := flags.String("model-id", "", "model identifier")
	version := flags.String("version", "", "semantic version")
	stage := flags.String("stage", "", "trained, offline, or replay")
	schemaPath := flags.String("feature-schema", "", "runtime feature-schema JSON")
	if err := flags.Parse(args); err != nil {
		return ModelStatus{}, err
	}
	registry, _, err := openMutableCLIRegistry(common)
	if err != nil {
		return ModelStatus{}, err
	}
	schema, err := readFeatureSchema(*schemaPath, stdin)
	if err != nil {
		return ModelStatus{}, err
	}
	return registry.Validate(ctx, ValidateRequest{ModelID: *modelID, SemanticVersion: *version, Stage: ValidationStage(*stage), ExpectedFeatureSchema: schema, Actor: common.actor, Reason: common.reason})
}

func runApprove(ctx context.Context, args []string, stdin io.Reader, stderr io.Writer) (ModelStatus, error) {
	flags := flag.NewFlagSet("approve", flag.ContinueOnError)
	flags.SetOutput(stderr)
	var common commonCLIFlags
	addCommonFlags(flags, &common, true)
	modelID := flags.String("model-id", "", "model identifier")
	version := flags.String("version", "", "semantic version")
	schemaPath := flags.String("feature-schema", "", "runtime feature-schema JSON")
	reference := flags.String("approval-reference", "", "external reviewed approval record")
	if err := flags.Parse(args); err != nil {
		return ModelStatus{}, err
	}
	registry, _, err := openMutableCLIRegistry(common)
	if err != nil {
		return ModelStatus{}, err
	}
	schema, err := readFeatureSchema(*schemaPath, stdin)
	if err != nil {
		return ModelStatus{}, err
	}
	return registry.Approve(ctx, ApproveRequest{ModelID: *modelID, SemanticVersion: *version, ExpectedFeatureSchema: schema, Actor: common.actor, Reason: common.reason, ApprovalReference: *reference})
}

func runDeploy(ctx context.Context, args []string, stdin io.Reader, stderr io.Writer, canary bool) (ModelStatus, error) {
	name := "deploy-shadow"
	if canary {
		name = "promote-canary"
	}
	flags := flag.NewFlagSet(name, flag.ContinueOnError)
	flags.SetOutput(stderr)
	var common commonCLIFlags
	addCommonFlags(flags, &common, true)
	modelID := flags.String("model-id", "", "model identifier")
	version := flags.String("version", "", "semantic version")
	schemaPath := flags.String("feature-schema", "", "runtime feature-schema JSON")
	environment := flags.String("environment", name, "deployment environment")
	reference := flags.String("authorization-reference", "", "external canary approval record")
	if err := flags.Parse(args); err != nil {
		return ModelStatus{}, err
	}
	registry, _, err := openMutableCLIRegistry(common)
	if err != nil {
		return ModelStatus{}, err
	}
	schema, err := readFeatureSchema(*schemaPath, stdin)
	if err != nil {
		return ModelStatus{}, err
	}
	request := DeploymentRequest{ModelID: *modelID, SemanticVersion: *version, ExpectedFeatureSchema: schema, Environment: *environment, Actor: common.actor, Reason: common.reason}
	if canary {
		request.AuthorizationReference = *reference
		return registry.PromoteCanary(ctx, request)
	}
	return registry.DeployShadow(ctx, request)
}

func runPromote(ctx context.Context, args []string, stdin io.Reader, stderr io.Writer) (ModelStatus, error) {
	flags := flag.NewFlagSet("promote", flag.ContinueOnError)
	flags.SetOutput(stderr)
	var common commonCLIFlags
	addCommonFlags(flags, &common, true)
	modelID := flags.String("model-id", "", "model identifier")
	version := flags.String("version", "", "semantic version")
	schemaPath := flags.String("feature-schema", "", "runtime feature-schema JSON")
	target := flags.String("target", "", "LIMITED_RISK or PRODUCTION")
	environment := flags.String("environment", "", "deployment environment")
	explicitProduction := flags.Bool("explicit-production", false, "confirm explicit full-production request")
	reference := flags.String("authorization-reference", "", "external approved change record")
	if err := flags.Parse(args); err != nil {
		return ModelStatus{}, err
	}
	registry, _, err := openMutableCLIRegistry(common)
	if err != nil {
		return ModelStatus{}, err
	}
	schema, err := readFeatureSchema(*schemaPath, stdin)
	if err != nil {
		return ModelStatus{}, err
	}
	return registry.Promote(ctx, PromoteRequest{
		ModelID: *modelID, SemanticVersion: *version, ExpectedFeatureSchema: schema,
		Target: LifecycleState(strings.ToUpper(*target)), Environment: *environment,
		Actor: common.actor, Reason: common.reason,
		ExplicitProductionAuthorization: *explicitProduction, AuthorizationReference: *reference,
	})
}

func runRollback(ctx context.Context, args []string, stdin io.Reader, stderr io.Writer) (ModelStatus, error) {
	flags := flag.NewFlagSet("rollback", flag.ContinueOnError)
	flags.SetOutput(stderr)
	var common commonCLIFlags
	addCommonFlags(flags, &common, true)
	modelID := flags.String("model-id", "", "model identifier")
	version := flags.String("version", "", "current semantic version")
	targetVersion := flags.String("to-version", "", "previous approved semantic version")
	schemaPath := flags.String("feature-schema", "", "runtime feature-schema JSON")
	environment := flags.String("environment", "", "deployment environment")
	reference := flags.String("authorization-reference", "", "external approved rollback record")
	if err := flags.Parse(args); err != nil {
		return ModelStatus{}, err
	}
	registry, _, err := openMutableCLIRegistry(common)
	if err != nil {
		return ModelStatus{}, err
	}
	schema, err := readFeatureSchema(*schemaPath, stdin)
	if err != nil {
		return ModelStatus{}, err
	}
	return registry.Rollback(ctx, RollbackRequest{
		ModelID: *modelID, SemanticVersion: *version, TargetSemanticVersion: *targetVersion,
		ExpectedFeatureSchema: schema, Environment: *environment,
		Actor: common.actor, Reason: common.reason, AuthorizationReference: *reference,
	})
}

func runDisable(ctx context.Context, args []string, stderr io.Writer) (ModelStatus, error) {
	flags := flag.NewFlagSet("disable", flag.ContinueOnError)
	flags.SetOutput(stderr)
	var common commonCLIFlags
	addCommonFlags(flags, &common, true)
	modelID := flags.String("model-id", "", "model identifier")
	version := flags.String("version", "", "semantic version")
	if err := flags.Parse(args); err != nil {
		return ModelStatus{}, err
	}
	registry, _, err := openMutableCLIRegistry(common)
	if err != nil {
		return ModelStatus{}, err
	}
	return registry.Disable(ctx, DisableRequest{ModelID: *modelID, SemanticVersion: *version, Actor: common.actor, Reason: common.reason})
}

func runInspect(ctx context.Context, args []string, stderr io.Writer) (Lineage, error) {
	flags := flag.NewFlagSet("inspect-lineage", flag.ContinueOnError)
	flags.SetOutput(stderr)
	var common commonCLIFlags
	addCommonFlags(flags, &common, false)
	modelID := flags.String("model-id", "", "model identifier")
	version := flags.String("version", "", "semantic version")
	if err := flags.Parse(args); err != nil {
		return Lineage{}, err
	}
	registry, err := openCLIRegistry(common, false)
	if err != nil {
		return Lineage{}, err
	}
	return registry.InspectLineage(ctx, *modelID, *version)
}

func openMutableCLIRegistry(common commonCLIFlags) (*Registry, ed25519.PrivateKey, error) {
	registry, err := openCLIRegistry(common, true)
	if err != nil {
		return nil, nil, err
	}
	privateKey, err := loadPrivateKey(common.privateKeyPath)
	if err != nil {
		return nil, nil, err
	}
	return registry, privateKey, nil
}

func openCLIRegistry(common commonCLIFlags, requirePrivate bool) (*Registry, error) {
	if strings.TrimSpace(common.root) == "" || !validIdentifier(common.keyID) {
		return nil, errors.New("--root and valid --key-id are required")
	}
	config := Config{Root: common.root, SignerKeyID: common.keyID, TrustedKeys: make(map[string]ed25519.PublicKey)}
	if common.privateKeyPath != "" {
		privateKey, err := loadPrivateKey(common.privateKeyPath)
		if err != nil {
			return nil, err
		}
		config.Signer = privateKey
	} else if requirePrivate {
		return nil, errors.New("--private-key is required for mutations")
	}
	if common.publicKeyPath != "" {
		publicKey, err := loadPublicKey(common.publicKeyPath)
		if err != nil {
			return nil, err
		}
		config.TrustedKeys[common.keyID] = publicKey
	}
	if len(config.Signer) == 0 && len(config.TrustedKeys) == 0 {
		return nil, errors.New("--public-key or --private-key is required")
	}
	return Open(config)
}

func loadPrivateKey(path string) (ed25519.PrivateKey, error) {
	info, err := os.Stat(path)
	if err != nil {
		return nil, fmt.Errorf("stat private key: %w", err)
	}
	if !info.Mode().IsRegular() || info.Mode().Perm()&0o077 != 0 {
		return nil, errors.New("private-key file must be regular and owner-only")
	}
	payload, err := readBoundedFile(path, 8*1024)
	if err != nil {
		return nil, fmt.Errorf("read private key: %w", err)
	}
	decoded, err := base64.StdEncoding.Strict().DecodeString(strings.TrimSpace(string(payload)))
	if err != nil || len(decoded) != ed25519.PrivateKeySize {
		return nil, errors.New("private-key file must contain one base64 Ed25519 private key")
	}
	return ed25519.PrivateKey(decoded), nil
}

func loadPublicKey(path string) (ed25519.PublicKey, error) {
	payload, err := readBoundedFile(path, 8*1024)
	if err != nil {
		return nil, fmt.Errorf("read public key: %w", err)
	}
	decoded, err := base64.StdEncoding.Strict().DecodeString(strings.TrimSpace(string(payload)))
	if err != nil || len(decoded) != ed25519.PublicKeySize {
		return nil, errors.New("public-key file must contain one base64 Ed25519 public key")
	}
	return ed25519.PublicKey(decoded), nil
}

func readFeatureSchema(path string, stdin io.Reader) (FeatureSchema, error) {
	var schema FeatureSchema
	if err := readJSONPath(path, stdin, &schema); err != nil {
		return FeatureSchema{}, fmt.Errorf("read feature schema: %w", err)
	}
	return schema, nil
}

func readJSONPath(path string, stdin io.Reader, destination any) error {
	if strings.TrimSpace(path) == "" {
		return errors.New("JSON path is required")
	}
	var payload []byte
	var err error
	if path == "-" {
		payload, err = io.ReadAll(io.LimitReader(stdin, 4*1024*1024+1))
		if len(payload) > 4*1024*1024 {
			return errors.New("JSON input exceeds 4 MiB")
		}
	} else {
		payload, err = readBoundedFile(path, 4*1024*1024)
	}
	if err != nil {
		return err
	}
	return decodeStrict(payload, destination)
}

func readBoundedFile(path string, maximumBytes int64) ([]byte, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	payload, err := io.ReadAll(io.LimitReader(file, maximumBytes+1))
	if err != nil {
		return nil, err
	}
	if int64(len(payload)) > maximumBytes {
		return nil, fmt.Errorf("input exceeds %d bytes", maximumBytes)
	}
	return payload, nil
}

func writeCLIError(writer io.Writer, err error) {
	_, _ = fmt.Fprintf(writer, "model-registry: %v\n", err)
}

func writeCLIUsage(writer io.Writer) {
	_, _ = fmt.Fprintln(writer, "usage: model-registry <version|health|register|validate|approve|deploy-shadow|promote-canary|promote|rollback|disable|inspect-lineage> [flags]")
}
