package util

import (
	"strings"
	"testing"
)

func TestParseUIDFromPS(t *testing.T) {
	uid, err := parseUIDFromPS(42, "  10234\r\n")
	if err != nil || uid != 10234 {
		t.Fatalf("uid=%d err=%v", uid, err)
	}
}

func TestParseUIDFromPSRejectsBadOutput(t *testing.T) {
	for _, output := range []string{"", "  \n", "u0_a234", "4294967296", "10001\n10002"} {
		_, err := parseUIDFromPS(42, output)
		if err == nil || !strings.Contains(err.Error(), "pid 42") {
			t.Fatalf("output %q produced %v", output, err)
		}
	}
}
