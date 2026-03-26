package rawinject

import (
	"encoding/hex"
	"testing"
	"time"

	"github.com/mstarzak/phytally/phytally-hub/internal/protocol"
)

func TestBuildMgmtPacketContainsRadiotapAndVendorElement(t *testing.T) {
	t.Parallel()

	var payload protocol.HubPayload
	payload.OUI = [3]byte{protocol.VendorOUI0, protocol.VendorOUI1, protocol.VendorOUI2}
	payload.ProtocolVersion = 0x01
	payload.States[0] = byte(protocol.TallyProgram)

	packet := buildMgmtPacket(0x80, frameConfig{
		SourceMAC:      [6]byte{0xf8, 0x1a, 0x67, 0x1c, 0x83, 0xe5},
		BSSID:          [6]byte{0xf8, 0x1a, 0x67, 0x1c, 0x83, 0xe5},
		SSID:           "PhyTally",
		Channel:        1,
		Rate500Kbps:    2,
		Sequence:       7,
		BeaconInterval: protocol.BeaconIntervalMS,
		Payload:        payload,
	})

	if got := hex.EncodeToString(packet[:9]); got != "000009000400000002" {
		t.Fatalf("unexpected radiotap header: %s", got)
	}
	if packet[9] != 0x80 || packet[10] != 0x00 {
		t.Fatalf("unexpected beacon frame control: %x %x", packet[9], packet[10])
	}
	foundVendor := false
	for i := 36; i+2 < len(packet); i++ {
		if packet[i] == 0xdd && packet[i+1] == 29 {
			foundVendor = true
			break
		}
	}
	if !foundVendor {
		t.Fatalf("missing vendor element")
	}
}

func TestCurrentPacketsIncludesBeaconAndProbeResponse(t *testing.T) {
	t.Parallel()

	backend := New(Config{
		Interface:     "wlan0",
		Channel:       1,
		SSID:          "PhyTally",
		TxRate500Kbps: 2,
	})
	backend.state.MAC = "F8:1A:67:1C:83:E5"
	backend.lastPayload.OUI = [3]byte{protocol.VendorOUI0, protocol.VendorOUI1, protocol.VendorOUI2}
	backend.lastPayload.ProtocolVersion = 0x01

	packets, err := backend.currentPackets()
	if err != nil {
		t.Fatalf("currentPackets: %v", err)
	}
	if len(packets) != 2 {
		t.Fatalf("expected 2 packets, got %d", len(packets))
	}
	if packets[0][9] != 0x80 {
		t.Fatalf("expected beacon frame, got %#x", packets[0][9])
	}
	if packets[1][9] != 0x50 {
		t.Fatalf("expected probe response frame, got %#x", packets[1][9])
	}
}

func TestParseTelemetryPacket(t *testing.T) {
	t.Parallel()

	raw, err := hex.DecodeString("0000090020000000d640000000f81a671c83e5807d3ac44060f81a671c83e50000dd0a112244010064d70000ff")
	if err != nil {
		t.Fatalf("decode packet: %v", err)
	}

	telem, ok := parseTelemetryPacketCommon(raw, time.Unix(1700000000, 0))
	if !ok {
		t.Fatalf("expected telemetry packet")
	}
	if telem.SourceMAC != "80:7D:3A:C4:40:60" {
		t.Fatalf("unexpected source mac: %s", telem.SourceMAC)
	}
	if telem.TallyID != 1 {
		t.Fatalf("unexpected tally id: %d", telem.TallyID)
	}
	if telem.ReportedHubRSSI != -41 {
		t.Fatalf("unexpected hub rssi: %d", telem.ReportedHubRSSI)
	}
	if telem.BrightnessPercent != 100 {
		t.Fatalf("unexpected brightness: %d", telem.BrightnessPercent)
	}
	if telem.AirRSSI != -42 {
		t.Fatalf("unexpected air rssi: %d", telem.AirRSSI)
	}
	if telem.AckCommandID != 0 {
		t.Fatalf("unexpected ack command id: %d", telem.AckCommandID)
	}
}

func TestParseTelemetryPacketWithAck(t *testing.T) {
	t.Parallel()

	raw, err := hex.DecodeString("0000090020000000d640000000f81a671c83e5807d3ac44060f81a671c83e50000dd0b112244010064d70000ff07")
	if err != nil {
		t.Fatalf("decode packet: %v", err)
	}

	telem, ok := parseTelemetryPacketCommon(raw, time.Unix(1700000000, 0))
	if !ok {
		t.Fatalf("expected telemetry packet")
	}
	if telem.AckCommandID != 7 {
		t.Fatalf("unexpected ack command id: %d", telem.AckCommandID)
	}
}
