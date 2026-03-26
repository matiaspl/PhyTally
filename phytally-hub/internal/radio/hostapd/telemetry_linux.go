//go:build linux

package hostapd

import (
	"context"
	"fmt"
	"net"
	"os"
	"syscall"
	"time"
)

func (b *Backend) startTelemetryCapture(ctx context.Context) error {
	if b.cfg.MonitorInterface == "" {
		return nil
	}
	if err := b.ensureMonitorInterface(ctx); err != nil {
		return err
	}

	go b.captureLoop(ctx)
	return nil
}

func (b *Backend) ensureMonitorInterface(ctx context.Context) error {
	if _, err := net.InterfaceByName(b.cfg.MonitorInterface); err == nil {
		_ = b.runner.Run(ctx, b.cfg.IPPath, "link", "set", b.cfg.MonitorInterface, "up")
		return nil
	}

	if err := b.runner.Run(ctx, b.cfg.IWPath, "dev", b.cfg.Interface, "interface", "add", b.cfg.MonitorInterface, "type", "monitor"); err != nil {
		return err
	}
	if err := b.runner.Run(ctx, b.cfg.IPPath, "link", "set", b.cfg.MonitorInterface, "up"); err != nil {
		return err
	}
	return nil
}

func (b *Backend) captureLoop(ctx context.Context) {
	fd, err := socketForInterface(b.cfg.MonitorInterface)
	if err != nil {
		return
	}
	defer syscall.Close(fd)

	buf := make([]byte, 4096)
	for {
		select {
		case <-ctx.Done():
			return
		default:
		}

		n, _, err := syscall.Recvfrom(fd, buf, 0)
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

func socketForInterface(name string) (int, error) {
	iface, err := net.InterfaceByName(name)
	if err != nil {
		return -1, err
	}

	fd, err := syscall.Socket(syscall.AF_PACKET, syscall.SOCK_RAW, int(htons(syscall.ETH_P_ALL)))
	if err != nil {
		return -1, err
	}

	sll := &syscall.SockaddrLinklayer{
		Protocol: htons(syscall.ETH_P_ALL),
		Ifindex:  iface.Index,
	}
	if err := syscall.Bind(fd, sll); err != nil {
		_ = syscall.Close(fd)
		return -1, err
	}

	return fd, nil
}

func htons(v uint16) uint16 {
	return (v<<8)&0xff00 | v>>8
}

func shouldIgnoreRecvError(err error) bool {
	return err == syscall.EINTR || err == syscall.EAGAIN || err == os.ErrDeadlineExceeded
}

func (b *Backend) stopTelemetryCapture(ctx context.Context) error {
	if b.cfg.MonitorInterface == "" {
		return nil
	}

	if _, err := net.InterfaceByName(b.cfg.MonitorInterface); err != nil {
		return nil
	}

	_ = b.runner.Run(ctx, b.cfg.IPPath, "link", "set", b.cfg.MonitorInterface, "down")
	if err := b.runner.Run(ctx, b.cfg.IWPath, "dev", b.cfg.MonitorInterface, "del"); err != nil {
		return fmt.Errorf("delete monitor interface: %w", err)
	}
	return nil
}
