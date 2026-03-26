package wifisurvey

import (
	"bufio"
	"bytes"
	"context"
	"fmt"
	"math"
	"os/exec"
	"sort"
	"strconv"
	"strings"
)

type Network struct {
	SSID     string `json:"ssid"`
	BSSID    string `json:"bssid"`
	RSSI     int    `json:"rssi"`
	Channel  int    `json:"channel"`
	Secure   bool   `json:"secure"`
	AuthMode string `json:"auth_mode"`
	Hidden   bool   `json:"hidden"`
}

type ChannelScore struct {
	Channel int `json:"channel"`
	Score   int `json:"score"`
}

type ChannelSuggestion struct {
	RecommendedChannel    int            `json:"recommended_channel"`
	RecommendedScore      int            `json:"recommended_score"`
	ConfiguredSSID        string         `json:"configured_ssid"`
	ConfiguredSSIDChannel *int           `json:"configured_ssid_channel"`
	TopNonOverlapping     []ChannelScore `json:"top_non_overlapping"`
	ChannelScores         []ChannelScore `json:"channel_scores"`
}

type SpectralChannel struct {
	Channel      int `json:"channel"`
	SampleCount  int `json:"sample_count"`
	AvgRSSI      int `json:"avg_rssi"`
	AvgNoise     int `json:"avg_noise"`
	AvgMagnitude int `json:"avg_magnitude"`
	MaxMagnitude int `json:"max_magnitude"`
	Score        int `json:"score"`
}

type SpectralRow struct {
	Channel      int   `json:"channel"`
	RSSI         int   `json:"rssi"`
	Noise        int   `json:"noise"`
	MaxMagnitude int   `json:"max_magnitude"`
	Bins         []int `json:"bins"`
}

type Status struct {
	ScannedAtMS         int64              `json:"scanned_at_ms"`
	ScanStartedAtMS     int64              `json:"scan_started_at_ms"`
	EstimatedDurationMS int64              `json:"estimated_duration_ms"`
	Scanning            bool               `json:"scanning"`
	ScanMode            string             `json:"scan_mode,omitempty"`
	NetworkCount        int                `json:"network_count"`
	Error               string             `json:"error,omitempty"`
	Warnings            []string           `json:"warnings,omitempty"`
	ChannelSuggestion   *ChannelSuggestion `json:"channel_suggestion"`
	SpectralChannels    []SpectralChannel  `json:"spectral_channels,omitempty"`
	SpectralRows        []SpectralRow      `json:"spectral_rows,omitempty"`
	Networks            []Network          `json:"networks"`
}

type Scanner struct {
	IWPath         string
	IPPath         string
	RadioInterface string
	DebugFSRoot    string
}

func (s Scanner) Scan(ctx context.Context, configuredSSID string) (Status, error) {
	iwPath := s.IWPath
	if iwPath == "" {
		iwPath = "iw"
	}
	ipPath := s.IPPath
	if ipPath == "" {
		ipPath = "ip"
	}

	scanIface := "phytallyscan0"
	_ = runCommand(ctx, ipPath, "link", "set", scanIface, "down")
	_ = runCommand(ctx, iwPath, "dev", scanIface, "del")

	output, cleanup, scanErr := s.scanOutput(ctx, iwPath, ipPath, scanIface)
	networks := []Network{}
	if scanErr == nil {
		networks = parseNetworks(string(output))
	}
	if cleanup != nil {
		cleanup()
	}

	spectralChannels, spectralRows, spectralErr := s.scanSpectral(ctx, iwPath, ipPath)
	if scanErr != nil && spectralErr != nil {
		return Status{}, fmt.Errorf("iw scan failed: %w; spectral scan failed: %v", scanErr, spectralErr)
	}

	warnings := make([]string, 0, 2)
	switch {
	case scanErr == nil && spectralErr == nil:
	case scanErr != nil:
		warnings = append(warnings, fmt.Sprintf("iw scan failed: %v", scanErr))
	case spectralErr != nil:
		warnings = append(warnings, fmt.Sprintf("spectral scan unavailable: %v", spectralErr))
	}

	return Status{
		Scanning:          false,
		ScanMode:          scanMode(scanErr == nil, len(spectralChannels) > 0),
		NetworkCount:      len(networks),
		Warnings:          warnings,
		ChannelSuggestion: buildSuggestion(networks, configuredSSID, spectralChannels),
		SpectralChannels:  spectralChannels,
		SpectralRows:      spectralRows,
		Networks:          networks,
	}, nil
}

func (s Scanner) scanOutput(ctx context.Context, iwPath string, ipPath string, scanIface string) ([]byte, func(), error) {
	_ = runCommand(ctx, ipPath, "link", "set", s.RadioInterface, "up")

	output, err := captureCommand(ctx, iwPath, "dev", s.RadioInterface, "scan", "ap-force")
	if err == nil {
		return output, nil, nil
	}

	if err := runCommand(ctx, iwPath, "dev", s.RadioInterface, "interface", "add", scanIface, "type", "managed"); err == nil {
		cleanup := func() {
			_ = runCommand(context.Background(), ipPath, "link", "set", scanIface, "down")
			_ = runCommand(context.Background(), iwPath, "dev", scanIface, "del")
		}
		if err := runCommand(ctx, ipPath, "link", "set", scanIface, "up"); err != nil {
			cleanup()
			return nil, nil, fmt.Errorf("bring up scan interface: %w", err)
		}
		output, scanErr := captureCommand(ctx, iwPath, "dev", scanIface, "scan", "ap-force")
		if scanErr != nil {
			cleanup()
			return nil, nil, fmt.Errorf("scan on temporary interface: %w", scanErr)
		}
		return output, cleanup, nil
	}

	return nil, nil, fmt.Errorf("scan on %s: %w", s.RadioInterface, err)
}

func parseNetworks(output string) []Network {
	scanner := bufio.NewScanner(strings.NewReader(output))
	networks := make([]Network, 0)

	var current *Network
	var sawWPA bool
	var sawRSN bool
	var sawPrivacy bool

	flush := func() {
		if current == nil || current.BSSID == "" {
			current = nil
			sawWPA = false
			sawRSN = false
			sawPrivacy = false
			return
		}
		if current.Channel == 0 {
			current = nil
			sawWPA = false
			sawRSN = false
			sawPrivacy = false
			return
		}
		current.Hidden = current.SSID == ""
		current.Secure, current.AuthMode = classifyAuth(sawWPA, sawRSN, sawPrivacy)
		networks = append(networks, *current)
		current = nil
		sawWPA = false
		sawRSN = false
		sawPrivacy = false
	}

	for scanner.Scan() {
		line := scanner.Text()
		trimmed := strings.TrimSpace(line)
		if strings.HasPrefix(trimmed, "BSS ") {
			flush()
			current = &Network{
				BSSID: strings.ToUpper(parseBSSID(trimmed)),
			}
			continue
		}
		if current == nil {
			continue
		}

		switch {
		case strings.HasPrefix(trimmed, "freq:"):
			freqText := strings.TrimSpace(strings.TrimPrefix(trimmed, "freq:"))
			if value, err := strconv.ParseFloat(freqText, 64); err == nil {
				current.Channel = freqToChannel(int(math.Round(value)))
			}
		case strings.HasPrefix(trimmed, "signal:"):
			fields := strings.Fields(strings.TrimSpace(strings.TrimPrefix(trimmed, "signal:")))
			if len(fields) > 0 {
				if value, err := strconv.ParseFloat(fields[0], 64); err == nil {
					current.RSSI = int(math.Round(value))
				}
			}
		case strings.HasPrefix(trimmed, "SSID:"):
			current.SSID = strings.TrimSpace(strings.TrimPrefix(trimmed, "SSID:"))
		case strings.HasPrefix(trimmed, "RSN:"):
			sawRSN = true
		case strings.HasPrefix(trimmed, "WPA:"):
			sawWPA = true
		case strings.HasPrefix(trimmed, "capability:") && strings.Contains(trimmed, "Privacy"):
			sawPrivacy = true
		}
	}
	flush()

	sort.SliceStable(networks, func(i, j int) bool {
		if networks[i].Channel == networks[j].Channel {
			return networks[i].RSSI > networks[j].RSSI
		}
		return networks[i].Channel < networks[j].Channel
	})

	return networks
}

func parseBSSID(line string) string {
	rest := strings.TrimPrefix(line, "BSS ")
	if idx := strings.Index(rest, "("); idx >= 0 {
		return strings.TrimSpace(rest[:idx])
	}
	return strings.TrimSpace(rest)
}

func classifyAuth(sawWPA bool, sawRSN bool, sawPrivacy bool) (bool, string) {
	switch {
	case sawWPA && sawRSN:
		return true, "wpa-wpa2-psk"
	case sawRSN:
		return true, "wpa2-psk"
	case sawWPA:
		return true, "wpa-psk"
	case sawPrivacy:
		return true, "wep"
	default:
		return false, "open"
	}
}

func freqToChannel(freq int) int {
	switch {
	case freq == 2484:
		return 14
	case freq >= 2412 && freq <= 2472:
		return (freq-2412)/5 + 1
	default:
		return 0
	}
}

func buildSuggestion(networks []Network, configuredSSID string, spectralChannels []SpectralChannel) *ChannelSuggestion {
	channelScores := make([]ChannelScore, 13)
	for i := range channelScores {
		channelScores[i] = ChannelScore{Channel: i + 1}
	}

	var configuredChannel *int
	configuredRSSI := -127

	for _, network := range networks {
		if network.Channel < 1 || network.Channel > 13 {
			continue
		}
		rssiScore := networkInterferenceScore(network.RSSI)
		for candidate := 1; candidate <= 13; candidate++ {
			channelScores[candidate-1].Score += rssiScore * wifiOverlapWeight(network.Channel, candidate)
		}

		if configuredSSID != "" && network.SSID == configuredSSID && network.RSSI > configuredRSSI {
			channel := network.Channel
			configuredChannel = &channel
			configuredRSSI = network.RSSI
		}
	}

	for _, spectral := range spectralChannels {
		if spectral.Channel < 1 || spectral.Channel > 13 || spectral.Score <= 0 {
			continue
		}
		spectralScore := spectralInterferenceScore(spectral)
		for candidate := 1; candidate <= 13; candidate++ {
			channelScores[candidate-1].Score += spectralScore * spectralOverlapWeight(spectral.Channel, candidate)
		}
	}

	preferred := []int{1, 6, 11}
	recommended := channelScores[preferred[0]-1]
	for _, channel := range preferred[1:] {
		score := channelScores[channel-1]
		if score.Score < recommended.Score {
			recommended = score
		}
	}

	topNonOverlapping := make([]ChannelScore, 0, len(preferred))
	for _, channel := range preferred {
		topNonOverlapping = append(topNonOverlapping, channelScores[channel-1])
	}
	sort.Slice(topNonOverlapping, func(i, j int) bool {
		if topNonOverlapping[i].Score == topNonOverlapping[j].Score {
			return topNonOverlapping[i].Channel < topNonOverlapping[j].Channel
		}
		return topNonOverlapping[i].Score < topNonOverlapping[j].Score
	})

	return &ChannelSuggestion{
		RecommendedChannel:    recommended.Channel,
		RecommendedScore:      recommended.Score,
		ConfiguredSSID:        configuredSSID,
		ConfiguredSSIDChannel: configuredChannel,
		TopNonOverlapping:     topNonOverlapping,
		ChannelScores:         channelScores,
	}
}

func scanMode(haveIW bool, haveSpectral bool) string {
	switch {
	case haveIW && haveSpectral:
		return "iw+spectral"
	case haveSpectral:
		return "spectral"
	case haveIW:
		return "iw"
	default:
		return ""
	}
}

func clampRSSIScore(rssi int) int {
	if rssi < -95 {
		rssi = -95
	}
	if rssi > -30 {
		rssi = -30
	}
	return rssi + 96
}

func networkInterferenceScore(rssi int) int {
	linear := clampRSSIScore(rssi)
	if linear <= 0 {
		return 0
	}

	level := (linear + 4) / 5
	return level * level
}

func spectralInterferenceScore(channel SpectralChannel) int {
	base := channel.Score
	if channel.MaxMagnitude > channel.AvgMagnitude {
		base += (channel.MaxMagnitude - channel.AvgMagnitude) / 2
	}
	if channel.SampleCount > 1 {
		base += min(channel.SampleCount-1, 8)
	}
	return base
}

func wifiOverlapWeight(observedChannel int, candidateChannel int) int {
	distance := observedChannel - candidateChannel
	if distance < 0 {
		distance = -distance
	}
	switch distance {
	case 0:
		return 100
	case 1:
		return 42
	case 2:
		return 12
	case 3:
		return 3
	default:
		return 0
	}
}

func spectralOverlapWeight(observedChannel int, candidateChannel int) int {
	distance := observedChannel - candidateChannel
	if distance < 0 {
		distance = -distance
	}
	switch distance {
	case 0:
		return 100
	case 1:
		return 24
	case 2:
		return 5
	default:
		return 0
	}
}

func min(a int, b int) int {
	if a < b {
		return a
	}
	return b
}

func runCommand(ctx context.Context, name string, args ...string) error {
	cmd := exec.CommandContext(ctx, name, args...)
	output, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("%w: %s", err, strings.TrimSpace(string(output)))
	}
	return nil
}

func captureCommand(ctx context.Context, name string, args ...string) ([]byte, error) {
	cmd := exec.CommandContext(ctx, name, args...)
	output, err := cmd.CombinedOutput()
	if err != nil {
		return nil, fmt.Errorf("%w: %s", err, strings.TrimSpace(string(output)))
	}
	return bytes.TrimSpace(output), nil
}
