//go:build !linux

package hostapd

import "context"

func (b *Backend) startTelemetryCapture(context.Context) error {
	return nil
}

func (b *Backend) stopTelemetryCapture(context.Context) error {
	return nil
}
