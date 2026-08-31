package cmd

import (
	"errors"
	"fmt"

	"stackplz/user/event"
)

func resolveBreakpointBase(pid uint32, library, mapsFile string, explicitBase uint64) (uint64, error) {
	if explicitBase != 0 {
		return explicitBase, nil
	}
	if library == "" {
		return 0, errors.New("breakpoint library is empty")
	}
	if mapsFile != "" {
		if err := event.LoadMapsFile(pid, mapsFile); err != nil {
			return 0, err
		}
	}
	info, err := event.FindLibInMaps(pid, library)
	if err != nil {
		return 0, fmt.Errorf("resolve breakpoint library %q for pid %d: %w", library, pid, err)
	}
	return info.BaseAddr, nil
}

func resolveBreakpointBaseForSource(source TargetSource, pid uint32, library, mapsFile string, explicitBase uint64) (uint64, error) {
	if _, err := ParseTargetSource(string(source)); err != nil {
		return 0, err
	}
	return resolveBreakpointBase(pid, library, mapsFile, explicitBase)
}
