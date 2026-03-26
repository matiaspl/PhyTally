package switcher

import (
	"testing"

	"github.com/mstarzak/phytally/phytally-hub/internal/protocol"
)

func TestParseTSLPacket(t *testing.T) {
	t.Parallel()

	tallies, ok := parseTSLPacket([]byte{0x80 + 2, 0x01})
	if !ok {
		t.Fatalf("expected TSL packet to parse")
	}
	if tallies[2] != protocol.TallyProgram {
		t.Fatalf("expected tally 3 to be program, got %d", tallies[2])
	}

	tallies, ok = parseTSLPacket([]byte{0x80 + 1, 0x02})
	if !ok {
		t.Fatalf("expected TSL preview packet to parse")
	}
	if tallies[1] != protocol.TallyPreview {
		t.Fatalf("expected tally 2 to be preview, got %d", tallies[1])
	}
}
