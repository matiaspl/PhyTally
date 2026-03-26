package hostapd

import (
	"context"
	"encoding/hex"
	"fmt"
	"net"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/mstarzak/phytally/phytally-hub/internal/protocol"
	"github.com/mstarzak/phytally/phytally-hub/internal/radio"
)

type Config struct {
	Interface        string
	Channel          int
	SSID             string
	BinaryPath       string
	CLIPath          string
	ControlDir       string
	ConfigPath       string
	MonitorInterface string
	IWPath           string
	IPPath           string
}

type commandRunner interface {
	Run(ctx context.Context, name string, args ...string) error
}

type execRunner struct{}

func (execRunner) Run(ctx context.Context, name string, args ...string) error {
	cmd := exec.CommandContext(ctx, name, args...)
	output, err := cmd.CombinedOutput()
	if err != nil {
		if len(output) == 0 {
			return err
		}
		return fmt.Errorf("%w: %s", err, strings.TrimSpace(string(output)))
	}
	return nil
}

type Backend struct {
	mu               sync.RWMutex
	cfg              Config
	state            radio.State
	runner           commandRunner
	runCtx           context.Context
	lastPayload      protocol.HubPayload
	started          bool
	telemetryHandler func(radio.Telemetry)
}

func New(cfg Config) *Backend {
	return &Backend{
		cfg: cfg,
		state: radio.State{
			Interface: cfg.Interface,
			MAC:       interfaceMAC(cfg.Interface),
			Channel:   cfg.Channel,
			SSID:      cfg.SSID,
			StartedAt: time.Now(),
		},
		runner: execRunner{},
	}
}

func (b *Backend) Name() string {
	return "hostapd"
}

func (b *Backend) SetTelemetryHandler(handler func(radio.Telemetry)) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.telemetryHandler = handler
}

func (b *Backend) State() radio.State {
	b.mu.RLock()
	defer b.mu.RUnlock()
	return b.state
}

func (b *Backend) Start(ctx context.Context) error {
	b.mu.Lock()
	if b.runCtx == nil || b.runCtx.Err() != nil {
		b.runCtx = ctx
	}
	if b.started {
		b.mu.Unlock()
		return nil
	}

	if err := os.MkdirAll(filepath.Dir(b.cfg.ConfigPath), 0o755); err != nil {
		b.mu.Unlock()
		return err
	}
	if err := os.MkdirAll(b.cfg.ControlDir, 0o755); err != nil {
		b.mu.Unlock()
		return err
	}

	if err := os.WriteFile(b.cfg.ConfigPath, []byte(b.renderConfigLocked()), 0o644); err != nil {
		b.mu.Unlock()
		return err
	}

	if err := b.runner.Run(ctx, b.cfg.BinaryPath, "-B", b.cfg.ConfigPath); err != nil {
		b.mu.Unlock()
		return err
	}

	b.started = true
	b.state.StartedAt = time.Now()
	b.mu.Unlock()

	if err := b.startTelemetryCapture(ctx); err != nil {
		return err
	}

	go func() {
		<-ctx.Done()
		_ = b.stop(context.Background())
	}()

	return nil
}

func (b *Backend) PrepareScan(ctx context.Context) error {
	return b.stop(ctx)
}

func (b *Backend) FinishScan(ctx context.Context) error {
	return b.Start(b.lifecycleContext(ctx))
}

func (b *Backend) Update(payload protocol.HubPayload) error {
	b.mu.Lock()
	b.lastPayload = payload
	started := b.started
	b.mu.Unlock()

	if !started {
		return nil
	}

	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()

	hexValue := vendorElementsHex(payload)
	if err := b.runner.Run(ctx, b.cfg.CLIPath, "-p", b.cfg.ControlDir, "-i", b.cfg.Interface, "set", "vendor_elements", hexValue); err != nil {
		return err
	}
	if err := b.runner.Run(ctx, b.cfg.CLIPath, "-p", b.cfg.ControlDir, "-i", b.cfg.Interface, "update_beacon"); err != nil {
		return err
	}
	return nil
}

func (b *Backend) stop(ctx context.Context) error {
	b.mu.Lock()
	if !b.started {
		b.mu.Unlock()
		return nil
	}
	b.started = false
	b.mu.Unlock()

	_ = b.stopTelemetryCapture(ctx)
	return b.runner.Run(ctx, b.cfg.CLIPath, "-p", b.cfg.ControlDir, "-i", b.cfg.Interface, "terminate")
}

func (b *Backend) emitTelemetry(telem radio.Telemetry) {
	b.mu.RLock()
	handler := b.telemetryHandler
	b.mu.RUnlock()
	if handler != nil {
		handler(telem)
	}
}

func (b *Backend) lifecycleContext(fallback context.Context) context.Context {
	b.mu.RLock()
	defer b.mu.RUnlock()
	if b.runCtx != nil {
		return b.runCtx
	}
	return fallback
}

func (b *Backend) renderConfigLocked() string {
	return strings.Join([]string{
		fmt.Sprintf("interface=%s", b.cfg.Interface),
		"driver=nl80211",
		fmt.Sprintf("ssid=%s", b.cfg.SSID),
		"hw_mode=g",
		fmt.Sprintf("channel=%d", b.cfg.Channel),
		"beacon_int=100",
		"auth_algs=1",
		"ignore_broadcast_ssid=1",
		fmt.Sprintf("ctrl_interface=%s", b.cfg.ControlDir),
		"ctrl_interface_group=0",
		fmt.Sprintf("vendor_elements=%s", vendorElementsHex(b.lastPayload)),
		"",
	}, "\n")
}

func vendorElementsHex(payload protocol.HubPayload) string {
	buf := make([]byte, 0, 2+len(payload.States)+12)
	buf = append(buf, 0xDD, byte(len(protocolBytes(payload))))
	buf = append(buf, protocolBytes(payload)...)
	return hex.EncodeToString(buf)
}

func protocolBytes(payload protocol.HubPayload) []byte {
	buf := make([]byte, 0, 29)
	buf = append(buf, payload.OUI[:]...)
	buf = append(buf, payload.ProtocolVersion)
	buf = append(buf, payload.States[:]...)
	buf = append(buf, payload.TargetMAC[:]...)
	buf = append(buf, payload.CommandType, payload.CommandParam, payload.CommandID)
	return buf
}

func interfaceMAC(iface string) string {
	netIf, err := net.InterfaceByName(iface)
	if err != nil || len(netIf.HardwareAddr) == 0 {
		return ""
	}
	return strings.ToUpper(netIf.HardwareAddr.String())
}
