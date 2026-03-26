package switcher

import (
	"context"
	"fmt"
	"net"
	"strings"
	"time"

	"github.com/mstarzak/phytally/phytally-hub/internal/protocol"
)

const tslIdleTimeout = 5 * time.Second

func runTSL(ctx context.Context, listenAddr string, tallyCb TallyCallback, statusCb StatusCallback) {
	listenAddr = strings.TrimSpace(listenAddr)
	if listenAddr == "" {
		statusCb("tsl", false, "empty listen address")
		return
	}

	packetBuf := make([]byte, 2048)
	for {
		select {
		case <-ctx.Done():
			return
		default:
		}

		conn, err := net.ListenPacket("udp", listenAddr)
		if err != nil {
			statusCb("tsl", false, err.Error())
			if !sleepContext(ctx, 2*time.Second) {
				return
			}
			continue
		}

		statusCb("tsl", false, fmt.Sprintf("listening on %s", listenAddr))
		lastPacketAt := time.Time{}

		for {
			if err := conn.SetReadDeadline(time.Now().Add(time.Second)); err != nil {
				statusCb("tsl", false, err.Error())
				break
			}

			n, _, err := conn.ReadFrom(packetBuf)
			if err != nil {
				if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
					if !lastPacketAt.IsZero() && time.Since(lastPacketAt) > tslIdleTimeout {
						statusCb("tsl", false, "waiting for packets")
						lastPacketAt = time.Time{}
					}
					select {
					case <-ctx.Done():
						_ = conn.Close()
						return
					default:
						continue
					}
				}
				statusCb("tsl", false, err.Error())
				break
			}

			tallies, ok := parseTSLPacket(packetBuf[:n])
			if !ok {
				continue
			}
			lastPacketAt = time.Now()
			statusCb("tsl", true, "")
			tallyCb("tsl", tallies)
		}

		_ = conn.Close()
		if !sleepContext(ctx, time.Second) {
			return
		}
	}
}

func parseTSLPacket(packet []byte) ([protocol.MaxTallies]protocol.TallyState, bool) {
	var tallies [protocol.MaxTallies]protocol.TallyState
	if len(packet) < 2 {
		return tallies, false
	}

	header := packet[0]
	if header < 0x80 {
		return tallies, false
	}
	address := int(header - 0x80)
	if address < 0 || address >= protocol.MaxTallies {
		return tallies, false
	}

	control := packet[1]
	switch {
	case control&0x01 != 0:
		tallies[address] = protocol.TallyProgram
	case control&0x02 != 0:
		tallies[address] = protocol.TallyPreview
	default:
		tallies[address] = protocol.TallyOff
	}

	return tallies, true
}
