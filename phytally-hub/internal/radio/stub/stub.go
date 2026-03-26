package stub

import (
	"context"
	"net"
	"strings"
	"sync"
	"time"

	"github.com/mstarzak/phytally/phytally-hub/internal/protocol"
	"github.com/mstarzak/phytally/phytally-hub/internal/radio"
)

type Backend struct {
	mu          sync.RWMutex
	state       radio.State
	lastPayload protocol.HubPayload
}

func New(iface string, channel int, ssid string) *Backend {
	return &Backend{
		state: radio.State{
			Interface: iface,
			MAC:       interfaceMAC(iface),
			Channel:   channel,
			SSID:      ssid,
			StartedAt: time.Now(),
		},
	}
}

func (b *Backend) Name() string {
	return "stub"
}

func (b *Backend) Start(context.Context) error {
	return nil
}

func (b *Backend) Update(payload protocol.HubPayload) error {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.lastPayload = payload
	return nil
}

func (b *Backend) SetTelemetryHandler(func(radio.Telemetry)) {
}

func (b *Backend) State() radio.State {
	b.mu.RLock()
	defer b.mu.RUnlock()
	return b.state
}

func interfaceMAC(iface string) string {
	netIf, err := net.InterfaceByName(iface)
	if err != nil || len(netIf.HardwareAddr) == 0 {
		return ""
	}
	return strings.ToUpper(netIf.HardwareAddr.String())
}
