package modelregistry

import (
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"errors"
	"fmt"
)

// FinalizeDeploymentConfig canonicalizes every set-like field and calculates
// the deterministic configuration digest.
func FinalizeDeploymentConfig(config DeploymentConfig) (DeploymentConfig, error) {
	config.ConfigurationSHA256 = ""
	config = canonicalizeDeploymentConfig(config)
	if err := validateDeploymentConfigFields(config); err != nil {
		return DeploymentConfig{}, err
	}
	digest, err := canonicalSHA256(config)
	if err != nil {
		return DeploymentConfig{}, err
	}
	config.ConfigurationSHA256 = digest
	return config, nil
}

// SignDeploymentConfig signs an already finalized deterministic config.
func SignDeploymentConfig(config DeploymentConfig, keyID string, privateKey ed25519.PrivateKey) (SignedDeploymentConfig, error) {
	if err := validateFinalDeploymentConfig(config); err != nil {
		return SignedDeploymentConfig{}, err
	}
	if !validIdentifier(keyID) || len(privateKey) != ed25519.PrivateKeySize {
		return SignedDeploymentConfig{}, fmt.Errorf("%w: invalid deployment signing key", ErrSignature)
	}
	payload, err := json.Marshal(config)
	if err != nil {
		return SignedDeploymentConfig{}, fmt.Errorf("marshal deployment config: %w", err)
	}
	digest := sha256.Sum256(payload)
	return SignedDeploymentConfig{
		Config:          config,
		SignerKeyID:     keyID,
		SignatureBase64: base64.StdEncoding.EncodeToString(ed25519.Sign(privateKey, digest[:])),
	}, nil
}

// VerifySignedDeploymentConfig rejects noncanonical, modified, or untrusted
// deployment configuration.
func VerifySignedDeploymentConfig(signed SignedDeploymentConfig, trustedKeys map[string]ed25519.PublicKey) error {
	if err := validateFinalDeploymentConfig(signed.Config); err != nil {
		return err
	}
	publicKey, ok := trustedKeys[signed.SignerKeyID]
	if !ok || len(publicKey) != ed25519.PublicKeySize {
		return fmt.Errorf("%w: untrusted deployment signer %q", ErrSignature, signed.SignerKeyID)
	}
	signature, err := base64.StdEncoding.Strict().DecodeString(signed.SignatureBase64)
	if err != nil || len(signature) != ed25519.SignatureSize {
		return fmt.Errorf("%w: malformed deployment signature", ErrSignature)
	}
	payload, err := json.Marshal(signed.Config)
	if err != nil {
		return fmt.Errorf("marshal deployment config: %w", err)
	}
	digest := sha256.Sum256(payload)
	if !ed25519.Verify(publicKey, digest[:], signature) {
		return fmt.Errorf("%w: deployment configuration signature mismatch", ErrSignature)
	}
	return nil
}

func validateFinalDeploymentConfig(config DeploymentConfig) error {
	if !hex64Pattern.MatchString(config.ConfigurationSHA256) {
		return errors.New("deployment configuration digest is invalid")
	}
	canonical := config
	declared := canonical.ConfigurationSHA256
	canonical.ConfigurationSHA256 = ""
	canonical = canonicalizeDeploymentConfig(canonical)
	if err := validateDeploymentConfigFields(canonical); err != nil {
		return err
	}
	if !deploymentConfigOrderingEqual(config, canonical) {
		return errors.New("deployment configuration is not in canonical order")
	}
	digest, err := canonicalSHA256(canonical)
	if err != nil {
		return err
	}
	if digest != declared {
		return errors.New("deployment configuration digest mismatch")
	}
	return nil
}

func deploymentConfigOrderingEqual(left DeploymentConfig, right DeploymentConfig) bool {
	if len(left.RequiredRegimes) != len(right.RequiredRegimes) || len(left.RollbackThresholds) != len(right.RollbackThresholds) || len(left.AllowedInstrumentIDs) != len(right.AllowedInstrumentIDs) || len(left.AllowedStrategyIDs) != len(right.AllowedStrategyIDs) {
		return false
	}
	for index := range left.RequiredRegimes {
		if left.RequiredRegimes[index] != right.RequiredRegimes[index] {
			return false
		}
	}
	for index := range left.RollbackThresholds {
		if left.RollbackThresholds[index] != right.RollbackThresholds[index] {
			return false
		}
	}
	for index := range left.AllowedInstrumentIDs {
		if left.AllowedInstrumentIDs[index] != right.AllowedInstrumentIDs[index] {
			return false
		}
	}
	for index := range left.AllowedStrategyIDs {
		if left.AllowedStrategyIDs[index] != right.AllowedStrategyIDs[index] {
			return false
		}
	}
	return true
}
