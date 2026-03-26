package hostapd

import (
	"context"
	"encoding/hex"
	"strings"
	"testing"
	"time"

	"github.com/mstarzak/phytally/phytally-hub/internal/config"
	"github.com/mstarzak/phytally/phytally-hub/internal/hub"
	"github.com/mstarzak/phytally/phytally-hub/internal/protocol"
	"github.com/mstarzak/phytally/phytally-hub/internal/radio"
	"github.com/mstarzak/phytally/phytally-hub/internal/radio/stub"
)

type fakeRunner struct {
	calls []string
}

func (f *fakeRunner) Run(_ context.Context, name string, args ...string) error {
	f.calls = append(f.calls, name+" "+strings.Join(args, " "))
	return nil
}

func TestVendorElementsHexIncludesBeaconTagAndPayload(t *testing.T) {
	t.Parallel()

	var payload protocol.HubPayload
	payload.OUI = [3]byte{protocol.VendorOUI0, protocol.VendorOUI1, protocol.VendorOUI2}
	payload.ProtocolVersion = 0x01
	payload.States[0] = byte(protocol.TallyProgram)

	got := vendorElementsHex(payload)
	if !strings.HasPrefix(got, "dd1d11223301") {
		t.Fatalf("unexpected vendor element prefix: %s", got)
	}
}

func TestUpdateUsesHostapdCLI(t *testing.T) {
	t.Parallel()

	runner := &fakeRunner{}
	backend := New(Config{
		Interface:  "wlan1",
		Channel:    1,
		SSID:       "PhyTally",
		BinaryPath: "hostapd",
		CLIPath:    "hostapd_cli",
		ControlDir: "/tmp/phytally-hostapd",
		ConfigPath: "/tmp/phytally-hostapd/hostapd.conf",
	})
	backend.runner = runner
	backend.started = true

	var payload protocol.HubPayload
	payload.OUI = [3]byte{protocol.VendorOUI0, protocol.VendorOUI1, protocol.VendorOUI2}
	payload.ProtocolVersion = 0x01
	payload.States[0] = byte(protocol.TallyPreview)

	if err := backend.Update(payload); err != nil {
		t.Fatalf("update: %v", err)
	}

	if len(runner.calls) != 2 {
		t.Fatalf("expected 2 commands, got %d", len(runner.calls))
	}
	if !strings.Contains(runner.calls[0], "set vendor_elements") {
		t.Fatalf("missing vendor_elements update: %v", runner.calls)
	}
	if !strings.Contains(runner.calls[1], "update_beacon") {
		t.Fatalf("missing beacon refresh: %v", runner.calls)
	}
}

func TestParseTelemetryPacket(t *testing.T) {
	t.Parallel()

	raw, err := hex.DecodeString("0000090020000000d640000000f81a671c83e5807d3ac44060f81a671c83e50000dd0a112244010064d70000ff")
	if err != nil {
		t.Fatalf("decode packet: %v", err)
	}

	telem, ok := parseTelemetryPacket(raw, time.Unix(1700000000, 0))
	if !ok {
		t.Fatalf("expected telemetry packet")
	}
	if telem.SourceMAC != "80:7D:3A:C4:40:60" {
		t.Fatalf("unexpected source mac: %s", telem.SourceMAC)
	}
	if telem.TallyID != 1 {
		t.Fatalf("unexpected tally id: %d", telem.TallyID)
	}
	if telem.BatteryPercent != 100 {
		t.Fatalf("unexpected battery: %d", telem.BatteryPercent)
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

func TestHubTelemetryUpdatesStatus(t *testing.T) {
	t.Parallel()

	app := hub.New(config.Config{
		APSSID:       "PhyTally",
		RadioChannel: 1,
	}, "test.json", stub.New("wlan1", 1, "PhyTally"))

	app.HandleTelemetry(radio.Telemetry{
		SourceMAC:         "80:7D:3A:C4:40:60",
		TallyID:           2,
		BatteryPercent:    87,
		BrightnessPercent: 42,
		ReportedHubRSSI:   -38,
		AirRSSI:           -46,
		ReceivedAt:        time.Unix(1700000100, 0),
	})

	status := app.Status()
	if got := status["receivers"].([]map[string]any); len(got) != 1 {
		t.Fatalf("expected one receiver, got %d", len(got))
	}
	telemetry := status["telemetry"].(map[string]any)
	if telemetry["packet_count"].(uint64) != 1 {
		t.Fatalf("unexpected packet count: %#v", telemetry["packet_count"])
	}
}
