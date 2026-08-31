package kpm

import (
	"context"
	"fmt"
	"os/exec"
	"strconv"
	"strings"
)

type Runner interface {
	Control(context.Context, string) (string, error)
}

type commandExecutor func(context.Context, string, ...string) ([]byte, error)

type ExecRunner struct {
	binary string
	module string
	run    commandExecutor
}

func NewExecRunner(binary, module string) (*ExecRunner, error) {
	if binary == "" || !safeAtom(module, 32) {
		return nil, fmt.Errorf("%w: binary or module", ErrInvalidCommand)
	}
	return &ExecRunner{binary: binary, module: module, run: runCommand}, nil
}

func runCommand(ctx context.Context, binary string, args ...string) ([]byte, error) {
	return exec.CommandContext(ctx, binary, args...).CombinedOutput()
}

func (r *ExecRunner) Control(ctx context.Context, command string) (string, error) {
	if r == nil || r.run == nil || r.binary == "" || !safeAtom(r.module, 32) || !safeCommand(command) {
		return "", ErrInvalidCommand
	}
	output, err := r.run(ctx, r.binary, "kpm", "ctl0", r.module, command)
	if len(output) > MaxResponseBytes {
		return "", fmt.Errorf("%w: response exceeds %d bytes", ErrMalformedResponse, MaxResponseBytes)
	}
	if err != nil {
		trimmed := strings.TrimSpace(string(output))
		if trimmed == "" {
			return "", fmt.Errorf("KPM control process: %w", err)
		}
		return "", fmt.Errorf("KPM control process: %w: %s", err, trimmed)
	}
	return string(output), nil
}

func BindCommand(request BindingRequest) (string, error) {
	if request.PID == 0 {
		return "", fmt.Errorf("%w: zero pid", ErrInvalidID)
	}
	if request.Mode != BindPID && request.Mode != BindTGID && request.Mode != BindEither {
		return "", fmt.Errorf("%w: bind mode %q", ErrInvalidMode, request.Mode)
	}
	if request.Comm != "" && !safeAtom(request.Comm, CommBytes-1) {
		return "", fmt.Errorf("%w: comm", ErrInvalidCommand)
	}
	var builder strings.Builder
	fmt.Fprintf(&builder, "bind pid=%d mode=%s", request.PID, request.Mode)
	if request.UID != nil {
		fmt.Fprintf(&builder, " uid=%d", *request.UID)
	}
	if request.Comm != "" {
		fmt.Fprintf(&builder, " comm=%s", request.Comm)
	}
	if request.StartBootTime != nil {
		fmt.Fprintf(&builder, " start=%d", *request.StartBootTime)
	}
	command := builder.String()
	if !safeCommand(command) {
		return "", ErrInvalidCommand
	}
	return command, nil
}

func BreakCommand(breakpoint Breakpoint) (string, error) {
	if breakpoint.ID == 0 {
		return "", ErrInvalidID
	}
	if breakpoint.Kind != BreakExecute && breakpoint.Kind != BreakRead && breakpoint.Kind != BreakWrite && breakpoint.Kind != BreakReadWrite {
		return "", ErrInvalidKind
	}
	if breakpoint.Mode != BreakOnce && breakpoint.Mode != BreakRepeat {
		return "", ErrInvalidMode
	}
	if breakpoint.Address == 0 || breakpoint.Address>>56 != 0 {
		return "", ErrInvalidAddress
	}
	if breakpoint.Kind == BreakExecute {
		if breakpoint.Length != 4 {
			return "", ErrInvalidLength
		}
		if breakpoint.Address&3 != 0 {
			return "", ErrInvalidAddress
		}
	} else {
		switch breakpoint.Length {
		case 1, 2, 4, 8:
		default:
			return "", ErrInvalidLength
		}
		end := breakpoint.Address + uint64(breakpoint.Length) - 1
		if end < breakpoint.Address || breakpoint.Address>>3 != end>>3 {
			return "", ErrInvalidAddress
		}
	}
	return fmt.Sprintf("break id=%d kind=%s addr=0x%x len=%d mode=%s",
		breakpoint.ID, breakpoint.Kind, breakpoint.Address, breakpoint.Length, breakpoint.Mode), nil
}

func MapsReadCommand(snapshot uint64, offset uint32) (string, error) {
	if snapshot == 0 {
		return "", ErrInvalidID
	}
	return fmt.Sprintf("maps-read snapshot=%d offset=%d", snapshot, offset), nil
}

func parseResponse(output string) (Response, error) {
	response := Response{Fields: make(map[string]string)}
	if len(output) == 0 || len(output) > MaxResponseBytes {
		return response, ErrMalformedResponse
	}
	tokens := strings.Fields(output)
	if len(tokens) < 2 || !strings.HasPrefix(tokens[0], "status=") || !strings.HasPrefix(tokens[1], "version=") {
		return response, ErrMalformedResponse
	}
	status, err := strconv.Atoi(strings.TrimPrefix(tokens[0], "status="))
	if err != nil {
		return response, ErrMalformedResponse
	}
	version, err := strconv.ParseUint(strings.TrimPrefix(tokens[1], "version="), 10, 16)
	if err != nil {
		return response, ErrMalformedResponse
	}
	response.Status = status
	response.Version = uint16(version)
	if response.Version != ProtocolVersion {
		return response, ErrUnsupportedVersion
	}
	for _, token := range tokens[2:] {
		key, value, found := strings.Cut(token, "=")
		if !found || key == "" || value == "" || !safeAtom(key, 64) {
			return response, ErrMalformedResponse
		}
		if _, duplicate := response.Fields[key]; duplicate {
			return response, ErrMalformedResponse
		}
		response.Fields[key] = value
	}
	if response.Status != 0 {
		return response, &ControlError{Status: response.Status, Reason: response.Fields["reason"]}
	}
	return response, nil
}

func safeCommand(command string) bool {
	if command == "" || len(command) > MaxCommandBytes {
		return false
	}
	for _, char := range []byte(command) {
		if char < 0x20 || char > 0x7e {
			return false
		}
	}
	return true
}

func safeAtom(value string, limit int) bool {
	if value == "" || len(value) > limit {
		return false
	}
	for _, char := range []byte(value) {
		if (char >= 'a' && char <= 'z') || (char >= 'A' && char <= 'Z') ||
			(char >= '0' && char <= '9') || char == '.' || char == '_' || char == ':' || char == '-' {
			continue
		}
		return false
	}
	return true
}
