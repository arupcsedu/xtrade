package configservice

import (
	"bytes"
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
)

func canonicalSHA256(value any) (string, error) {
	payload, err := json.Marshal(value)
	if err != nil {
		return "", err
	}
	digest := sha256.Sum256(payload)
	return hex.EncodeToString(digest[:]), nil
}

// FinalizeConfiguration canonicalizes set-like fields and calculates the
// immutable content digest.
func FinalizeConfiguration(configuration Configuration) (Configuration, error) {
	configuration.SHA256 = ""
	configuration = canonicalizeConfiguration(configuration)
	for _, venue := range configuration.Venues {
		if venue.Mode == TradingModeLive {
			return Configuration{}, ErrLiveModeUnavailable
		}
	}
	if failures := validateConfigurationFields(configuration); len(failures) != 0 {
		return Configuration{}, fmt.Errorf("%w: %s", ErrConfigurationInvalid, failures[0])
	}
	digest, err := canonicalSHA256(configuration)
	if err != nil {
		return Configuration{}, fmt.Errorf("hash configuration: %w", err)
	}
	configuration.SHA256 = digest
	return configuration, nil
}

func ConfigurationVersion(configuration Configuration) string {
	if !hex64Pattern.MatchString(configuration.SHA256) {
		return ""
	}
	return configuration.SHA256[:32]
}

func SignConfiguration(configuration Configuration, keyID string, privateKey ed25519.PrivateKey) (SignedConfiguration, error) {
	if err := validateFinalConfiguration(configuration); err != nil {
		return SignedConfiguration{}, err
	}
	if !validIdentifier(keyID) || len(privateKey) != ed25519.PrivateKeySize {
		return SignedConfiguration{}, fmt.Errorf("%w: invalid signer", ErrSignature)
	}
	payload, err := json.Marshal(configuration)
	if err != nil {
		return SignedConfiguration{}, err
	}
	digest := sha256.Sum256(payload)
	return SignedConfiguration{
		Configuration: configuration, SignerKeyID: keyID,
		SignatureBase64: base64.StdEncoding.EncodeToString(ed25519.Sign(privateKey, digest[:])),
	}, nil
}

func VerifySignedConfiguration(signed SignedConfiguration, trustedKeys map[string]ed25519.PublicKey) error {
	if err := validateFinalConfiguration(signed.Configuration); err != nil {
		return err
	}
	return verifySignature(signed.Configuration, signed.SignerKeyID, signed.SignatureBase64, trustedKeys)
}

// DecodeSignedConfiguration accepts exactly one bounded JSON object and rejects
// unknown fields before any signature decision is made.
func DecodeSignedConfiguration(reader io.Reader) (SignedConfiguration, error) {
	var signed SignedConfiguration
	limited := io.LimitReader(reader, maximumConfigurationBytes+1)
	payload, err := io.ReadAll(limited)
	if err != nil {
		return SignedConfiguration{}, err
	}
	if len(payload) == 0 || int64(len(payload)) > maximumConfigurationBytes {
		return SignedConfiguration{}, ErrConfigurationInvalid
	}
	if err := decodeStrict(payload, &signed); err != nil {
		return SignedConfiguration{}, fmt.Errorf("%w: %v", ErrConfigurationInvalid, err)
	}
	return signed, nil
}

func signKillCommand(command KillCommand, keyID string, privateKey ed25519.PrivateKey) (SignedKillCommand, error) {
	if err := validateKillCommand(command); err != nil {
		return SignedKillCommand{}, err
	}
	if !validIdentifier(keyID) || len(privateKey) != ed25519.PrivateKeySize {
		return SignedKillCommand{}, fmt.Errorf("%w: invalid kill signer", ErrSignature)
	}
	payload, err := json.Marshal(command)
	if err != nil {
		return SignedKillCommand{}, err
	}
	digest := sha256.Sum256(payload)
	return SignedKillCommand{
		Command: command, SignerKeyID: keyID,
		SignatureBase64: base64.StdEncoding.EncodeToString(ed25519.Sign(privateKey, digest[:])),
	}, nil
}

func VerifySignedKillCommand(signed SignedKillCommand, trustedKeys map[string]ed25519.PublicKey) error {
	if err := validateKillCommand(signed.Command); err != nil {
		return err
	}
	return verifySignature(signed.Command, signed.SignerKeyID, signed.SignatureBase64, trustedKeys)
}

func verifySignature(value any, keyID string, encoded string, trustedKeys map[string]ed25519.PublicKey) error {
	publicKey, ok := trustedKeys[keyID]
	if !ok || len(publicKey) != ed25519.PublicKeySize {
		return fmt.Errorf("%w: untrusted signer %q", ErrSignature, keyID)
	}
	signature, err := base64.StdEncoding.Strict().DecodeString(encoded)
	if err != nil || len(signature) != ed25519.SignatureSize {
		return fmt.Errorf("%w: malformed signature", ErrSignature)
	}
	payload, err := json.Marshal(value)
	if err != nil {
		return err
	}
	digest := sha256.Sum256(payload)
	if !ed25519.Verify(publicKey, digest[:], signature) {
		return fmt.Errorf("%w: signature mismatch", ErrSignature)
	}
	return nil
}

func validateFinalConfiguration(configuration Configuration) error {
	if !hex64Pattern.MatchString(configuration.SHA256) {
		return fmt.Errorf("%w: malformed digest", ErrHashMismatch)
	}
	declared := configuration.SHA256
	canonical := configuration
	canonical.SHA256 = ""
	canonical = canonicalizeConfiguration(canonical)
	for _, venue := range canonical.Venues {
		if venue.Mode == TradingModeLive {
			return ErrLiveModeUnavailable
		}
	}
	if failures := validateConfigurationFields(canonical); len(failures) != 0 {
		return fmt.Errorf("%w: %s", ErrConfigurationInvalid, failures[0])
	}
	if !configurationOrderingEqual(configuration, canonical) {
		return fmt.Errorf("%w: noncanonical field ordering", ErrConfigurationInvalid)
	}
	digest, err := canonicalSHA256(canonical)
	if err != nil {
		return err
	}
	if digest != declared {
		return ErrHashMismatch
	}
	return nil
}

func decodeStrict(payload []byte, destination any) error {
	decoder := json.NewDecoder(bytes.NewReader(payload))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(destination); err != nil {
		return err
	}
	if err := decoder.Decode(&struct{}{}); !errors.Is(err, io.EOF) {
		if err == nil {
			return errors.New("multiple JSON values are not allowed")
		}
		return err
	}
	return nil
}
