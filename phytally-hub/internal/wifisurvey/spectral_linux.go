//go:build linux

package wifisurvey

import (
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"slices"
	"strconv"
	"time"
)

const (
	defaultDebugFSRoot      = "/sys/kernel/debug/ieee80211"
	spectralCaptureWindow   = 350 * time.Millisecond
	spectralTriggerCount    = 4
	spectralTriggerInterval = 25 * time.Millisecond
)

func (s Scanner) scanSpectral(ctx context.Context, iwPath string, ipPath string) ([]SpectralChannel, []SpectralRow, error) {
	basePath, err := s.findSpectralBasePath()
	if err != nil {
		return nil, nil, err
	}

	if err := setInterfaceMode(ctx, ipPath, iwPath, s.RadioInterface, "monitor"); err != nil {
		return nil, nil, fmt.Errorf("switch interface to monitor mode: %w", err)
	}
	defer func() {
		_ = setInterfaceMode(context.Background(), ipPath, iwPath, s.RadioInterface, "managed")
	}()

	channels := make([]SpectralChannel, 0, 13)
	rows := make([]SpectralRow, 0, 32)
	for channel := 1; channel <= 13; channel++ {
		if err := runCommand(ctx, iwPath, "dev", s.RadioInterface, "set", "channel", strconv.Itoa(channel)); err != nil {
			return nil, nil, fmt.Errorf("set channel %d: %w", channel, err)
		}

		samples, err := captureSpectralSamples(ctx, basePath)
		if err != nil {
			continue
		}

		rows = append(rows, spectralRowsForChannel(channel, samples)...)
		summary, ok := summarizeSpectralChannel(channel, samples)
		if !ok {
			continue
		}
		channels = append(channels, summary)
	}

	if len(channels) == 0 {
		return nil, nil, fmt.Errorf("no spectral samples captured")
	}

	normalizeSpectralScores(channels)
	slices.SortFunc(channels, func(a, b SpectralChannel) int {
		return a.Channel - b.Channel
	})
	return channels, rows, nil
}

func (s Scanner) findSpectralBasePath() (string, error) {
	root := s.DebugFSRoot
	if root == "" {
		root = defaultDebugFSRoot
	}

	phys, err := os.ReadDir(root)
	if err != nil {
		return "", fmt.Errorf("open debugfs root: %w", err)
	}

	preferred := []string{"ath9k_htc", "ath9k"}
	for _, phy := range phys {
		if !phy.IsDir() {
			continue
		}
		phyPath := filepath.Join(root, phy.Name())
		if _, err := os.Stat(filepath.Join(phyPath, "netdev:"+s.RadioInterface)); err != nil {
			continue
		}
		for _, name := range preferred {
			base := filepath.Join(phyPath, name)
			if _, err := os.Stat(filepath.Join(base, "spectral_scan_ctl")); err == nil {
				return base, nil
			}
		}
	}

	return "", fmt.Errorf("spectral debugfs not found for %s", s.RadioInterface)
}

func captureSpectralSamples(parent context.Context, basePath string) ([]byte, error) {
	captureCtx, cancel := context.WithTimeout(parent, spectralCaptureWindow)
	defer cancel()

	scanFile := filepath.Join(basePath, "spectral_scan0")
	cmd := exec.CommandContext(captureCtx, "cat", scanFile)
	outputCh := make(chan []byte, 1)
	errCh := make(chan error, 1)

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return nil, fmt.Errorf("open spectral stdout pipe: %w", err)
	}
	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("start spectral capture: %w", err)
	}

	go func() {
		data, readErr := ioReadAll(stdout)
		outputCh <- data
		errCh <- readErr
	}()

	if err := writeDebugFS(filepath.Join(basePath, "spectral_scan_ctl"), "disable"); err != nil {
		_ = cmd.Process.Kill()
		_ = cmd.Wait()
		return nil, err
	}
	_ = writeDebugFS(filepath.Join(basePath, "spectral_short_repeat"), "1")
	_ = writeDebugFS(filepath.Join(basePath, "spectral_fft_period"), "1")
	_ = writeDebugFS(filepath.Join(basePath, "spectral_period"), "4")
	_ = writeDebugFS(filepath.Join(basePath, "spectral_count"), "8")
	if err := writeDebugFS(filepath.Join(basePath, "spectral_scan_ctl"), "manual"); err != nil {
		_ = cmd.Process.Kill()
		_ = cmd.Wait()
		return nil, err
	}

	for range spectralTriggerCount {
		_ = writeDebugFS(filepath.Join(basePath, "spectral_scan_ctl"), "trigger")
		time.Sleep(spectralTriggerInterval)
	}
	_ = writeDebugFS(filepath.Join(basePath, "spectral_scan_ctl"), "disable")

	waitErr := cmd.Wait()
	data := <-outputCh
	readErr := <-errCh

	if len(data) > 0 {
		return data, nil
	}
	if readErr != nil {
		return nil, fmt.Errorf("read spectral stream: %w", readErr)
	}
	if captureCtx.Err() != nil {
		return nil, captureCtx.Err()
	}
	if waitErr != nil {
		return nil, fmt.Errorf("wait for spectral stream: %w", waitErr)
	}
	return nil, fmt.Errorf("empty spectral sample stream")
}

func writeDebugFS(path string, value string) error {
	return os.WriteFile(path, []byte(value), 0o644)
}

func ioReadAll(reader io.Reader) ([]byte, error) {
	return io.ReadAll(reader)
}

func setInterfaceMode(ctx context.Context, ipPath string, iwPath string, iface string, mode string) error {
	if err := runCommand(ctx, ipPath, "link", "set", iface, "down"); err != nil {
		return err
	}
	if err := runCommand(ctx, iwPath, "dev", iface, "set", "type", mode); err != nil {
		return err
	}
	return runCommand(ctx, ipPath, "link", "set", iface, "up")
}
