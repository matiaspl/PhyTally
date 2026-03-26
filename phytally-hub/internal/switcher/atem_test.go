package switcher

import (
	"testing"

	"github.com/mstarzak/phytally/phytally-hub/internal/protocol"
)

func TestParseATEMTlIn(t *testing.T) {
	t.Parallel()

	body := []byte{0x00, 0x04, 0x01, 0x02, 0x00, 0x03}
	tallies := parseATEMTlIn(body)
	if tallies[0] != protocol.TallyProgram {
		t.Fatalf("expected tally 1 to be program")
	}
	if tallies[1] != protocol.TallyPreview {
		t.Fatalf("expected tally 2 to be preview")
	}
	if tallies[2] != protocol.TallyOff {
		t.Fatalf("expected tally 3 to be off")
	}
	if tallies[3] != protocol.TallyProgram {
		t.Fatalf("expected tally 4 to be program when both bits are set")
	}
}
