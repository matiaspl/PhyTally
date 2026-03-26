//go:build linux

package rawinject

import (
	"context"
	"net"
	"os/exec"
	"strconv"
	"syscall"
	"time"

	"github.com/mstarzak/phytally/phytally-hub/internal/radio"
)

type execRunner struct{}

func (execRunner) Run(ctx context.Context, name string, args ...string) error {
	cmd := exec.CommandContext(ctx, name, args...)
	output, err := cmd.CombinedOutput()
	if err != nil {
		if len(output) == 0 {
			return err
		}
		return execError(err, string(output))
	}
	return nil
}

type sockAddr struct {
	link *syscall.SockaddrLinklayer
}

func (b *Backend) startMonitorInterface(ctx context.Context) error {
	b.mu.Lock()
	b.socketInterface = b.cfg.Interface
	b.mu.Unlock()

	if err := b.runner.Run(ctx, b.cfg.IPPath, "link", "set", b.cfg.Interface, "down"); err != nil {
		return err
	}
	if err := b.runner.Run(ctx, b.cfg.IWPath, "dev", b.cfg.Interface, "set", "type", "monitor"); err != nil {
		return err
	}
	if err := b.runner.Run(ctx, b.cfg.IPPath, "link", "set", b.cfg.Interface, "up"); err != nil {
		return err
	}
	return b.runner.Run(ctx, b.cfg.IWPath, "dev", b.cfg.Interface, "set", "channel", strconv.Itoa(b.cfg.Channel))
}

func (b *Backend) stopMonitorInterface(ctx context.Context) error {
	if _, err := net.InterfaceByName(b.cfg.Interface); err != nil {
		return nil
	}
	_ = b.runner.Run(ctx, b.cfg.IPPath, "link", "set", b.cfg.Interface, "down")
	if err := b.runner.Run(ctx, b.cfg.IWPath, "dev", b.cfg.Interface, "set", "type", "managed"); err != nil {
		return err
	}
	b.mu.Lock()
	b.socketInterface = ""
	b.mu.Unlock()
	return b.runner.Run(ctx, b.cfg.IPPath, "link", "set", b.cfg.Interface, "up")
}

func socketForInterface(name string) (int, *sockAddr, error) {
	iface, err := net.InterfaceByName(name)
	if err != nil {
		return -1, nil, err
	}

	fd, err := syscall.Socket(syscall.AF_PACKET, syscall.SOCK_RAW, int(htons(syscall.ETH_P_ALL)))
	if err != nil {
		return -1, nil, err
	}

	addr := &syscall.SockaddrLinklayer{
		Protocol: htons(syscall.ETH_P_ALL),
		Ifindex:  iface.Index,
	}

	if err := syscall.Bind(fd, addr); err != nil {
		_ = syscall.Close(fd)
		return -1, nil, err
	}

	return fd, &sockAddr{link: addr}, nil
}

func sendPacket(fd int, addr *sockAddr, packet []byte) error {
	return syscall.Sendto(fd, packet, 0, addr.link)
}

func recvPacket(fd int, buf []byte) (int, error) {
	n, _, err := syscall.Recvfrom(fd, buf, 0)
	return n, err
}

func closeSocket(fd int) error {
	return syscall.Close(fd)
}

func shouldIgnoreRecvError(err error) bool {
	return err == syscall.EINTR || err == syscall.EAGAIN
}

func htons(v uint16) uint16 {
	return (v<<8)&0xff00 | v>>8
}

func parseTelemetryPacket(packet []byte, receivedAt time.Time) (radio.Telemetry, bool) {
	return parseTelemetryPacketCommon(packet, receivedAt)
}

func execError(err error, output string) error {
	return &commandError{err: err, output: output}
}

type commandError struct {
	err    error
	output string
}

func (e *commandError) Error() string {
	return e.err.Error() + ": " + e.output
}
