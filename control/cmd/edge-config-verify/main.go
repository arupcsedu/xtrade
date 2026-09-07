package main

import (
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"os"
	"time"

	configservice "github.com/aegis-mx/aegis-mx/control/config_service"
)

const maximumInputBytes = int64(4 * 1024 * 1024)

type profile struct {
	SchemaVersion       uint32 `json:"profile_schema_version"`
	Name                string `json:"name"`
	TradingMode         string `json:"trading_mode"`
	LiveEnabled         bool   `json:"live_transmission_enabled"`
	AutomaticActivation bool   `json:"automatic_activation"`
}

type manifest struct {
	SchemaVersion       uint32            `json:"schema_version"`
	Profile             string            `json:"profile"`
	ProfileSHA256       string            `json:"profile_sha256"`
	TradingMode         string            `json:"trading_mode"`
	LiveEnabled         bool              `json:"live_transmission_enabled"`
	AutomaticActivation bool              `json:"automatic_activation"`
	Files               map[string]string `json:"files"`
	ManifestSHA256      string            `json:"manifest_sha256"`
}

type trustedKey struct {
	KeyID           string `json:"key_id"`
	PublicKeyBase64 string `json:"public_key_base64"`
}

type trustDocument struct {
	SchemaVersion uint32       `json:"schema_version"`
	Keys          []trustedKey `json:"keys"`
}

func readBounded(path string) ([]byte, error) {
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	payload, err := io.ReadAll(io.LimitReader(file, maximumInputBytes+1))
	if err != nil || len(payload) == 0 || int64(len(payload)) > maximumInputBytes {
		return nil, errors.New("input is absent or exceeds its bound")
	}
	return payload, nil
}

func decodeStrict(payload []byte, destination any) error {
	decoder := json.NewDecoder(bytes.NewReader(payload))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(destination); err != nil {
		return err
	}
	if err := decoder.Decode(&struct{}{}); !errors.Is(err, io.EOF) {
		return errors.New("input must contain exactly one JSON object")
	}
	return nil
}

func digest(payload []byte) string {
	value := sha256.Sum256(payload)
	return hex.EncodeToString(value[:])
}

func loadTrust(path string) (map[string]ed25519.PublicKey, error) {
	payload, err := readBounded(path)
	if err != nil {
		return nil, err
	}
	var document trustDocument
	if err := decodeStrict(payload, &document); err != nil || document.SchemaVersion != 1 || len(document.Keys) == 0 {
		return nil, errors.New("trust root is malformed")
	}
	result := make(map[string]ed25519.PublicKey, len(document.Keys))
	for _, row := range document.Keys {
		decoded, decodeErr := base64.StdEncoding.Strict().DecodeString(row.PublicKeyBase64)
		if decodeErr != nil || len(decoded) != ed25519.PublicKeySize || row.KeyID == "" {
			return nil, errors.New("trust root contains an invalid key")
		}
		if _, duplicate := result[row.KeyID]; duplicate {
			return nil, errors.New("trust root contains a duplicate key identifier")
		}
		result[row.KeyID] = ed25519.PublicKey(decoded)
	}
	return result, nil
}

func canonicalManifestHash(value manifest) (string, error) {
	claimed := value.ManifestSHA256
	unsigned := map[string]any{
		"automatic_activation":      value.AutomaticActivation,
		"files":                     value.Files,
		"live_transmission_enabled": value.LiveEnabled,
		"profile":                   value.Profile,
		"profile_sha256":            value.ProfileSHA256,
		"schema_version":            value.SchemaVersion,
		"trading_mode":              value.TradingMode,
	}
	payload, err := json.Marshal(unsigned)
	if err != nil {
		return "", err
	}
	if digest(payload) != claimed {
		return "", errors.New("deployment manifest digest mismatch")
	}
	return claimed, nil
}

func run(args []string) error {
	flags := flag.NewFlagSet("aegis-edge-config-verify", flag.ContinueOnError)
	profilePath := flags.String("profile", "", "active deployment profile")
	manifestPath := flags.String("manifest", "", "rendered deployment manifest")
	configurationPath := flags.String("configuration", "", "signed runtime configuration")
	defaultTrust := os.Getenv("AEGIS_CONFIGURATION_TRUST_ROOT")
	if defaultTrust == "" {
		defaultTrust = "/etc/aegis-mx/trust/configuration-signers.json"
	}
	trustPath := flags.String("trusted-keys", defaultTrust, "trusted Ed25519 public keys")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *profilePath == "" || *manifestPath == "" || *configurationPath == "" || flags.NArg() != 0 {
		return errors.New("profile, manifest, and configuration are required")
	}
	profileBytes, err := readBounded(*profilePath)
	if err != nil {
		return err
	}
	manifestBytes, err := readBounded(*manifestPath)
	if err != nil {
		return err
	}
	var selected profile
	if err := json.Unmarshal(profileBytes, &selected); err != nil {
		return err
	}
	var deployment manifest
	if err := decodeStrict(manifestBytes, &deployment); err != nil {
		return err
	}
	if _, err := canonicalManifestHash(deployment); err != nil {
		return err
	}
	if selected.SchemaVersion != 1 || selected.LiveEnabled || selected.AutomaticActivation ||
		(selected.TradingMode != "SIMULATION" && selected.TradingMode != "PAPER") ||
		deployment.SchemaVersion != 1 || deployment.LiveEnabled || deployment.AutomaticActivation ||
		deployment.Profile != selected.Name || deployment.TradingMode != selected.TradingMode ||
		deployment.ProfileSHA256 != digest(profileBytes) ||
		deployment.Files["deployment/active-profile.json"] != digest(profileBytes) {
		return errors.New("deployment profile and manifest are not the same non-live release")
	}
	configurationBytes, err := readBounded(*configurationPath)
	if err != nil {
		return err
	}
	signed, err := configservice.DecodeSignedConfiguration(bytes.NewReader(configurationBytes))
	if err != nil {
		return err
	}
	trusted, err := loadTrust(*trustPath)
	if err != nil {
		return err
	}
	if err := configservice.VerifySignedConfiguration(signed, trusted); err != nil {
		return err
	}
	now := time.Now().UnixNano()
	if now < signed.Configuration.ValidFromWallUTCNS || now >= signed.Configuration.ValidUntilWallUTCNS ||
		signed.Configuration.Environment != selected.Name {
		return configservice.ErrConfigurationStale
	}
	for _, venue := range signed.Configuration.Venues {
		if venue.Mode == configservice.TradingModeLive ||
			(selected.TradingMode == "SIMULATION" && venue.Mode != configservice.TradingModeSimulation) {
			return configservice.ErrLiveModeUnavailable
		}
	}
	fmt.Printf("{\"configuration_sha256\":%q,\"mode\":%q,\"profile\":%q,\"verified\":true}\n",
		signed.Configuration.SHA256, selected.TradingMode, selected.Name)
	return nil
}

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintf(os.Stderr, "aegis-edge-config-verify: %v\n", err)
		os.Exit(1)
	}
}
