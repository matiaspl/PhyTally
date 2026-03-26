package switcher

import (
	"context"
	"encoding/binary"
	"fmt"
	"math/rand"
	"net"
	"time"

	"github.com/mstarzak/phytally/phytally-hub/internal/protocol"
)

const (
	atemHeaderAckRequest       = 0x1
	atemHeaderHelloPacket      = 0x2
	atemHeaderAck              = 0x10
	atemPort                   = 9910
	atemHelloPacketLength      = 20
	atemHeaderLength           = 12
	atemSessionPlaceholderID   = 0x53AB
	atemReconnectInterval      = 2 * time.Second
	atemConnectionLostInterval = 5 * time.Second
)

func runATEM(ctx context.Context, host string, tallyCb TallyCallback, statusCb StatusCallback) {
	remoteAddr := &net.UDPAddr{IP: net.ParseIP(host), Port: atemPort}
	if remoteAddr.IP == nil {
		statusCb("atem", false, "invalid IP address")
		return
	}

	rng := rand.New(rand.NewSource(time.Now().UnixNano()))
	buffer := make([]byte, 2048)

	for {
		select {
		case <-ctx.Done():
			return
		default:
		}

		localPort := 50100 + rng.Intn(15200)
		conn, err := net.ListenUDP("udp4", &net.UDPAddr{Port: localPort})
		if err != nil {
			statusCb("atem", false, err.Error())
			if !sleepContext(ctx, atemReconnectInterval) {
				return
			}
			continue
		}

		client := atemClient{
			conn:      conn,
			remote:    remoteAddr,
			sessionID: atemSessionPlaceholderID,
		}

		if err := client.sendHello(); err != nil {
			statusCb("atem", false, err.Error())
			_ = conn.Close()
			if !sleepContext(ctx, atemReconnectInterval) {
				return
			}
			continue
		}

		statusCb("atem", true, "")
		lastContact := time.Now()

		for {
			if err := conn.SetReadDeadline(time.Now().Add(500 * time.Millisecond)); err != nil {
				break
			}
			n, _, err := conn.ReadFromUDP(buffer)
			if err != nil {
				if ne, ok := err.(net.Error); ok && ne.Timeout() {
					if time.Since(lastContact) > atemConnectionLostInterval {
						statusCb("atem", false, "connection timed out")
						break
					}
					select {
					case <-ctx.Done():
						_ = conn.Close()
						return
					default:
						continue
					}
				}
				statusCb("atem", false, err.Error())
				break
			}

			lastContact = time.Now()
			tallies, hasTallies, err := client.handlePacket(buffer[:n])
			if err != nil {
				statusCb("atem", false, err.Error())
				break
			}
			statusCb("atem", true, "")
			if hasTallies {
				tallyCb("atem", tallies)
			}
		}

		_ = conn.Close()
		if !sleepContext(ctx, atemReconnectInterval) {
			return
		}
	}
}

type atemClient struct {
	conn      *net.UDPConn
	remote    *net.UDPAddr
	sessionID uint16
}

func (c *atemClient) sendHello() error {
	packet := make([]byte, atemHelloPacketLength)
	packet[0] = byte((atemHeaderHelloPacket << 3) | ((atemHelloPacketLength >> 8) & 0x07))
	packet[1] = byte(atemHelloPacketLength)
	binary.BigEndian.PutUint16(packet[2:4], c.sessionID)
	packet[9] = 0x3a
	packet[12] = 0x01
	_, err := c.conn.WriteToUDP(packet, c.remote)
	return err
}

func (c *atemClient) sendAck(remotePacketID uint16) error {
	packet := make([]byte, atemHeaderLength)
	packet[0] = byte((atemHeaderAck << 3) | ((atemHeaderLength >> 8) & 0x07))
	packet[1] = byte(atemHeaderLength)
	binary.BigEndian.PutUint16(packet[2:4], c.sessionID)
	binary.BigEndian.PutUint16(packet[4:6], remotePacketID)
	packet[9] = 0x03
	_, err := c.conn.WriteToUDP(packet, c.remote)
	return err
}

func (c *atemClient) handlePacket(packet []byte) ([protocol.MaxTallies]protocol.TallyState, bool, error) {
	var tallies [protocol.MaxTallies]protocol.TallyState
	if len(packet) < atemHeaderLength {
		return tallies, false, fmt.Errorf("short ATEM packet")
	}

	c.sessionID = binary.BigEndian.Uint16(packet[2:4])
	packetLength := int(binary.BigEndian.Uint16([]byte{packet[0] & 0x07, packet[1]}))
	if packetLength > len(packet) {
		packetLength = len(packet)
	}
	headerMask := packet[0] >> 3
	remotePacketID := binary.BigEndian.Uint16(packet[10:12])

	if headerMask&atemHeaderHelloPacket != 0 {
		if err := c.sendAck(remotePacketID); err != nil {
			return tallies, false, err
		}
	}
	if headerMask&atemHeaderAckRequest != 0 {
		if err := c.sendAck(remotePacketID); err != nil {
			return tallies, false, err
		}
	}
	if packetLength <= atemHeaderLength || headerMask&atemHeaderHelloPacket != 0 {
		return tallies, false, nil
	}

	index := atemHeaderLength
	for index+8 <= packetLength {
		segmentLength := int(binary.BigEndian.Uint16(packet[index : index+2]))
		if segmentLength < 8 || index+segmentLength > packetLength {
			break
		}
		cmd := string(packet[index+4 : index+8])
		body := packet[index+8 : index+segmentLength]
		if cmd == "TlIn" {
			parsed := parseATEMTlIn(body)
			return parsed, true, nil
		}
		index += segmentLength
	}

	return tallies, false, nil
}

func parseATEMTlIn(body []byte) [protocol.MaxTallies]protocol.TallyState {
	var tallies [protocol.MaxTallies]protocol.TallyState
	if len(body) < 2 {
		return tallies
	}

	sources := int(binary.BigEndian.Uint16(body[:2]))
	if sources > protocol.MaxTallies {
		sources = protocol.MaxTallies
	}
	if len(body) < 2+sources {
		sources = len(body) - 2
		if sources < 0 {
			sources = 0
		}
	}

	for index := 0; index < sources; index++ {
		flags := body[2+index]
		switch {
		case flags&0x01 != 0:
			tallies[index] = protocol.TallyProgram
		case flags&0x02 != 0:
			tallies[index] = protocol.TallyPreview
		default:
			tallies[index] = protocol.TallyOff
		}
	}

	return tallies
}
