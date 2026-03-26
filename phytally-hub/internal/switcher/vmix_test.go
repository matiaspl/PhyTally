package switcher

import (
	"testing"

	"github.com/mstarzak/phytally/phytally-hub/internal/protocol"
)

func TestParseVMixTallyLine(t *testing.T) {
	t.Parallel()

	tallies, ok := parseVMixTallyLine("TALLY OK 1200")
	if !ok {
		t.Fatalf("expected vMix tally line to parse")
	}
	if tallies[0] != protocol.TallyProgram {
		t.Fatalf("expected tally 1 to be program")
	}
	if tallies[1] != protocol.TallyPreview {
		t.Fatalf("expected tally 2 to be preview")
	}
	if tallies[2] != protocol.TallyOff || tallies[3] != protocol.TallyOff {
		t.Fatalf("expected remaining tallies to be off")
	}
}
