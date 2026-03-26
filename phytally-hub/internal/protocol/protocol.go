package protocol

const (
	MaxTallies           = 16
	BeaconIntervalMS     = 100
	DefaultChannel       = 1
	DefaultLEDBrightness = 255

	VendorOUI0 = 0x11
	VendorOUI1 = 0x22
	VendorOUI2 = 0x33

	TelemOUI0 = 0x11
	TelemOUI1 = 0x22
	TelemOUI2 = 0x44
)

type TallyState uint8

const (
	TallyOff     TallyState = 0
	TallyPreview TallyState = 1
	TallyProgram TallyState = 2
	TallyError   TallyState = 3
)

type CommandType uint8

const (
	CommandNone          CommandType = 0
	CommandSetID         CommandType = 1
	CommandReboot        CommandType = 2
	CommandSetBrightness CommandType = 3
	CommandIdentify      CommandType = 4
)

type HubPayload struct {
	OUI             [3]byte
	ProtocolVersion byte
	States          [MaxTallies]byte
	TargetMAC       [6]byte
	CommandType     byte
	CommandParam    byte
	CommandID       byte
}

type TelemetryPayload struct {
	OUI             [3]byte
	ProtocolVersion byte
	TallyID         byte
	BatteryPercent  byte
	RSSI            int8
	StatusFlags     uint16
	LEDBrightness   byte
	AckCommandID    byte
}

func BrightnessPercentToByte(percent int) byte {
	switch {
	case percent <= 0:
		return 0
	case percent >= 100:
		return DefaultLEDBrightness
	default:
		return byte((percent*255 + 50) / 100)
	}
}

func BrightnessByteToPercent(brightness byte) int {
	return (int(brightness)*100 + 127) / 255
}
