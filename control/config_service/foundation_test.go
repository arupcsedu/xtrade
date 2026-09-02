package configservice

import "testing"

func TestCurrentBuildInfoIsDeterministicAndNonLive(t *testing.T) {
	t.Parallel()

	first := CurrentBuildInfo()
	second := CurrentBuildInfo()
	if first != second {
		t.Fatalf("build information changed between calls: first=%+v second=%+v", first, second)
	}
	if first.Project != "Aegis-MX" || first.Version != "0.2.0" {
		t.Fatalf("unexpected build identity: %+v", first)
	}
	if first.TestSeed != 20260902 {
		t.Fatalf("unexpected deterministic test seed: %d", first.TestSeed)
	}
	if first.LiveTradingCapable {
		t.Fatal("foundation package must not advertise live-trading capability")
	}
}
