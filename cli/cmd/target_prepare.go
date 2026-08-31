package cmd

import (
	"context"
	"errors"
	"fmt"

	"stackplz/user/kpm"
)

type TargetSource string

const (
	TargetSourceProc TargetSource = "proc"
	TargetSourceKPM  TargetSource = "kpm"
	TargetSourceAuto TargetSource = "auto"
)

func ParseTargetSource(value string) (TargetSource, error) {
	source := TargetSource(value)
	switch source {
	case TargetSourceProc, TargetSourceKPM, TargetSourceAuto:
		return source, nil
	default:
		return "", fmt.Errorf("invalid task source %q (want proc, kpm, or auto)", value)
	}
}

type TargetResolver interface {
	UIDFromProc(pid uint32) (uint32, error)
	IdentityFromKPM(ctx context.Context, pid uint32) (kpm.Identity, error)
}

type ResolvedTarget struct {
	PID      uint32
	UID      uint32
	Source   TargetSource
	Identity *kpm.Identity
}

type TargetResolutionError struct {
	PID     uint32
	ProcErr error
	KPMErr  error
}

func (err *TargetResolutionError) Error() string {
	return fmt.Sprintf("resolve pid %d: proc: %v; kpm: %v", err.PID, err.ProcErr, err.KPMErr)
}

func (err *TargetResolutionError) Is(target error) bool {
	return errors.Is(err.ProcErr, target) || errors.Is(err.KPMErr, target)
}

func preparePIDTargets(ctx context.Context, resolver TargetResolver, source TargetSource, pids []uint32, uidOverride *uint32) ([]ResolvedTarget, error) {
	if resolver == nil {
		return nil, errors.New("target resolver is nil")
	}
	if _, err := ParseTargetSource(string(source)); err != nil {
		return nil, err
	}
	if len(pids) == 0 {
		return nil, errors.New("no target pid")
	}

	resolved := make([]ResolvedTarget, 0, len(pids))
	for _, pid := range pids {
		if pid == 0 {
			return nil, errors.New("target pid must be nonzero")
		}
		target, err := preparePIDTarget(ctx, resolver, source, pid, uidOverride)
		if err != nil {
			return nil, err
		}
		resolved = append(resolved, target)
	}
	return resolved, nil
}

func preparePIDTarget(ctx context.Context, resolver TargetResolver, source TargetSource, pid uint32, uidOverride *uint32) (ResolvedTarget, error) {
	switch source {
	case TargetSourceProc:
		return resolveProcTarget(resolver, pid, uidOverride)
	case TargetSourceKPM:
		return resolveKPMTarget(ctx, resolver, pid, uidOverride)
	case TargetSourceAuto:
		procTarget, procErr := resolveProcTarget(resolver, pid, nil)
		if procErr == nil {
			if uidOverride != nil {
				procTarget.UID = *uidOverride
			}
			return procTarget, nil
		}
		kpmTarget, kpmErr := resolveKPMTarget(ctx, resolver, pid, uidOverride)
		if kpmErr == nil {
			return kpmTarget, nil
		}
		return ResolvedTarget{}, &TargetResolutionError{PID: pid, ProcErr: procErr, KPMErr: kpmErr}
	default:
		return ResolvedTarget{}, fmt.Errorf("invalid task source %q", source)
	}
}

func resolveProcTarget(resolver TargetResolver, pid uint32, uidOverride *uint32) (ResolvedTarget, error) {
	if uidOverride != nil {
		return ResolvedTarget{PID: pid, UID: *uidOverride, Source: TargetSourceProc}, nil
	}
	uid, err := resolver.UIDFromProc(pid)
	if err != nil {
		return ResolvedTarget{}, fmt.Errorf("resolve pid %d UID from proc: %w", pid, err)
	}
	return ResolvedTarget{PID: pid, UID: uid, Source: TargetSourceProc}, nil
}

func resolveKPMTarget(ctx context.Context, resolver TargetResolver, pid uint32, uidOverride *uint32) (ResolvedTarget, error) {
	identity, err := resolver.IdentityFromKPM(ctx, pid)
	if err != nil {
		return ResolvedTarget{}, fmt.Errorf("resolve pid %d identity from KPM: %w", pid, err)
	}
	if identity.PID != pid && identity.TGID != pid {
		return ResolvedTarget{}, fmt.Errorf("KPM identity mismatch: requested pid %d, observed pid=%d tgid=%d", pid, identity.PID, identity.TGID)
	}
	uid := identity.UID
	if uidOverride != nil {
		uid = *uidOverride
	}
	return ResolvedTarget{PID: pid, UID: uid, Source: TargetSourceKPM, Identity: &identity}, nil
}

func singleBreakpointPID(pids []uint32) (uint32, error) {
	if len(pids) != 1 || pids[0] == 0 {
		return 0, fmt.Errorf("direct breakpoint mode requires exactly one nonzero target pid")
	}
	return pids[0], nil
}
