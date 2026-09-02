package modelregistry

import (
	"crypto/ed25519"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"fmt"
)

// SignManifest creates a detached Ed25519 signature over canonical JSON.
func SignManifest(manifest ArtifactManifest, keyID string, privateKey ed25519.PrivateKey) (SignedManifest, error) {
	if err := validateManifest(manifest); err != nil {
		return SignedManifest{}, err
	}
	if !validIdentifier(keyID) || len(privateKey) != ed25519.PrivateKeySize {
		return SignedManifest{}, fmt.Errorf("%w: invalid signing key", ErrSignature)
	}
	payload, err := json.Marshal(manifest)
	if err != nil {
		return SignedManifest{}, fmt.Errorf("marshal manifest: %w", err)
	}
	digest := sha256.Sum256(payload)
	signature := ed25519.Sign(privateKey, digest[:])
	return SignedManifest{
		Manifest:        manifest,
		SignerKeyID:     keyID,
		SignatureBase64: base64.StdEncoding.EncodeToString(signature),
	}, nil
}

// VerifySignedManifest validates the schema, signer trust, and signature.
func VerifySignedManifest(signed SignedManifest, trustedKeys map[string]ed25519.PublicKey) error {
	if err := validateManifest(signed.Manifest); err != nil {
		return err
	}
	publicKey, ok := trustedKeys[signed.SignerKeyID]
	if !ok || len(publicKey) != ed25519.PublicKeySize {
		return fmt.Errorf("%w: untrusted manifest signer %q", ErrSignature, signed.SignerKeyID)
	}
	signature, err := base64.StdEncoding.Strict().DecodeString(signed.SignatureBase64)
	if err != nil || len(signature) != ed25519.SignatureSize {
		return fmt.Errorf("%w: malformed manifest signature", ErrSignature)
	}
	payload, err := json.Marshal(signed.Manifest)
	if err != nil {
		return fmt.Errorf("marshal manifest: %w", err)
	}
	digest := sha256.Sum256(payload)
	if !ed25519.Verify(publicKey, digest[:], signature) {
		return fmt.Errorf("%w: manifest signature mismatch", ErrSignature)
	}
	return nil
}

func signLifecycleEvent(body LifecycleEventBody, keyID string, privateKey ed25519.PrivateKey) (SignedLifecycleEvent, error) {
	digest, err := canonicalSHA256(body)
	if err != nil {
		return SignedLifecycleEvent{}, err
	}
	digestBytes, err := hex.DecodeString(digest)
	if err != nil {
		return SignedLifecycleEvent{}, fmt.Errorf("decode event digest: %w", err)
	}
	return SignedLifecycleEvent{
		Body:            body,
		EventSHA256:     digest,
		SignerKeyID:     keyID,
		SignatureBase64: base64.StdEncoding.EncodeToString(ed25519.Sign(privateKey, digestBytes)),
	}, nil
}

func verifyLifecycleEvent(event SignedLifecycleEvent, trustedKeys map[string]ed25519.PublicKey) error {
	digest, err := canonicalSHA256(event.Body)
	if err != nil {
		return err
	}
	if digest != event.EventSHA256 || !hex64Pattern.MatchString(event.EventSHA256) {
		return fmt.Errorf("%w: lifecycle event digest mismatch", ErrRegistryIntegrity)
	}
	return verifyDigestSignature(event.EventSHA256, event.SignerKeyID, event.SignatureBase64, trustedKeys, "lifecycle event")
}

func signDeploymentPointer(body DeploymentPointerBody, keyID string, privateKey ed25519.PrivateKey) (SignedDeploymentPointer, error) {
	digest, err := canonicalSHA256(body)
	if err != nil {
		return SignedDeploymentPointer{}, err
	}
	digestBytes, err := hex.DecodeString(digest)
	if err != nil {
		return SignedDeploymentPointer{}, fmt.Errorf("decode pointer digest: %w", err)
	}
	return SignedDeploymentPointer{
		Body:            body,
		PointerSHA256:   digest,
		SignerKeyID:     keyID,
		SignatureBase64: base64.StdEncoding.EncodeToString(ed25519.Sign(privateKey, digestBytes)),
	}, nil
}

func verifyDeploymentPointer(pointer SignedDeploymentPointer, trustedKeys map[string]ed25519.PublicKey) error {
	digest, err := canonicalSHA256(pointer.Body)
	if err != nil {
		return err
	}
	if digest != pointer.PointerSHA256 || !hex64Pattern.MatchString(pointer.PointerSHA256) {
		return fmt.Errorf("%w: deployment pointer digest mismatch", ErrRegistryIntegrity)
	}
	return verifyDigestSignature(pointer.PointerSHA256, pointer.SignerKeyID, pointer.SignatureBase64, trustedKeys, "deployment pointer")
}

func verifyDigestSignature(digest string, keyID string, signatureText string, trustedKeys map[string]ed25519.PublicKey, kind string) error {
	publicKey, ok := trustedKeys[keyID]
	if !ok || len(publicKey) != ed25519.PublicKeySize {
		return fmt.Errorf("%w: untrusted %s signer %q", ErrSignature, kind, keyID)
	}
	digestBytes, err := hex.DecodeString(digest)
	if err != nil {
		return fmt.Errorf("%w: malformed %s digest", ErrSignature, kind)
	}
	signature, err := base64.StdEncoding.Strict().DecodeString(signatureText)
	if err != nil || len(signature) != ed25519.SignatureSize || !ed25519.Verify(publicKey, digestBytes, signature) {
		return fmt.Errorf("%w: %s signature mismatch", ErrSignature, kind)
	}
	return nil
}
