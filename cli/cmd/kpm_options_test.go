package cmd

import (
	"errors"
	"testing"
	"time"

	"stackplz/user/config"
	"stackplz/user/kpm"
)

func TestKPMOptionsDefaults(t *testing.T) {
	got := config.NewGlobalConfig()
	if got.TaskSource != "proc" || got.BrkBackend != "perf" || got.BrkMode != "once" {
		t.Fatalf("unsafe compatibility defaults: %#v", got)
	}
	profile := kpm.DefaultDeviceProfile()
	if got.KPMProfile != profile.ID {
		t.Fatalf("profile=%q", got.KPMProfile)
	}
	if got.KPMControl != profile.KPatch.ControlPath || got.KPMModule != profile.KPatch.ModuleName {
		t.Fatalf("control=%q module=%q", got.KPMControl, got.KPMModule)
	}
	if got.KPMBindTimeout != 10*time.Second {
		t.Fatalf("timeout=%s", got.KPMBindTimeout)
	}
}

func TestKPMOptionsAreRegistered(t *testing.T) {
	profile := kpm.DefaultDeviceProfile()
	want := map[string]string{
		"task-source":      "proc",
		"brk-backend":      "perf",
		"kpm-profile":      profile.ID,
		"kpm-control":      profile.KPatch.ControlPath,
		"kpm-module":       profile.KPatch.ModuleName,
		"maps-file":        "",
		"brk-base":         "0",
		"brk-mode":         "once",
		"kpm-bind-timeout": "10s",
	}
	for name, defaultValue := range want {
		flag := rootCmd.PersistentFlags().Lookup(name)
		if flag == nil || flag.DefValue != defaultValue {
			t.Fatalf("flag %s=%v", name, flag)
		}
	}
}

func TestSelectBreakBackendIsConservative(t *testing.T) {
	probeErr := errors.New("KPM unavailable")
	tests := []struct {
		name      string
		requested BreakBackend
		probeErr  error
		want      BreakBackend
		wantErr   error
		calls     int
	}{
		{name: "explicit perf never probes", requested: BreakBackendPerf, want: BreakBackendPerf},
		{name: "explicit KPM ready", requested: BreakBackendKPMDirect, want: BreakBackendKPMDirect, calls: 1},
		{name: "explicit KPM never falls back", requested: BreakBackendKPMDirect, probeErr: probeErr, wantErr: probeErr, calls: 1},
		{name: "auto selects KPM", requested: BreakBackendAuto, want: BreakBackendKPMDirect, calls: 1},
		{name: "auto falls back to perf", requested: BreakBackendAuto, probeErr: probeErr, want: BreakBackendPerf, calls: 1},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			calls := 0
			got, err := selectBreakBackend(test.requested, func() error {
				calls++
				return test.probeErr
			})
			if !errors.Is(err, test.wantErr) || got != test.want || calls != test.calls {
				t.Fatalf("backend=%q err=%v calls=%d", got, err, calls)
			}
		})
	}
}

func TestParseBreakBackendRejectsUnknown(t *testing.T) {
	for _, value := range []BreakBackend{BreakBackendPerf, BreakBackendKPMDirect, BreakBackendAuto} {
		got, err := ParseBreakBackend(string(value))
		if err != nil || got != value {
			t.Fatalf("ParseBreakBackend(%q)=(%q,%v)", value, got, err)
		}
	}
	if _, err := ParseBreakBackend("direct"); err == nil {
		t.Fatal("unknown backend accepted")
	}
}
