package hub

import (
	"context"
	"fmt"
	"net"
	"strings"
	"sync"
	"time"

	"github.com/mstarzak/phytally/phytally-hub/internal/config"
	"github.com/mstarzak/phytally/phytally-hub/internal/protocol"
	"github.com/mstarzak/phytally/phytally-hub/internal/radio"
	"github.com/mstarzak/phytally/phytally-hub/internal/switcher"
	"github.com/mstarzak/phytally/phytally-hub/internal/wifisurvey"
)

type Receiver struct {
	MAC               string `json:"mac"`
	TallyID           int    `json:"tally_id"`
	Online            bool   `json:"online"`
	BatteryPercent    int    `json:"battery_percent"`
	BrightnessPercent int    `json:"brightness_percent"`
	HubRSSI           int    `json:"hub_rssi"`
	LastSeen          time.Time
}

type Command struct {
	Status            string
	Active            bool
	Kind              string
	CommandID         int
	TargetMAC         string
	TallyID           int
	BrightnessPercent int
	ExpiresAt         time.Time
}

type TelemetrySnapshot struct {
	PacketCount         uint64
	LastReceiverMAC     string
	LastReceiverID      int
	LastSeen            time.Time
	LastReportedHubRSSI int
	LastAirRSSI         int
	HaveLastReceiver    bool
}

type SwitcherConnectionState struct {
	Connected  bool
	LastError  string
	LastUpdate time.Time
}

type UpdateEvent struct {
	Status bool
	Survey bool
}

var supportedWirelessTransports = []string{"stub", "hostapd", "rawinject"}

type Hub struct {
	mu           sync.RWMutex
	cfg          config.Config
	configPath   string
	radioBackend radio.Backend
	radioFactory func(config.Config) (radio.Backend, error)
	runCtx       context.Context

	simulationEnabled bool
	lastSurveyAt      int64
	tallies           [protocol.MaxTallies]protocol.TallyState
	receivers         map[string]*Receiver
	command           Command
	telemetry         TelemetrySnapshot
	survey            wifisurvey.Status
	lastSimStep       time.Time
	simStep           int
	nextCommandID     byte
	switchers         map[string]SwitcherConnectionState
	switcherCancel    context.CancelFunc
	radioCancel       context.CancelFunc
	subscribers       map[int]chan UpdateEvent
	nextSubscriberID  int
}

const wifiSurveyExpectedDuration = 26 * time.Second

func New(cfg config.Config, configPath string, radioBackend radio.Backend) *Hub {
	return NewWithFactory(cfg, configPath, radioBackend, nil)
}

func NewWithFactory(cfg config.Config, configPath string, radioBackend radio.Backend, radioFactory func(config.Config) (radio.Backend, error)) *Hub {
	h := &Hub{
		cfg:               cfg,
		configPath:        configPath,
		radioBackend:      radioBackend,
		radioFactory:      radioFactory,
		receivers:         make(map[string]*Receiver),
		simulationEnabled: !switcher.HasAnyEnabled(cfg),
		switchers:         make(map[string]SwitcherConnectionState),
		command: Command{
			Status: "None",
		},
		survey: wifisurvey.Status{
			Networks: []wifisurvey.Network{},
		},
		subscribers: make(map[int]chan UpdateEvent),
	}
	h.applySimulationStepLocked()
	return h
}

func (h *Hub) Start(ctx context.Context) error {
	h.mu.Lock()
	h.runCtx = ctx
	h.mu.Unlock()
	if err := h.reloadRadio(); err != nil {
		return err
	}
	h.reloadSwitchers()

	ticker := time.NewTicker(time.Second)
	go func() {
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				h.tick()
			}
		}
	}()
	return nil
}

func (h *Hub) StaticDir() string {
	h.mu.RLock()
	defer h.mu.RUnlock()
	return h.cfg.StaticDir
}

func (h *Hub) Discovery() map[string]any {
	h.mu.RLock()
	backend := h.radioBackend
	h.mu.RUnlock()

	return map[string]any{
		"name":               "PhyTally Hub API",
		"version":            "v1",
		"dashboard":          "/",
		"status":             "/api/v1/status",
		"events":             "/api/v1/events",
		"config":             "/api/v1/config",
		"wireless_transport": "/api/v1/wireless/transport",
		"wifi_survey":        "/api/v1/wifi/survey",
		"simulation":         "/api/v1/simulation",
		"set_tally":          "/api/v1/tallies/set",
		"receivers":          "/api/v1/receivers",
		"assign_id":          "/api/v1/receivers/assign-id",
		"set_brightness":     "/api/v1/receivers/set-brightness",
		"identify_receiver":  "/api/v1/receivers/identify",
		"radio":              backend.Name(),
	}
}

func (h *Hub) Status() map[string]any {
	h.mu.RLock()
	defer h.mu.RUnlock()

	radioState := h.radioBackend.State()
	wifiIP := interfaceIPv4(h.cfg.EthernetInterface)

	tallies := make([]int, len(h.tallies))
	for i, state := range h.tallies {
		tallies[i] = int(state)
	}

	receivers := make([]map[string]any, 0, len(h.receivers))
	now := time.Now()
	for _, receiver := range h.receivers {
		receivers = append(receivers, map[string]any{
			"mac":                receiver.MAC,
			"tally_id":           receiver.TallyID,
			"online":             receiver.Online,
			"battery_percent":    receiver.BatteryPercent,
			"brightness_percent": receiver.BrightnessPercent,
			"hub_rssi":           receiver.HubRSSI,
			"last_seen_ms_ago":   maxInt64(0, now.Sub(receiver.LastSeen).Milliseconds()),
		})
	}

	command := map[string]any{
		"status":             h.command.Status,
		"active":             h.command.Active,
		"type":               h.command.Kind,
		"command_id":         0,
		"target_mac":         nil,
		"tally_id":           nil,
		"brightness_percent": nil,
		"expires_in_ms":      0,
	}
	if h.command.Active {
		command["command_id"] = h.command.CommandID
		command["target_mac"] = h.command.TargetMAC
		if h.command.Kind == "set_id" {
			command["tally_id"] = h.command.TallyID
		}
		if h.command.Kind == "set_brightness" {
			command["brightness_percent"] = h.command.BrightnessPercent
		}
		command["expires_in_ms"] = maxInt64(0, h.command.ExpiresAt.Sub(now).Milliseconds())
	}

	telemetry := map[string]any{
		"packet_count":  h.telemetry.PacketCount,
		"last_receiver": nil,
	}
	if h.telemetry.HaveLastReceiver {
		telemetry["last_receiver"] = map[string]any{
			"mac":                        h.telemetry.LastReceiverMAC,
			"tally_id":                   h.telemetry.LastReceiverID,
			"age_ms":                     maxInt64(0, now.Sub(h.telemetry.LastSeen).Milliseconds()),
			"receiver_reported_hub_rssi": h.telemetry.LastReportedHubRSSI,
			"air_rssi":                   h.telemetry.LastAirRSSI,
		}
	}

	return map[string]any{
		"mode": map[string]any{
			"config": false,
		},
		"simulation": map[string]any{
			"enabled": h.simulationEnabled,
		},
		"network": map[string]any{
			"wifi_connected":     false,
			"wifi_ip":            "",
			"uplink_ip":          wifiIP,
			"ap_ssid":            radioState.SSID,
			"ap_mac":             radioState.MAC,
			"radio_channel":      radioState.Channel,
			"wireless_transport": h.cfg.RadioBackend,
		},
		"wireless": h.wirelessTransportStatusLocked(),
		"switchers": map[string]any{
			"atem": map[string]any{
				"enabled":    h.cfg.AtemEnabled,
				"ip":         h.cfg.AtemIP,
				"connected":  h.switchers["atem"].Connected,
				"last_error": h.switchers["atem"].LastError,
			},
			"vmix": map[string]any{
				"enabled":    h.cfg.VMixEnabled,
				"ip":         h.cfg.VMixIP,
				"connected":  h.switchers["vmix"].Connected,
				"last_error": h.switchers["vmix"].LastError,
			},
			"tsl": map[string]any{
				"enabled":     h.cfg.TSLEnabled,
				"listen_addr": h.cfg.TSLListenAddr,
				"connected":   h.switchers["tsl"].Connected,
				"last_error":  h.switchers["tsl"].LastError,
			},
			"obs": map[string]any{
				"enabled":      h.cfg.OBSEnabled,
				"url":          h.cfg.OBSURL,
				"password_set": h.cfg.OBSPassword != "",
				"connected":    h.switchers["obs"].Connected,
				"last_error":   h.switchers["obs"].LastError,
			},
		},
		"telemetry": telemetry,
		"command":   command,
		"tallies":   tallies,
		"receivers": receivers,
	}
}

func (h *Hub) Config() map[string]any {
	h.mu.RLock()
	defer h.mu.RUnlock()

	return map[string]any{
		"last_survey_at_ms":  h.lastSurveyAt,
		"wireless_transport": h.cfg.RadioBackend,
		"radio_channel":      h.cfg.RadioChannel,
		"atem_ip":            h.cfg.AtemIP,
		"atem_enabled":       h.cfg.AtemEnabled,
		"vmix_ip":            h.cfg.VMixIP,
		"vmix_enabled":       h.cfg.VMixEnabled,
		"tsl_listen_addr":    h.cfg.TSLListenAddr,
		"tsl_enabled":        h.cfg.TSLEnabled,
		"obs_url":            h.cfg.OBSURL,
		"obs_password_set":   h.cfg.OBSPassword != "",
		"obs_enabled":        h.cfg.OBSEnabled,
	}
}

func (h *Hub) UpdateConfig(update map[string]any) error {
	h.mu.Lock()
	previousCfg := h.cfg

	if value, ok := update["wireless_transport"].(string); ok && value != "" {
		value = strings.TrimSpace(strings.ToLower(value))
		if !isSupportedWirelessTransport(value) {
			h.mu.Unlock()
			return fmt.Errorf("unsupported wireless transport %q", value)
		}
		h.cfg.RadioBackend = value
	}
	if value, ok := update["radio_channel"]; ok {
		channel, valid := configInt(value)
		if valid && channel >= 1 && channel <= 13 {
			h.cfg.RadioChannel = channel
		}
	}
	if value, ok := update["atem_ip"].(string); ok {
		h.cfg.AtemIP = value
	}
	if value, ok := update["vmix_ip"].(string); ok {
		h.cfg.VMixIP = value
	}
	if value, ok := update["atem_enabled"].(bool); ok {
		h.cfg.AtemEnabled = value
	}
	if value, ok := update["vmix_enabled"].(bool); ok {
		h.cfg.VMixEnabled = value
	}
	if value, ok := update["tsl_listen_addr"].(string); ok {
		h.cfg.TSLListenAddr = value
	}
	if value, ok := update["tsl_enabled"].(bool); ok {
		h.cfg.TSLEnabled = value
	}
	if value, ok := update["obs_url"].(string); ok {
		h.cfg.OBSURL = value
	}
	if value, ok := update["obs_password"].(string); ok {
		h.cfg.OBSPassword = value
	}
	if value, ok := update["obs_enabled"].(bool); ok {
		h.cfg.OBSEnabled = value
	}

	saveCfg := h.cfg
	h.mu.Unlock()

	if err := config.Save(h.configPath, saveCfg); err != nil {
		return err
	}

	if radioConfigChanged(previousCfg, saveCfg) {
		if err := h.reloadRadio(); err != nil {
			return err
		}
	}
	h.reloadSwitchers()
	h.notifyStatusChanged()
	return nil
}

func (h *Hub) WirelessTransport() map[string]any {
	h.mu.RLock()
	defer h.mu.RUnlock()
	return h.wirelessTransportStatusLocked()
}

func (h *Hub) SetWirelessTransport(transport string) error {
	transport = strings.TrimSpace(strings.ToLower(transport))
	if transport == "" {
		return fmt.Errorf("wireless transport is required")
	}
	if !isSupportedWirelessTransport(transport) {
		return fmt.Errorf("unsupported wireless transport %q", transport)
	}

	h.mu.Lock()
	if h.cfg.RadioBackend == transport {
		h.mu.Unlock()
		return nil
	}
	h.cfg.RadioBackend = transport
	saveCfg := h.cfg
	h.mu.Unlock()

	if err := config.Save(h.configPath, saveCfg); err != nil {
		return err
	}
	if err := h.reloadRadio(); err != nil {
		return err
	}
	h.notifyStatusChanged()
	return nil
}

func (h *Hub) wirelessTransportStatusLocked() map[string]any {
	supported := make([]string, len(supportedWirelessTransports))
	copy(supported, supportedWirelessTransports)
	return map[string]any{
		"transport":            h.cfg.RadioBackend,
		"supported_transports": supported,
	}
}

func isSupportedWirelessTransport(transport string) bool {
	for _, candidate := range supportedWirelessTransports {
		if candidate == transport {
			return true
		}
	}
	return false
}

func (h *Hub) reloadSwitchers() {
	h.mu.Lock()
	parentCtx := h.runCtx
	cfg := h.cfg
	previousCancel := h.switcherCancel
	h.switcherCancel = nil
	h.switchers = make(map[string]SwitcherConnectionState)
	if switcher.HasAnyEnabled(cfg) {
		h.simulationEnabled = false
	}
	h.mu.Unlock()

	if previousCancel != nil {
		previousCancel()
	}
	if parentCtx == nil || !switcher.HasAnyEnabled(cfg) {
		h.notifyStatusChanged()
		return
	}

	switcherCtx, cancel := context.WithCancel(parentCtx)
	h.mu.Lock()
	h.switcherCancel = cancel
	h.mu.Unlock()

	switcher.StartEnabled(switcherCtx, cfg, h.ApplyExternalTallies, h.ReportSwitcherStatus)
	h.notifyStatusChanged()
}

func (h *Hub) reloadRadio() error {
	h.mu.Lock()
	parentCtx := h.runCtx
	cfg := h.cfg
	factory := h.radioFactory
	previousCancel := h.radioCancel
	previousBackend := h.radioBackend
	h.radioCancel = nil
	h.mu.Unlock()

	newBackend := previousBackend
	var err error
	if factory != nil {
		newBackend, err = factory(cfg)
		if err != nil {
			return err
		}
	}
	if newBackend == nil {
		return fmt.Errorf("radio backend is not configured")
	}

	if previousCancel != nil {
		previousCancel()
		time.Sleep(300 * time.Millisecond)
	}

	if parentCtx != nil {
		radioCtx, cancel := context.WithCancel(parentCtx)
		newBackend.SetTelemetryHandler(h.HandleTelemetry)
		if err := newBackend.Start(radioCtx); err != nil {
			cancel()
			return err
		}
		h.mu.Lock()
		h.radioBackend = newBackend
		h.radioCancel = cancel
		h.mu.Unlock()
	} else {
		h.mu.Lock()
		h.radioBackend = newBackend
		h.mu.Unlock()
	}

	h.notifyStatusChanged()
	return h.pushRadioPayload()
}

func (h *Hub) SetSimulation(enabled bool) {
	h.mu.Lock()
	h.simulationEnabled = enabled
	if !enabled {
		for i := range h.tallies {
			h.tallies[i] = protocol.TallyOff
		}
		h.mu.Unlock()
		h.notifyStatusChanged()
		_ = h.pushRadioPayload()
		return
	}
	h.applySimulationStepLocked()
	h.mu.Unlock()
	h.notifyStatusChanged()
	_ = h.pushRadioPayload()
}

func (h *Hub) SetSingleTally(tallyID int, enabled bool) error {
	if tallyID < 1 || tallyID > protocol.MaxTallies {
		return fmt.Errorf("tally_id must be between 1 and %d", protocol.MaxTallies)
	}

	h.mu.Lock()
	h.simulationEnabled = false
	if enabled {
		h.tallies[tallyID-1] = protocol.TallyProgram
	} else {
		h.tallies[tallyID-1] = protocol.TallyOff
	}
	h.mu.Unlock()
	h.notifyStatusChanged()

	return h.pushRadioPayload()
}

func (h *Hub) QueueAssignID(mac string, tallyID int) error {
	if tallyID < 1 || tallyID > protocol.MaxTallies {
		return fmt.Errorf("tally_id must be between 1 and %d", protocol.MaxTallies)
	}
	normalized, _, err := normalizeMAC(mac)
	if err != nil {
		return fmt.Errorf("invalid mac address")
	}

	h.mu.Lock()
	commandID := int(h.nextCommandIDLocked())
	h.command = Command{
		Status:    fmt.Sprintf("Active: SET_ID=%d for %s", tallyID, normalized),
		Active:    true,
		Kind:      "set_id",
		CommandID: commandID,
		TargetMAC: normalized,
		TallyID:   tallyID,
		ExpiresAt: time.Now().Add(10 * time.Second),
	}
	h.mu.Unlock()
	h.notifyStatusChanged()

	if err := h.pushRadioPayload(); err != nil {
		return err
	}
	return nil
}

func (h *Hub) QueueSetBrightness(mac string, brightnessPercent int) error {
	if brightnessPercent < 0 || brightnessPercent > 100 {
		return fmt.Errorf("brightness_percent must be between 0 and 100")
	}
	normalized, _, err := normalizeMAC(mac)
	if err != nil {
		return fmt.Errorf("invalid mac address")
	}

	h.mu.Lock()
	commandID := int(h.nextCommandIDLocked())
	h.command = Command{
		Status:            fmt.Sprintf("Active: BRIGHTNESS=%d%% for %s", brightnessPercent, normalized),
		Active:            true,
		Kind:              "set_brightness",
		CommandID:         commandID,
		TargetMAC:         normalized,
		BrightnessPercent: brightnessPercent,
		ExpiresAt:         time.Now().Add(4 * time.Second),
	}
	h.mu.Unlock()
	h.notifyStatusChanged()

	if err := h.pushRadioPayload(); err != nil {
		return err
	}
	return nil
}

func (h *Hub) QueueIdentify(mac string) error {
	normalized, _, err := normalizeMAC(mac)
	if err != nil {
		return fmt.Errorf("invalid mac address")
	}

	h.mu.Lock()
	commandID := int(h.nextCommandIDLocked())
	h.command = Command{
		Status:    fmt.Sprintf("Active: IDENTIFY for %s", normalized),
		Active:    true,
		Kind:      "identify",
		CommandID: commandID,
		TargetMAC: normalized,
		ExpiresAt: time.Now().Add(4 * time.Second),
	}
	h.mu.Unlock()
	h.notifyStatusChanged()

	if err := h.pushRadioPayload(); err != nil {
		return err
	}
	return nil
}

func (h *Hub) Receivers() map[string]any {
	status := h.Status()
	return map[string]any{
		"receivers": status["receivers"],
	}
}

func (h *Hub) WiFiSurvey() map[string]any {
	h.mu.RLock()
	defer h.mu.RUnlock()
	return map[string]any{
		"scanned_at_ms":         h.survey.ScannedAtMS,
		"scan_started_at_ms":    h.survey.ScanStartedAtMS,
		"estimated_duration_ms": h.survey.EstimatedDurationMS,
		"scanning":              h.survey.Scanning,
		"scan_mode":             h.survey.ScanMode,
		"network_count":         h.survey.NetworkCount,
		"error":                 h.survey.Error,
		"warnings":              h.survey.Warnings,
		"channel_suggestion":    h.survey.ChannelSuggestion,
		"spectral_channels":     h.survey.SpectralChannels,
		"spectral_rows":         h.survey.SpectralRows,
		"networks":              h.survey.Networks,
	}
}

func (h *Hub) StartWiFiSurvey() error {
	h.mu.Lock()
	if h.survey.Scanning {
		h.mu.Unlock()
		return nil
	}

	startedAt := time.Now().UnixMilli()
	h.survey = wifisurvey.Status{
		ScannedAtMS:         h.lastSurveyAt,
		ScanStartedAtMS:     startedAt,
		EstimatedDurationMS: wifiSurveyExpectedDuration.Milliseconds(),
		Scanning:            true,
		NetworkCount:        0,
		ChannelSuggestion:   nil,
		Networks:            []wifisurvey.Network{},
	}
	cfg := h.cfg
	h.mu.Unlock()
	h.notifySurveyChanged()

	go func() {
		ctx, cancel := context.WithTimeout(context.Background(), 28*time.Second)
		defer cancel()

		var coordinator radio.SurveyCoordinator
		if candidate, ok := h.radioBackend.(radio.SurveyCoordinator); ok {
			coordinator = candidate
			if err := coordinator.PrepareScan(ctx); err != nil {
				h.mu.Lock()
				h.survey = wifisurvey.Status{
					ScannedAtMS:         h.lastSurveyAt,
					ScanStartedAtMS:     startedAt,
					EstimatedDurationMS: wifiSurveyExpectedDuration.Milliseconds(),
					Scanning:            false,
					NetworkCount:        0,
					Error:               err.Error(),
					ChannelSuggestion:   nil,
					Networks:            []wifisurvey.Network{},
				}
				h.mu.Unlock()
				h.notifySurveyChanged()
				return
			}
		}

		status, err := wifisurvey.Scanner{
			IWPath:         cfg.IWPath,
			IPPath:         cfg.IPPath,
			RadioInterface: cfg.RadioInterface,
			DebugFSRoot:    cfg.DebugFSRoot,
		}.Scan(ctx, "")

		var restoreErr error
		if coordinator != nil {
			restoreCtx, restoreCancel := context.WithTimeout(context.Background(), 10*time.Second)
			restoreErr = coordinator.FinishScan(restoreCtx)
			restoreCancel()
		}
		if err == nil && restoreErr == nil {
			restoreErr = h.pushRadioPayload()
		}

		h.mu.Lock()
		if err != nil || restoreErr != nil {
			errorText := ""
			switch {
			case err != nil && restoreErr != nil:
				errorText = fmt.Sprintf("survey failed: %v; radio restore failed: %v", err, restoreErr)
			case err != nil:
				errorText = err.Error()
			default:
				errorText = fmt.Sprintf("radio restore failed: %v", restoreErr)
			}
			h.survey = wifisurvey.Status{
				ScannedAtMS:         h.lastSurveyAt,
				ScanStartedAtMS:     startedAt,
				EstimatedDurationMS: wifiSurveyExpectedDuration.Milliseconds(),
				Scanning:            false,
				NetworkCount:        0,
				Error:               errorText,
				ChannelSuggestion:   nil,
				Networks:            []wifisurvey.Network{},
			}
			h.mu.Unlock()
			h.notifySurveyChanged()
			return
		}

		h.lastSurveyAt = time.Now().UnixMilli()
		status.ScannedAtMS = h.lastSurveyAt
		status.ScanStartedAtMS = startedAt
		status.EstimatedDurationMS = wifiSurveyExpectedDuration.Milliseconds()
		status.Scanning = false
		h.survey = status
		h.mu.Unlock()
		h.notifySurveyChanged()
		return
	}()

	return nil
}

func (h *Hub) tick() {
	h.mu.Lock()
	now := time.Now()
	needsPush := false
	statusChanged := false

	if h.command.Active && now.After(h.command.ExpiresAt) {
		h.command.Active = false
		h.command.Status = "Expired"
		needsPush = true
		statusChanged = true
	}

	if h.simulationEnabled && now.Sub(h.lastSimStep) >= time.Second {
		h.simStep++
		h.applySimulationStepLocked()
		needsPush = true
		statusChanged = true
	}

	for mac, receiver := range h.receivers {
		if now.Sub(receiver.LastSeen) > time.Minute {
			if receiver.Online {
				statusChanged = true
			}
			receiver.Online = false
		}
		if now.Sub(receiver.LastSeen) > 10*time.Minute && !receiver.Online {
			delete(h.receivers, mac)
			statusChanged = true
		}
	}
	h.mu.Unlock()

	if needsPush {
		_ = h.pushRadioPayload()
	}
	if statusChanged {
		h.notifyStatusChanged()
	}
}

func (h *Hub) applySimulationStepLocked() {
	h.lastSimStep = time.Now()
	state := protocol.TallyOff
	switch h.simStep % 3 {
	case 1:
		state = protocol.TallyPreview
	case 2:
		state = protocol.TallyProgram
	}
	for i := range h.tallies {
		h.tallies[i] = state
	}
}

func interfaceIPv4(iface string) string {
	netIf, err := net.InterfaceByName(iface)
	if err != nil {
		return ""
	}
	addrs, err := netIf.Addrs()
	if err != nil {
		return ""
	}
	for _, addr := range addrs {
		ipNet, ok := addr.(*net.IPNet)
		if !ok || ipNet.IP == nil {
			continue
		}
		ip := ipNet.IP.To4()
		if ip != nil {
			return ip.String()
		}
	}
	return ""
}

func configInt(value any) (int, bool) {
	switch v := value.(type) {
	case int:
		return v, true
	case int32:
		return int(v), true
	case int64:
		return int(v), true
	case float64:
		return int(v), true
	default:
		return 0, false
	}
}

func validMAC(value string) bool {
	_, err := net.ParseMAC(value)
	return err == nil
}

func normalizeMAC(value string) (string, []byte, error) {
	hw, err := net.ParseMAC(value)
	if err != nil {
		return "", nil, err
	}
	normalized := strings.ToUpper(hw.String())
	raw := make([]byte, len(hw))
	copy(raw, hw)
	return normalized, raw, nil
}

func maxInt64(a, b int64) int64 {
	if a > b {
		return a
	}
	return b
}

func radioConfigChanged(previous config.Config, current config.Config) bool {
	return previous.RadioBackend != current.RadioBackend ||
		previous.RadioInterface != current.RadioInterface ||
		previous.RadioChannel != current.RadioChannel ||
		previous.MonitorInterface != current.MonitorInterface ||
		previous.TxRate500Kbps != current.TxRate500Kbps ||
		previous.APSSID != current.APSSID ||
		previous.HostapdPath != current.HostapdPath ||
		previous.HostapdCLIPath != current.HostapdCLIPath ||
		previous.HostapdControlDir != current.HostapdControlDir ||
		previous.HostapdConfigPath != current.HostapdConfigPath ||
		previous.IWPath != current.IWPath ||
		previous.IPPath != current.IPPath
}

func NormalizeMAC(value string) string {
	normalized, _, err := normalizeMAC(value)
	if err != nil {
		return value
	}
	return normalized
}

func (h *Hub) HandleTelemetry(telem radio.Telemetry) {
	h.mu.Lock()
	needsPush := false

	receiver, ok := h.receivers[telem.SourceMAC]
	if !ok {
		receiver = &Receiver{
			MAC: telem.SourceMAC,
		}
		h.receivers[telem.SourceMAC] = receiver
	}

	receiver.MAC = telem.SourceMAC
	receiver.TallyID = telem.TallyID
	receiver.BatteryPercent = telem.BatteryPercent
	receiver.BrightnessPercent = telem.BrightnessPercent
	receiver.HubRSSI = telem.ReportedHubRSSI
	receiver.LastSeen = telem.ReceivedAt
	receiver.Online = true

	h.telemetry.PacketCount++
	h.telemetry.LastReceiverMAC = telem.SourceMAC
	h.telemetry.LastReceiverID = telem.TallyID
	h.telemetry.LastSeen = telem.ReceivedAt
	h.telemetry.LastReportedHubRSSI = telem.ReportedHubRSSI
	h.telemetry.LastAirRSSI = telem.AirRSSI
	h.telemetry.HaveLastReceiver = true

	if h.command.Active &&
		telem.SourceMAC == h.command.TargetMAC &&
		telem.AckCommandID != 0 &&
		telem.AckCommandID == h.command.CommandID {
		h.command.Active = false
		h.command.Status = fmt.Sprintf("Acknowledged: %s by %s", strings.ToUpper(h.command.Kind), telem.SourceMAC)
		needsPush = true
	}
	h.mu.Unlock()

	if needsPush {
		_ = h.pushRadioPayload()
	}
	h.notifyStatusChanged()
}

func (h *Hub) pushRadioPayload() error {
	h.mu.RLock()
	payload := h.currentPayloadLocked()
	backend := h.radioBackend
	h.mu.RUnlock()
	return backend.Update(payload)
}

func (h *Hub) currentPayloadLocked() protocol.HubPayload {
	var payload protocol.HubPayload
	payload.OUI = [3]byte{protocol.VendorOUI0, protocol.VendorOUI1, protocol.VendorOUI2}
	payload.ProtocolVersion = 0x01

	for i, state := range h.tallies {
		payload.States[i] = byte(state)
	}

	if h.command.Active {
		if _, raw, err := normalizeMAC(h.command.TargetMAC); err == nil {
			copy(payload.TargetMAC[:], raw)
			switch h.command.Kind {
			case "set_id":
				payload.CommandType = byte(protocol.CommandSetID)
				payload.CommandParam = byte(h.command.TallyID - 1)
			case "set_brightness":
				payload.CommandType = byte(protocol.CommandSetBrightness)
				payload.CommandParam = protocol.BrightnessPercentToByte(h.command.BrightnessPercent)
			case "identify":
				payload.CommandType = byte(protocol.CommandIdentify)
				payload.CommandParam = 0
			}
			payload.CommandID = byte(h.command.CommandID)
		}
	}

	return payload
}

func (h *Hub) nextCommandIDLocked() byte {
	h.nextCommandID++
	if h.nextCommandID == 0 {
		h.nextCommandID++
	}
	return h.nextCommandID
}

func (h *Hub) ApplyExternalTallies(name string, tallies [protocol.MaxTallies]protocol.TallyState) {
	h.mu.Lock()
	h.simulationEnabled = false
	h.switchers[name] = SwitcherConnectionState{
		Connected:  true,
		LastError:  "",
		LastUpdate: time.Now(),
	}
	h.tallies = tallies
	h.mu.Unlock()
	h.notifyStatusChanged()
	_ = h.pushRadioPayload()
}

func (h *Hub) ReportSwitcherStatus(name string, connected bool, detail string) {
	h.mu.Lock()
	h.switchers[name] = SwitcherConnectionState{
		Connected:  connected,
		LastError:  detail,
		LastUpdate: time.Now(),
	}
	h.mu.Unlock()
	h.notifyStatusChanged()
}

func (h *Hub) SubscribeUpdates() (int, <-chan UpdateEvent) {
	h.mu.Lock()
	defer h.mu.Unlock()

	h.nextSubscriberID++
	id := h.nextSubscriberID
	ch := make(chan UpdateEvent, 1)
	h.subscribers[id] = ch
	return id, ch
}

func (h *Hub) UnsubscribeUpdates(id int) {
	h.mu.Lock()
	if _, ok := h.subscribers[id]; ok {
		delete(h.subscribers, id)
	}
	h.mu.Unlock()
}

func (h *Hub) notifyStatusChanged() {
	h.notifySubscribers(UpdateEvent{Status: true})
}

func (h *Hub) notifySurveyChanged() {
	h.notifySubscribers(UpdateEvent{Survey: true})
}

func (h *Hub) notifySubscribers(event UpdateEvent) {
	h.mu.RLock()
	subscribers := make([]chan UpdateEvent, 0, len(h.subscribers))
	for _, ch := range h.subscribers {
		subscribers = append(subscribers, ch)
	}
	h.mu.RUnlock()

	for _, ch := range subscribers {
		pending := event
		select {
		case ch <- pending:
			continue
		default:
		}

		select {
		case existing := <-ch:
			pending = mergeUpdateEvents(existing, pending)
		default:
		}

		select {
		case ch <- pending:
		default:
		}
	}
}

func mergeUpdateEvents(left UpdateEvent, right UpdateEvent) UpdateEvent {
	return UpdateEvent{
		Status: left.Status || right.Status,
		Survey: left.Survey || right.Survey,
	}
}
