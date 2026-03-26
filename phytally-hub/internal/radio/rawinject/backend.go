package rawinject

import (
	"context"
	"encoding/binary"
	"fmt"
	"net"
	"strings"
	"sync"
	"time"

	"github.com/mstarzak/phytally/phytally-hub/internal/protocol"
	"github.com/mstarzak/phytally/phytally-hub/internal/radio"
)

type Config struct {
	Interface        string
	MonitorInterface string
	Channel          int
	SSID             string
	TxRate500Kbps    int
	IWPath           string
	IPPath           string
}

type commandRunner interface {
	Run(ctx context.Context, name string, args ...string) error
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
	sequence         uint16
	socketInterface  string
	txFD             int
	rxFD             int
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
		txFD:   -1,
		rxFD:   -1,
	}
}

func (b *Backend) Name() string {
	return "rawinject"
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
	b.mu.Unlock()

	if err := b.startMonitorInterface(ctx); err != nil {
		return err
	}

	socketIface := b.socketInterfaceName()

	txFD, txAddr, err := socketForInterface(socketIface)
	if err != nil {
		return err
	}
	rxFD, _, err := socketForInterface(socketIface)
	if err != nil {
		_ = closeSocket(txFD)
		return err
	}

	b.mu.Lock()
	b.started = true
	b.state.StartedAt = time.Now()
	b.txFD = txFD
	b.rxFD = rxFD
	b.mu.Unlock()

	go b.txLoop(ctx, txFD, txAddr)
	go b.captureLoop(ctx, rxFD)
	go func() {
		<-ctx.Done()
		b.closeSockets()
		_ = b.stop(context.Background())
	}()

	return nil
}

func (b *Backend) PrepareScan(ctx context.Context) error {
	if err := b.stop(ctx); err != nil {
		return err
	}
	if b.cfg.MonitorInterface != "" && b.cfg.MonitorInterface != b.cfg.Interface {
		_ = b.runner.Run(ctx, b.cfg.IPPath, "link", "set", b.cfg.MonitorInterface, "down")
		_ = b.runner.Run(ctx, b.cfg.IWPath, "dev", b.cfg.MonitorInterface, "del")
	}
	return nil
}

func (b *Backend) FinishScan(ctx context.Context) error {
	return b.Start(b.lifecycleContext(ctx))
}

func (b *Backend) Update(payload protocol.HubPayload) error {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.lastPayload = payload
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

	b.closeSockets()
	return b.stopMonitorInterface(ctx)
}

func (b *Backend) closeSockets() {
	b.mu.Lock()
	defer b.mu.Unlock()

	if b.txFD >= 0 {
		_ = closeSocket(b.txFD)
		b.txFD = -1
	}
	if b.rxFD >= 0 {
		_ = closeSocket(b.rxFD)
		b.rxFD = -1
	}
}

func (b *Backend) txLoop(ctx context.Context, fd int, addr *sockAddr) {
	ticker := time.NewTicker(time.Duration(protocol.BeaconIntervalMS) * time.Millisecond)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			packets, err := b.currentPackets()
			if err != nil {
				continue
			}
			for _, packet := range packets {
				_ = sendPacket(fd, addr, packet)
			}
		}
	}
}

func (b *Backend) captureLoop(ctx context.Context, fd int) {
	buf := make([]byte, 4096)
	for {
		select {
		case <-ctx.Done():
			return
		default:
		}

		n, err := recvPacket(fd, buf)
		if err != nil {
			if shouldIgnoreRecvError(err) {
				continue
			}
			return
		}

		if telem, ok := parseTelemetryPacket(buf[:n], time.Now()); ok {
			b.emitTelemetry(telem)
		}
	}
}

func (b *Backend) currentPackets() ([][]byte, error) {
	b.mu.Lock()
	defer b.mu.Unlock()

	mac, err := parseMACString(b.state.MAC)
	if err != nil {
		return nil, err
	}

	b.sequence++
	cfg := frameConfig{
		SourceMAC:      mac,
		BSSID:          mac,
		SSID:           b.cfg.SSID,
		Channel:        b.cfg.Channel,
		Rate500Kbps:    b.cfg.TxRate500Kbps,
		Sequence:       b.sequence,
		BeaconInterval: protocol.BeaconIntervalMS,
		Payload:        b.lastPayload,
	}
	return [][]byte{
		buildMgmtPacket(0x80, cfg),
		buildMgmtPacket(0x50, cfg),
	}, nil
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

func interfaceMAC(iface string) string {
	netIf, err := net.InterfaceByName(iface)
	if err != nil || len(netIf.HardwareAddr) == 0 {
		return ""
	}
	return strings.ToUpper(netIf.HardwareAddr.String())
}

func parseMACString(value string) ([6]byte, error) {
	var out [6]byte
	hw, err := net.ParseMAC(value)
	if err != nil {
		return out, err
	}
	if len(hw) < len(out) {
		return out, fmt.Errorf("short mac address")
	}
	copy(out[:], hw[:6])
	return out, nil
}

func (b *Backend) socketInterfaceName() string {
	if b.socketInterface != "" {
		return b.socketInterface
	}
	if b.cfg.MonitorInterface != "" {
		return b.cfg.MonitorInterface
	}
	return b.cfg.Interface
}

type frameConfig struct {
	SourceMAC      [6]byte
	BSSID          [6]byte
	SSID           string
	Channel        int
	Rate500Kbps    int
	Sequence       uint16
	BeaconInterval int
	Payload        protocol.HubPayload
}

func buildMgmtPacket(frameControl byte, cfg frameConfig) []byte {
	body := buildMgmtFrame(frameControl, cfg)
	rtap := []byte{
		0x00, 0x00,
		0x09, 0x00,
		0x04, 0x00, 0x00, 0x00,
		byte(cfg.Rate500Kbps),
	}

	packet := make([]byte, 0, len(rtap)+len(body))
	packet = append(packet, rtap...)
	packet = append(packet, body...)
	return packet
}

func buildMgmtFrame(frameControl byte, cfg frameConfig) []byte {
	frame := make([]byte, 0, 128)

	frame = append(frame, frameControl, 0x00)
	frame = append(frame, 0x00, 0x00)
	frame = append(frame, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff)
	frame = append(frame, cfg.SourceMAC[:]...)
	frame = append(frame, cfg.BSSID[:]...)

	seqControl := cfg.Sequence << 4
	frame = binary.LittleEndian.AppendUint16(frame, seqControl)

	frame = binary.LittleEndian.AppendUint64(frame, uint64(time.Now().UnixMicro()))
	frame = binary.LittleEndian.AppendUint16(frame, uint16(cfg.BeaconInterval))
	frame = binary.LittleEndian.AppendUint16(frame, 0x0001)

	frame = append(frame, 0x00, 0x00)
	frame = append(frame, 0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24)
	frame = append(frame, 0x03, 0x01, byte(cfg.Channel))

	vendor := vendorElementBytes(cfg.Payload)
	frame = append(frame, vendor...)
	return frame
}

func vendorElementBytes(payload protocol.HubPayload) []byte {
	body := protocolBytes(payload)
	out := make([]byte, 0, 2+len(body))
	out = append(out, 0xdd, byte(len(body)))
	out = append(out, body...)
	return out
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
