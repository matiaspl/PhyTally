package switcher

import (
	"bufio"
	"context"
	"fmt"
	"net"
	"strings"
	"time"

	"github.com/mstarzak/phytally/phytally-hub/internal/protocol"
)

func parseVMixTallyLine(line string) ([protocol.MaxTallies]protocol.TallyState, bool) {
	var tallies [protocol.MaxTallies]protocol.TallyState

	line = strings.TrimSpace(line)
	if !strings.HasPrefix(line, "TALLY OK ") {
		return tallies, false
	}

	states := strings.TrimPrefix(line, "TALLY OK ")
	for index, value := range states {
		if index >= protocol.MaxTallies {
			break
		}
		switch value {
		case '1':
			tallies[index] = protocol.TallyProgram
		case '2':
			tallies[index] = protocol.TallyPreview
		default:
			tallies[index] = protocol.TallyOff
		}
	}

	return tallies, true
}

func runVMix(ctx context.Context, host string, tallyCb TallyCallback, statusCb StatusCallback) {
	address := net.JoinHostPort(host, "8099")

	for {
		select {
		case <-ctx.Done():
			return
		default:
		}

		dialer := net.Dialer{Timeout: 3 * time.Second}
		conn, err := dialer.DialContext(ctx, "tcp", address)
		if err != nil {
			statusCb("vmix", false, err.Error())
			if !sleepContext(ctx, 5*time.Second) {
				return
			}
			continue
		}

		statusCb("vmix", true, "")
		_, _ = fmt.Fprint(conn, "SUBSCRIBE TALLY\r\n")
		scanner := bufio.NewScanner(conn)
		for scanner.Scan() {
			tallies, ok := parseVMixTallyLine(scanner.Text())
			if ok {
				tallyCb("vmix", tallies)
			}
		}
		_ = conn.Close()
		if scanErr := scanner.Err(); scanErr != nil {
			statusCb("vmix", false, scanErr.Error())
		} else {
			statusCb("vmix", false, "disconnected")
		}
		if !sleepContext(ctx, 2*time.Second) {
			return
		}
	}
}
