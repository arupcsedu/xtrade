// Package configservice contains the non-networked control-plane foundation.
package configservice

const (
	// Version is the immutable foundation package version.
	Version = "0.2.0"
	// DefaultTestSeed is shared by deterministic control-plane tests.
	DefaultTestSeed int64 = 20260902
)

// BuildInfo identifies this control-plane foundation build.
type BuildInfo struct {
	Project            string
	Version            string
	TestSeed           int64
	LiveTradingCapable bool
}

// CurrentBuildInfo returns deterministic metadata without host or wall-clock data.
func CurrentBuildInfo() BuildInfo {
	return BuildInfo{
		Project:            "Aegis-MX",
		Version:            Version,
		TestSeed:           DefaultTestSeed,
		LiveTradingCapable: false,
	}
}
