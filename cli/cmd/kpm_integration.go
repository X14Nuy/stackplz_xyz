package cmd

import (
	"context"
	"errors"
	"fmt"
	"time"

	"stackplz/user/config"
	"stackplz/user/event"
	"stackplz/user/kpm"
	"stackplz/user/module"
	"stackplz/user/util"
)

func validateTracingOptions(options *config.GlobalConfig, pids []uint32) (TargetSource, BreakBackend, error) {
	if options == nil {
		return "", "", errors.New("global config is nil")
	}
	source, err := ParseTargetSource(options.TaskSource)
	if err != nil {
		return "", "", err
	}
	backend, err := ParseBreakBackend(options.BrkBackend)
	if err != nil {
		return "", "", err
	}
	breakpointRequested := options.BrkAddr != ""
	if breakpointRequested && (options.BrkLen == 0 || options.BrkLen > 8) {
		return "", "", fmt.Errorf("BrkLen %d invalid, support [1, 8]", options.BrkLen)
	}
	if options.BrkMode != string(kpm.BreakOnce) && options.BrkMode != string(kpm.BreakRepeat) {
		return "", "", fmt.Errorf("invalid --brk-mode %q (want once or repeat)", options.BrkMode)
	}
	usesKPM := source != TargetSourceProc || backend != BreakBackendPerf
	if usesKPM {
		if _, found := kpm.FindDeviceProfile(options.KPMProfile); !found {
			return "", "", fmt.Errorf("unknown KPM profile %q", options.KPMProfile)
		}
		if options.KPMBindTimeout <= 0 {
			return "", "", errors.New("--kpm-bind-timeout must be positive")
		}
	}
	if source == TargetSourceKPM {
		if len(pids) == 0 {
			return "", "", errors.New("--task-source=kpm requires a numeric --pid")
		}
		if options.Name != "" {
			return "", "", errors.New("--task-source=kpm does not support --name; use a numeric --pid")
		}
	}
	if backend == BreakBackendKPMDirect {
		if !breakpointRequested {
			return "", "", errors.New("--brk-backend=kpm-direct requires --brk")
		}
		if options.Name != "" {
			return "", "", errors.New("--brk-backend=kpm-direct does not support --name; use exactly one numeric --pid")
		}
		if _, err := singleBreakpointPID(pids); err != nil {
			return "", "", err
		}
	}
	return source, backend, nil
}

func cacheKPMTargetMaps(ctx context.Context, client *kpm.Client,
	targets []ResolvedTarget, mapsFile string) map[uint32]error {
	errorsByPID := make(map[uint32]error)
	for _, target := range targets {
		if target.Source != TargetSourceKPM {
			continue
		}
		if mapsFile != "" {
			if err := event.LoadMapsFile(target.PID, mapsFile); err != nil {
				errorsByPID[target.PID] = err
			}
			continue
		}
		if client == nil {
			errorsByPID[target.PID] = errors.New("KPM maps client is unavailable")
			continue
		}
		content, err := client.SnapshotMaps(ctx)
		if err == nil {
			err = event.LoadMapsContent(target.PID, content)
		}
		if err != nil {
			errorsByPID[target.PID] = err
		}
	}
	return errorsByPID
}

func resolveRuntimePIDTargets(ctx context.Context, resolver TargetResolver, source TargetSource, pids []uint32, uidOverride *uint32, enrichProc func(ResolvedTarget) error) ([]ResolvedTarget, error) {
	targets, err := preparePIDTargets(ctx, resolver, source, pids, uidOverride)
	if err != nil {
		return nil, err
	}
	for _, target := range targets {
		if target.Source != TargetSourceProc || enrichProc == nil {
			continue
		}
		if err := enrichProc(target); err != nil {
			return nil, fmt.Errorf("enrich proc target pid %d: %w", target.PID, err)
		}
	}
	return targets, nil
}

func clearKPMPreparationBinding(ctx context.Context, client *kpm.Client, targets []ResolvedTarget) error {
	for _, target := range targets {
		if target.Source != TargetSourceKPM {
			continue
		}
		if client == nil {
			return errors.New("cannot clear preparation binding: KPM client is unavailable")
		}
		if err := client.Clear(ctx); err != nil {
			return fmt.Errorf("clear preparation-time KPM binding: %w", err)
		}
		return nil
	}
	return nil
}

func selectModuleNames(conf *config.ModuleConfig) ([]string, error) {
	if conf == nil {
		return nil, errors.New("module config is nil")
	}
	if conf.BrkAddr != 0 {
		backend, err := ParseBreakBackend(conf.BrkBackend)
		if err != nil {
			return nil, err
		}
		switch backend {
		case BreakBackendPerf:
			return []string{module.MODULE_NAME_BRK}, nil
		case BreakBackendKPMDirect:
			return []string{module.MODULE_NAME_KPM_BRK}, nil
		case BreakBackendAuto:
			return nil, errors.New("breakpoint backend auto was not resolved before module selection")
		}
	}
	if conf.SysCallConf != nil && conf.SysCallConf.Enable {
		return []string{module.MODULE_NAME_PERF, module.MODULE_NAME_SYSCALL}, nil
	}
	if conf.StackUprobeConf != nil && len(conf.StackUprobeConf.Points) > 0 {
		return []string{module.MODULE_NAME_PERF, module.MODULE_NAME_STACK}, nil
	}
	return nil, errors.New("hook nothing, set -w/--point, -s/--syscall, or --brk")
}

type liveTargetResolver struct {
	client       *kpm.Client
	uidOverride  *uint32
	bindTimeout  time.Duration
	pollInterval time.Duration
}

func (resolver *liveTargetResolver) UIDFromProc(pid uint32) (uint32, error) {
	return new(util.PackageInfos).FindUidByPid(pid)
}

func (resolver *liveTargetResolver) IdentityFromKPM(ctx context.Context, pid uint32) (kpm.Identity, error) {
	if resolver.client == nil {
		return kpm.Identity{}, errors.New("KPM target resolver is unavailable")
	}
	if _, err := resolver.client.Status(ctx); err != nil {
		return kpm.Identity{}, fmt.Errorf("KPM status: %w", err)
	}
	if _, err := resolver.client.Bind(ctx, kpm.BindingRequest{PID: pid, Mode: kpm.BindEither, UID: resolver.uidOverride}); err != nil {
		return kpm.Identity{}, resolver.clearFailedBinding(fmt.Errorf("KPM bind pid %d: %w", pid, err))
	}
	timeout := resolver.bindTimeout
	if timeout <= 0 {
		timeout = 10 * time.Second
	}
	waitCtx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	identity, err := resolver.client.WaitBound(waitCtx, resolver.pollInterval)
	if err != nil {
		return kpm.Identity{}, resolver.clearFailedBinding(fmt.Errorf("wait for KPM identity pid %d: %w", pid, err))
	}
	return identity, nil
}

func (resolver *liveTargetResolver) clearFailedBinding(primary error) error {
	timeout := resolver.bindTimeout
	if timeout <= 0 {
		timeout = 10 * time.Second
	}
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	if err := resolver.client.Clear(ctx); err != nil {
		return combineRunErrors(primary, fmt.Errorf("clear failed KPM binding: %w", err))
	}
	return primary
}

func newKPMClient(options *config.GlobalConfig) (*kpm.Client, error) {
	runner, err := kpm.NewExecRunner(options.KPMControl, options.KPMModule)
	if err != nil {
		return nil, err
	}
	return kpm.NewClient(runner, options.KPMProfile)
}
