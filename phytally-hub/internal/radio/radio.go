package radio

import (
	"context"
	"time"

	"github.com/mstarzak/phytally/phytally-hub/internal/protocol"
)

type State struct {
	Interface string
	MAC       string
	Channel   int
	SSID      string
	StartedAt time.Time
}

type Telemetry struct {
	SourceMAC         string
	TallyID           int
	BatteryPercent    int
	BrightnessPercent int
	AckCommandID      int
	ReportedHubRSSI   int
	AirRSSI           int
	ReceivedAt        time.Time
}

type Backend interface {
	State() State
	Name() string
	Start(ctx context.Context) error
	Update(payload protocol.HubPayload) error
	SetTelemetryHandler(func(Telemetry))
}

type SurveyCoordinator interface {
	PrepareScan(ctx context.Context) error
	FinishScan(ctx context.Context) error
}
