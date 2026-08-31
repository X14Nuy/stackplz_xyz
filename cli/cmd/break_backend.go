package cmd

import "fmt"

type BreakBackend string

const (
	BreakBackendPerf      BreakBackend = "perf"
	BreakBackendKPMDirect BreakBackend = "kpm-direct"
	BreakBackendAuto      BreakBackend = "auto"
)

func ParseBreakBackend(value string) (BreakBackend, error) {
	backend := BreakBackend(value)
	switch backend {
	case BreakBackendPerf, BreakBackendKPMDirect, BreakBackendAuto:
		return backend, nil
	default:
		return "", fmt.Errorf("invalid breakpoint backend %q (want perf, kpm-direct, or auto)", value)
	}
}

func selectBreakBackend(requested BreakBackend, kpmReady func() error) (BreakBackend, error) {
	if _, err := ParseBreakBackend(string(requested)); err != nil {
		return "", err
	}
	switch requested {
	case BreakBackendPerf:
		return BreakBackendPerf, nil
	case BreakBackendKPMDirect:
		if kpmReady == nil {
			return "", fmt.Errorf("KPM readiness check is unavailable")
		}
		if err := kpmReady(); err != nil {
			return "", fmt.Errorf("explicit kpm-direct backend is unavailable: %w", err)
		}
		return BreakBackendKPMDirect, nil
	case BreakBackendAuto:
		if kpmReady != nil && kpmReady() == nil {
			return BreakBackendKPMDirect, nil
		}
		return BreakBackendPerf, nil
	default:
		panic("unreachable breakpoint backend")
	}
}
