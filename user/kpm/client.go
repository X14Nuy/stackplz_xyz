package kpm

import (
	"context"
	"encoding/hex"
	"errors"
	"fmt"
	"hash/crc32"
	"strconv"
	"sync"
	"time"
)

type Client struct {
	mu                  sync.Mutex
	runner              Runner
	profileID           string
	ready               bool
	profileValidated    bool
	mapsCapabilityKnown bool
	mapsSupported       bool
	mapsSnapshot        uint64
	mapsSize            uint32
}

const requestPollInterval = time.Millisecond

func NewClient(runner Runner, profileID string) (*Client, error) {
	if runner == nil || !safeAtom(profileID, 128) {
		return nil, fmt.Errorf("%w: runner or profile", ErrInvalidCommand)
	}
	return &Client{runner: runner, profileID: profileID}, nil
}

func (client *Client) Status(ctx context.Context) (ModuleStatus, error) {
	client.mu.Lock()
	defer client.mu.Unlock()
	return client.statusLocked(ctx)
}

func (client *Client) statusLocked(ctx context.Context) (ModuleStatus, error) {
	var status ModuleStatus
	response, err := client.controlLocked(ctx, "status")
	if err != nil {
		client.ready = false
		return status, err
	}
	status.State = response.Fields["state"]
	status.Profile = response.Fields["profile"]
	status.Binding = response.Fields["binding"]
	requestID, requestState, requestStatus, requestPresent, requestErr :=
		requestFromFields(response.Fields)
	if requestErr != nil {
		return status, requestErr
	}
	status.RequestID = requestID
	status.RequestState = requestState
	status.RequestStatus = requestStatus
	status.RequestPresent = requestPresent
	mapsPresent, mapsSupported, mapsState, mapsSnapshot, mapsSize, mapsErr :=
		mapsStatusFromFields(response.Fields)
	if mapsErr != nil {
		return status, mapsErr
	}
	status.MapsCapabilityPresent = mapsPresent
	status.MapsSupported = mapsSupported
	status.MapsState = mapsState
	status.MapsSnapshot = mapsSnapshot
	status.MapsSize = mapsSize
	client.mapsCapabilityKnown = mapsPresent
	client.mapsSupported = mapsSupported
	client.mapsSnapshot = mapsSnapshot
	client.mapsSize = mapsSize
	if status.Profile != client.profileID {
		client.ready = false
		client.profileValidated = false
		return status, fmt.Errorf("%w: got %q want %q", ErrProfileMismatch, status.Profile, client.profileID)
	}
	client.profileValidated = true
	if status.State != "ready" {
		client.ready = false
		return status, fmt.Errorf("%w: state %q", ErrNotReady, status.State)
	}
	client.ready = true
	if status.Binding == "bound" {
		identity, parseErr := identityFromFields(response.Fields)
		if parseErr != nil {
			return status, parseErr
		}
		status.Identity = &identity
	}
	return status, nil
}

func (client *Client) Bind(ctx context.Context, request BindingRequest) (uint64, error) {
	command, err := BindCommand(request)
	if err != nil {
		return 0, err
	}
	client.mu.Lock()
	defer client.mu.Unlock()
	if !client.ready {
		return 0, ErrNotReady
	}
	response, err := client.controlLocked(ctx, command)
	if err != nil {
		return 0, err
	}
	generation, err := parseUintField(response.Fields, "generation", 64)
	if err != nil || generation == 0 {
		return 0, fmt.Errorf("%w: generation", ErrMalformedResponse)
	}
	return generation, nil
}

func (client *Client) WaitBound(ctx context.Context, interval time.Duration) (Identity, error) {
	if interval <= 0 {
		interval = time.Millisecond
	}
	for {
		status, err := client.Status(ctx)
		if err != nil {
			return Identity{}, err
		}
		switch status.Binding {
		case "bound":
			if status.Identity == nil {
				return Identity{}, ErrMalformedResponse
			}
			return *status.Identity, nil
		case "stale":
			return Identity{}, ErrBindingStale
		case "exited":
			return Identity{}, ErrBindingExited
		case "pending", "none", "":
		default:
			return Identity{}, fmt.Errorf("%w: binding %q", ErrMalformedResponse, status.Binding)
		}
		timer := time.NewTimer(interval)
		select {
		case <-ctx.Done():
			if !timer.Stop() {
				<-timer.C
			}
			return Identity{}, ctx.Err()
		case <-timer.C:
		}
	}
}

func (client *Client) ConfigureBreakpoint(ctx context.Context, breakpoint Breakpoint) error {
	command, err := BreakCommand(breakpoint)
	if err != nil {
		return err
	}
	return client.readyControl(ctx, command)
}

func (client *Client) Enable(ctx context.Context, id uint32) error {
	if id == 0 {
		return ErrInvalidID
	}
	return client.readyAsyncControl(ctx, fmt.Sprintf("enable id=%d", id))
}

func (client *Client) Disable(ctx context.Context, id uint32) error {
	if id == 0 {
		return ErrInvalidID
	}
	return client.readyAsyncControl(ctx, fmt.Sprintf("disable id=%d", id))
}

func (client *Client) Clear(ctx context.Context) error {
	client.mu.Lock()
	defer client.mu.Unlock()
	if !client.profileValidated {
		return ErrNotReady
	}
	return client.asyncControlLocked(ctx, "clear")
}

func (client *Client) SnapshotMaps(ctx context.Context) ([]byte, error) {
	client.mu.Lock()
	defer client.mu.Unlock()
	if !client.ready {
		return nil, ErrNotReady
	}
	if client.mapsCapabilityKnown && !client.mapsSupported {
		return nil, ErrMapsUnsupported
	}
	requestID, err := client.submitAsyncLocked(ctx, "maps")
	if err != nil {
		var controlErr *ControlError
		if errors.As(err, &controlErr) && controlErr.Reason == "unsupported" {
			return nil, ErrMapsUnsupported
		}
		return nil, err
	}
	if requestID == 0 {
		return nil, ErrMalformedResponse
	}
	if err := client.waitRequestLocked(ctx, requestID); err != nil {
		return nil, err
	}
	if client.mapsCapabilityKnown &&
		(client.mapsSnapshot != requestID || client.mapsSize == 0) {
		return nil, ErrMalformedResponse
	}

	var content []byte
	var total uint32
	var checksum uint32
	for offset := uint32(0); ; {
		command, commandErr := MapsReadCommand(requestID, offset)
		if commandErr != nil {
			return nil, commandErr
		}
		response, controlErr := client.controlLocked(ctx, command)
		if controlErr != nil {
			return nil, controlErr
		}
		chunk, parseErr := mapsChunkFromFields(response.Fields)
		if parseErr != nil || chunk.Snapshot != requestID ||
			chunk.Offset != offset {
			return nil, ErrMalformedResponse
		}
		if offset == 0 {
			total = chunk.Total
			checksum = chunk.CRC32
			if total == 0 || total > MaxMapsSnapshotBytes ||
				(client.mapsCapabilityKnown && client.mapsSize != total) {
				return nil, ErrMalformedResponse
			}
			content = make([]byte, 0, total)
		} else if chunk.Total != total || chunk.CRC32 != checksum {
			return nil, ErrMalformedResponse
		}
		if uint64(offset)+uint64(len(chunk.Data)) > uint64(total) {
			return nil, ErrMalformedResponse
		}
		content = append(content, chunk.Data...)
		next := offset + uint32(len(chunk.Data))
		if chunk.EOF {
			if next != total {
				return nil, ErrMalformedResponse
			}
			break
		}
		if next <= offset || next >= total {
			return nil, ErrMalformedResponse
		}
		offset = next
	}
	if crc32.ChecksumIEEE(content) != checksum {
		return nil, ErrMapsChecksum
	}
	return content, nil
}

func (client *Client) Poll(ctx context.Context, after uint64) (Event, bool, error) {
	client.mu.Lock()
	defer client.mu.Unlock()
	if !client.ready {
		return Event{}, false, ErrNotReady
	}
	response, err := client.controlLocked(ctx, fmt.Sprintf("poll after=%d", after))
	if err != nil {
		return Event{}, false, err
	}
	encoded, found := response.Fields["event"]
	if !found {
		if response.Fields["empty"] == "1" {
			return Event{}, false, nil
		}
		return Event{}, false, ErrMalformedResponse
	}
	event, err := DecodeEventHex(encoded)
	if err != nil {
		return Event{}, false, err
	}
	return event, true, nil
}

func (client *Client) readyControl(ctx context.Context, command string) error {
	client.mu.Lock()
	defer client.mu.Unlock()
	if !client.ready {
		return ErrNotReady
	}
	_, err := client.controlLocked(ctx, command)
	return err
}

func (client *Client) readyAsyncControl(ctx context.Context, command string) error {
	client.mu.Lock()
	defer client.mu.Unlock()
	if !client.ready {
		return ErrNotReady
	}
	return client.asyncControlLocked(ctx, command)
}

func (client *Client) asyncControlLocked(ctx context.Context, command string) error {
	requestID, err := client.submitAsyncLocked(ctx, command)
	if err != nil {
		return err
	}
	if requestID == 0 {
		return nil
	}
	return client.waitRequestLocked(ctx, requestID)
}

func (client *Client) submitAsyncLocked(ctx context.Context, command string) (uint64, error) {
	response, err := client.controlLocked(ctx, command)
	if err != nil {
		return 0, err
	}
	requestID, err := parseUintField(response.Fields, "request", 64)
	if err != nil {
		return 0, ErrMalformedResponse
	}
	return requestID, nil
}

func (client *Client) waitRequestLocked(ctx context.Context, requestID uint64) error {
	for {
		if err := ctx.Err(); err != nil {
			return err
		}
		status, err := client.statusLocked(ctx)
		if err != nil {
			return err
		}
		if !status.RequestPresent || status.RequestID != requestID {
			return fmt.Errorf("%w: request got %d want %d", ErrMalformedResponse,
				status.RequestID, requestID)
		}
		switch status.RequestState {
		case "done":
			if status.RequestStatus != 0 {
				return &AsyncRequestError{RequestID: requestID, Status: status.RequestStatus}
			}
			return nil
		case "pending", "running":
		case "free":
			return fmt.Errorf("%w: request %d is free", ErrMalformedResponse, requestID)
		default:
			return fmt.Errorf("%w: request state %q", ErrMalformedResponse,
				status.RequestState)
		}
		timer := time.NewTimer(requestPollInterval)
		select {
		case <-ctx.Done():
			if !timer.Stop() {
				<-timer.C
			}
			return ctx.Err()
		case <-timer.C:
		}
	}
}

func (client *Client) controlLocked(ctx context.Context, command string) (Response, error) {
	output, err := client.runner.Control(ctx, command)
	if err != nil {
		return Response{}, err
	}
	return parseResponse(output)
}

func identityFromFields(fields map[string]string) (Identity, error) {
	var identity Identity
	var err error
	if identity.Generation, err = parseUintField(fields, "generation", 64); err != nil || identity.Generation == 0 {
		return identity, ErrMalformedResponse
	}
	if identity.TaskCookie, err = parseUintField(fields, "task_cookie", 64); err != nil {
		return identity, ErrMalformedResponse
	}
	pid, err := parseUintField(fields, "pid", 32)
	if err != nil || pid == 0 {
		return identity, ErrMalformedResponse
	}
	tgid, err := parseUintField(fields, "tgid", 32)
	if err != nil || tgid == 0 {
		return identity, ErrMalformedResponse
	}
	uid, err := parseUintField(fields, "uid", 32)
	if err != nil {
		return identity, ErrMalformedResponse
	}
	identity.PID, identity.TGID, identity.UID = uint32(pid), uint32(tgid), uint32(uid)
	if identity.StartTime, err = parseUintField(fields, "start_time", 64); err != nil {
		return identity, ErrMalformedResponse
	}
	if identity.StartBootTime, err = parseUintField(fields, "start_boottime", 64); err != nil {
		return identity, ErrMalformedResponse
	}
	commHex, hasCommHex := fields["comm_hex"]
	comm, hasComm := fields["comm"]
	if hasCommHex == hasComm {
		return identity, ErrMalformedResponse
	}
	if hasCommHex {
		if len(commHex) != CommBytes*2 {
			return identity, ErrMalformedResponse
		}
		raw, decodeErr := hex.DecodeString(commHex)
		if decodeErr != nil || len(raw) != CommBytes {
			return identity, ErrMalformedResponse
		}
		terminator := len(raw)
		for index, value := range raw {
			if value == 0 {
				terminator = index
				break
			}
		}
		if terminator == len(raw) {
			return identity, ErrMalformedResponse
		}
		identity.Comm = string(raw[:terminator])
	} else {
		identity.Comm = comm
		if !safeAtom(identity.Comm, CommBytes-1) {
			return identity, ErrMalformedResponse
		}
	}
	return identity, nil
}

func parseUintField(fields map[string]string, key string, bits int) (uint64, error) {
	value, found := fields[key]
	if !found {
		return 0, ErrMalformedResponse
	}
	parsed, err := strconv.ParseUint(value, 0, bits)
	if err != nil {
		return 0, ErrMalformedResponse
	}
	return parsed, nil
}

func requestFromFields(fields map[string]string) (uint64, string, int, bool, error) {
	requestValue, hasRequest := fields["request"]
	state, hasState := fields["request_state"]
	statusValue, hasStatus := fields["request_status"]
	if !hasRequest && !hasState && !hasStatus {
		return 0, "", 0, false, nil
	}
	if !hasRequest || !hasState || !hasStatus {
		return 0, "", 0, false, ErrMalformedResponse
	}
	requestID, err := strconv.ParseUint(requestValue, 10, 64)
	if err != nil {
		return 0, "", 0, false, ErrMalformedResponse
	}
	requestStatus, err := strconv.Atoi(statusValue)
	if err != nil {
		return 0, "", 0, false, ErrMalformedResponse
	}
	return requestID, state, requestStatus, true, nil
}

type mapsChunk struct {
	Snapshot uint64
	Offset   uint32
	Total    uint32
	CRC32    uint32
	EOF      bool
	Data     []byte
}

func mapsChunkFromFields(fields map[string]string) (mapsChunk, error) {
	var chunk mapsChunk
	var err error
	if chunk.Snapshot, err = parseDecimalField(fields, "snapshot", 64); err != nil || chunk.Snapshot == 0 {
		return chunk, ErrMalformedResponse
	}
	offset, err := parseDecimalField(fields, "offset", 32)
	if err != nil {
		return chunk, ErrMalformedResponse
	}
	total, err := parseDecimalField(fields, "total", 32)
	if err != nil || total == 0 || total > MaxMapsSnapshotBytes {
		return chunk, ErrMalformedResponse
	}
	checksum, err := parseDecimalField(fields, "crc32", 32)
	if err != nil {
		return chunk, ErrMalformedResponse
	}
	eof, err := parseDecimalField(fields, "eof", 8)
	if err != nil || eof > 1 {
		return chunk, ErrMalformedResponse
	}
	encoded, found := fields["data"]
	if !found || len(encoded) == 0 || len(encoded)%2 != 0 ||
		len(encoded) > MaxMapsChunkBytes*2 {
		return chunk, ErrMalformedResponse
	}
	data, err := hex.DecodeString(encoded)
	if err != nil || len(data) == 0 || len(data) > MaxMapsChunkBytes {
		return chunk, ErrMalformedResponse
	}
	chunk.Offset = uint32(offset)
	chunk.Total = uint32(total)
	chunk.CRC32 = uint32(checksum)
	chunk.EOF = eof == 1
	chunk.Data = data
	return chunk, nil
}

func mapsStatusFromFields(fields map[string]string) (bool, bool, string, uint64, uint32, error) {
	_, hasSupported := fields["maps_supported"]
	_, hasState := fields["maps_state"]
	_, hasSnapshot := fields["maps_snapshot"]
	_, hasSize := fields["maps_size"]
	if !hasSupported && !hasState && !hasSnapshot && !hasSize {
		return false, false, "", 0, 0, nil
	}
	if !hasSupported || !hasState || !hasSnapshot || !hasSize {
		return false, false, "", 0, 0, ErrMalformedResponse
	}
	supported, err := parseDecimalField(fields, "maps_supported", 8)
	if err != nil || supported > 1 {
		return false, false, "", 0, 0, ErrMalformedResponse
	}
	state := fields["maps_state"]
	if !safeAtom(state, 32) {
		return false, false, "", 0, 0, ErrMalformedResponse
	}
	snapshot, err := parseDecimalField(fields, "maps_snapshot", 64)
	if err != nil {
		return false, false, "", 0, 0, ErrMalformedResponse
	}
	size, err := parseDecimalField(fields, "maps_size", 32)
	if err != nil || size > MaxMapsSnapshotBytes {
		return false, false, "", 0, 0, ErrMalformedResponse
	}
	if supported == 0 && (state != "unsupported" || snapshot != 0 || size != 0) {
		return false, false, "", 0, 0, ErrMalformedResponse
	}
	return true, supported == 1, state, snapshot, uint32(size), nil
}

func parseDecimalField(fields map[string]string, key string, bits int) (uint64, error) {
	value, found := fields[key]
	if !found {
		return 0, ErrMalformedResponse
	}
	parsed, err := strconv.ParseUint(value, 10, bits)
	if err != nil {
		return 0, ErrMalformedResponse
	}
	return parsed, nil
}
