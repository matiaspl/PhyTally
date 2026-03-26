//go:build !linux

package rawinject

import (
	"context"
	"errors"
	"time"

	"github.com/mstarzak/phytally/phytally-hub/internal/radio"
)

type execRunner struct{}

func (execRunner) Run(context.Context, string, ...string) error {
	return errors.New("unsupported")
}

type sockAddr struct{}

func (b *Backend) startMonitorInterface(context.Context) error { return nil }
func (b *Backend) stopMonitorInterface(context.Context) error  { return nil }
func socketForInterface(string) (int, *sockAddr, error)        { return -1, nil, errors.New("unsupported") }
func sendPacket(int, *sockAddr, []byte) error                  { return errors.New("unsupported") }
func recvPacket(int, []byte) (int, error)                      { return 0, errors.New("unsupported") }
func closeSocket(int) error                                    { return nil }
func shouldIgnoreRecvError(error) bool                         { return false }
func parseTelemetryPacket(packet []byte, receivedAt time.Time) (radio.Telemetry, bool) {
	return parseTelemetryPacketCommon(packet, receivedAt)
}
