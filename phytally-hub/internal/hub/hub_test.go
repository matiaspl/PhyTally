package hub

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/mstarzak/phytally/phytally-hub/internal/config"
	"github.com/mstarzak/phytally/phytally-hub/internal/protocol"
	"github.com/mstarzak/phytally/phytally-hub/internal/radio"
)

type fakeRadioBackend struct {
	name        string
	lastPayload protocol.HubPayload
}

func (f *fakeRadioBackend) State() radio.State {
	return radio.State{
		Interface: "wlan1",
		Channel:   1,
		SSID:      "PhyTally",
	}
}

func (f *fakeRadioBackend) Name() string {
	if f.name == "" {
		return "fake"
	}
	return f.name
}
func (f *fakeRadioBackend) Start(context.Context) error               { return nil }
func (f *fakeRadioBackend) SetTelemetryHandler(func(radio.Telemetry)) {}
func (f *fakeRadioBackend) Update(payload protocol.HubPayload) error {
	f.lastPayload = payload
	return nil
}

func TestTelemetryAckClearsActiveCommand(t *testing.T) {
	t.Parallel()

	backend := &fakeRadioBackend{}
	app := New(config.Config{
		StaticDir:      t.TempDir(),
		RadioInterface: "wlan1",
		RadioChannel:   1,
		APSSID:         "PhyTally",
		AtemIP:         "0.0.0.0",
		VMixIP:         "0.0.0.0",
	}, filepath.Join(t.TempDir(), "hub.json"), backend)

	if err := app.QueueSetBrightness("80:7D:3A:C4:40:60", 35); err != nil {
		t.Fatalf("queue brightness: %v", err)
	}
	if backend.lastPayload.CommandType != byte(protocol.CommandSetBrightness) {
		t.Fatalf("expected set brightness payload, got %d", backend.lastPayload.CommandType)
	}
	if backend.lastPayload.CommandID == 0 {
		t.Fatalf("expected non-zero command id")
	}

	app.HandleTelemetry(radio.Telemetry{
		SourceMAC:         "80:7D:3A:C4:40:60",
		TallyID:           1,
		BatteryPercent:    100,
		BrightnessPercent: 35,
		AckCommandID:      int(backend.lastPayload.CommandID),
		ReportedHubRSSI:   -40,
		AirRSSI:           -43,
		ReceivedAt:        time.Now(),
	})

	status := app.Status()
	command := status["command"].(map[string]any)
	if active := command["active"].(bool); active {
		t.Fatalf("expected command to be inactive")
	}
	if backend.lastPayload.CommandType != byte(protocol.CommandNone) {
		t.Fatalf("expected cleared payload command type, got %d", backend.lastPayload.CommandType)
	}
	if backend.lastPayload.CommandID != 0 {
		t.Fatalf("expected cleared payload command id, got %d", backend.lastPayload.CommandID)
	}
}

func TestSetWirelessTransportReloadsAndPersists(t *testing.T) {
	t.Parallel()

	configPath := filepath.Join(t.TempDir(), "hub.json")
	initialBackend := &fakeRadioBackend{name: "stub"}
	app := NewWithFactory(config.Config{
		StaticDir:      t.TempDir(),
		RadioInterface: "wlan1",
		RadioChannel:   1,
		RadioBackend:   "stub",
		APSSID:         "PhyTally",
		AtemIP:         "0.0.0.0",
		VMixIP:         "0.0.0.0",
	}, configPath, initialBackend, func(cfg config.Config) (radio.Backend, error) {
		return &fakeRadioBackend{name: cfg.RadioBackend}, nil
	})

	if err := app.SetWirelessTransport("rawinject"); err != nil {
		t.Fatalf("set wireless transport: %v", err)
	}

	status := app.WirelessTransport()
	if got := status["transport"]; got != "rawinject" {
		t.Fatalf("expected rawinject transport, got %v", got)
	}
	if got := app.Status()["wireless"].(map[string]any)["transport"]; got != "rawinject" {
		t.Fatalf("expected status transport rawinject, got %v", got)
	}

	data, err := os.ReadFile(configPath)
	if err != nil {
		t.Fatalf("read saved config: %v", err)
	}

	var saved config.Config
	if err := json.Unmarshal(data, &saved); err != nil {
		t.Fatalf("decode saved config: %v", err)
	}
	if saved.RadioBackend != "rawinject" {
		t.Fatalf("expected persisted backend rawinject, got %q", saved.RadioBackend)
	}
}
