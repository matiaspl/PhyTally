package hostapd

import (
	"encoding/binary"
	"fmt"
	"time"

	"github.com/mstarzak/phytally/phytally-hub/internal/protocol"
	"github.com/mstarzak/phytally/phytally-hub/internal/radio"
)

func parseTelemetryPacket(packet []byte, receivedAt time.Time) (radio.Telemetry, bool) {
	radiotapLen, airRSSI, ok := parseRadiotap(packet)
	if !ok || len(packet) < radiotapLen+24 {
		return radio.Telemetry{}, false
	}

	frame := packet[radiotapLen:]
	if frame[0] != 0x40 {
		return radio.Telemetry{}, false
	}

	sourceMAC := formatMAC(frame[10:16])
	offset := 24
	for offset+2 <= len(frame) {
		tag := frame[offset]
		tagLen := int(frame[offset+1])
		offset += 2
		if offset+tagLen > len(frame) {
			return radio.Telemetry{}, false
		}

		if tag == 0xDD && tagLen >= 9 {
			body := frame[offset : offset+tagLen]
			if len(body) >= 9 && body[0] == 0x11 && body[1] == 0x22 && body[2] == 0x44 {
				brightnessPercent := 100
				ackCommandID := 0
				if len(body) >= 10 {
					brightnessPercent = protocol.BrightnessByteToPercent(body[9])
				}
				if len(body) >= 11 {
					ackCommandID = int(body[10])
				}
				return radio.Telemetry{
					SourceMAC:         sourceMAC,
					TallyID:           int(body[4]) + 1,
					BatteryPercent:    int(body[5]),
					BrightnessPercent: brightnessPercent,
					AckCommandID:      ackCommandID,
					ReportedHubRSSI:   int(int8(body[6])),
					AirRSSI:           airRSSI,
					ReceivedAt:        receivedAt,
				}, true
			}
		}

		offset += tagLen
	}

	return radio.Telemetry{}, false
}

func parseRadiotap(packet []byte) (int, int, bool) {
	if len(packet) < 8 || packet[0] != 0x00 {
		return 0, 0, false
	}
	headerLen := int(binary.LittleEndian.Uint16(packet[2:4]))
	if headerLen <= 0 || len(packet) < headerLen {
		return 0, 0, false
	}

	presentWords := 1
	presentOffset := 4
	for {
		if len(packet) < presentOffset+4 {
			return 0, 0, false
		}
		word := binary.LittleEndian.Uint32(packet[presentOffset : presentOffset+4])
		presentOffset += 4
		if word&(1<<31) == 0 {
			break
		}
		presentWords++
		if presentWords > 8 {
			return 0, 0, false
		}
	}

	fieldsOffset := 4 + presentWords*4
	if fieldsOffset > headerLen {
		return 0, 0, false
	}

	fieldOffset := fieldsOffset
	presentOffset = 4
	for wordIndex := 0; wordIndex < presentWords; wordIndex++ {
		present := binary.LittleEndian.Uint32(packet[presentOffset : presentOffset+4])
		presentOffset += 4
		for bit := 0; bit < 32; bit++ {
			mask := uint32(1) << bit
			if present&mask == 0 {
				continue
			}
			globalBit := wordIndex*32 + bit
			if globalBit == 31 {
				continue
			}

			align, size, known := radiotapFieldLayout(globalBit)
			if !known {
				return headerLen, 0, true
			}
			fieldOffset = alignOffset(fieldOffset, align)
			if fieldOffset+size > headerLen {
				return 0, 0, false
			}
			if globalBit == 5 {
				return headerLen, int(int8(packet[fieldOffset])), true
			}
			fieldOffset += size
		}
	}

	return headerLen, 0, true
}

func radiotapFieldLayout(bit int) (align int, size int, known bool) {
	switch bit {
	case 0:
		return 8, 8, true
	case 1:
		return 1, 1, true
	case 2:
		return 1, 1, true
	case 3:
		return 2, 4, true
	case 4:
		return 2, 2, true
	case 5:
		return 1, 1, true
	case 6:
		return 1, 1, true
	case 7:
		return 2, 2, true
	case 8:
		return 2, 2, true
	case 9:
		return 2, 2, true
	case 10:
		return 1, 1, true
	case 11:
		return 1, 1, true
	case 12:
		return 1, 1, true
	case 13:
		return 1, 1, true
	case 14:
		return 2, 2, true
	case 15:
		return 2, 2, true
	case 16:
		return 1, 1, true
	case 17:
		return 1, 1, true
	default:
		return 0, 0, false
	}
}

func alignOffset(offset int, align int) int {
	if align <= 1 {
		return offset
	}
	remainder := offset % align
	if remainder == 0 {
		return offset
	}
	return offset + (align - remainder)
}

func formatMAC(raw []byte) string {
	if len(raw) < 6 {
		return ""
	}
	return fmt.Sprintf("%02X:%02X:%02X:%02X:%02X:%02X", raw[0], raw[1], raw[2], raw[3], raw[4], raw[5])
}
