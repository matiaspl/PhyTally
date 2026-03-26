package wifisurvey

import (
	"encoding/binary"
	"testing"
)

func TestParseNetworksParsesDecimalFreqAndSuggestsChannel(t *testing.T) {
	t.Parallel()

	output := `
BSS 08:7e:64:2b:2d:55(on wlp0s6u3)
	freq: 2437.0
	signal: -88.00 dBm
	capability: ESS Privacy ShortPreamble ShortSlotTime (0x0431)
	SSID: toya1234568259093
	RSN:
BSS 60:02:92:6f:ca:4e(on wlp0s6u3)
	freq: 2462.0
	signal: -79.00 dBm
	capability: ESS Privacy ShortPreamble ShortSlotTime (0x0431)
	SSID: toya123456788574
`

	networks := parseNetworks(output)
	if len(networks) != 2 {
		t.Fatalf("expected 2 networks, got %d", len(networks))
	}
	if networks[0].Channel != 6 {
		t.Fatalf("expected first network on channel 6, got %d", networks[0].Channel)
	}
	if networks[1].Channel != 11 {
		t.Fatalf("expected second network on channel 11, got %d", networks[1].Channel)
	}

	suggestion := buildSuggestion(networks, "", nil)
	if suggestion == nil {
		t.Fatalf("expected channel suggestion")
	}
	if suggestion.RecommendedChannel != 1 {
		t.Fatalf("expected channel 1 recommendation, got %d", suggestion.RecommendedChannel)
	}
}

func TestSummarizeSpectralChannelParsesHT20Samples(t *testing.T) {
	t.Parallel()

	record := makeHT20Record(2437, -65, -96, 80, 1, 48)
	summary, ok := summarizeSpectralChannel(6, record)
	if !ok {
		t.Fatalf("expected spectral summary")
	}
	if summary.Channel != 6 {
		t.Fatalf("expected channel 6, got %d", summary.Channel)
	}
	if summary.SampleCount != 1 {
		t.Fatalf("expected one sample, got %d", summary.SampleCount)
	}
	if summary.AvgRSSI != -65 {
		t.Fatalf("expected avg rssi -65, got %d", summary.AvgRSSI)
	}
	if summary.MaxMagnitude != 80 {
		t.Fatalf("expected max magnitude 80, got %d", summary.MaxMagnitude)
	}
	if summary.AvgMagnitude <= 0 {
		t.Fatalf("expected positive average magnitude, got %d", summary.AvgMagnitude)
	}
}

func TestSummarizeSpectralChannelFallsBackToCaptureChannel(t *testing.T) {
	t.Parallel()

	record := makeHT20Record(2472, -62, -95, 96, 1, 40)
	summary, ok := summarizeSpectralChannel(1, record)
	if !ok {
		t.Fatalf("expected spectral summary")
	}
	if summary.Channel != 1 {
		t.Fatalf("expected capture channel 1, got %d", summary.Channel)
	}
	if summary.SampleCount != 1 {
		t.Fatalf("expected one sample, got %d", summary.SampleCount)
	}
}

func TestSpectralRowsForChannelExposeBinData(t *testing.T) {
	t.Parallel()

	record := makeHT20Record(2437, -65, -96, 80, 1, 12)
	rows := spectralRowsForChannel(6, record)
	if len(rows) != 1 {
		t.Fatalf("expected one spectral row, got %d", len(rows))
	}
	if rows[0].Channel != 6 {
		t.Fatalf("expected channel 6 row, got %d", rows[0].Channel)
	}
	if len(rows[0].Bins) != 56 {
		t.Fatalf("expected 56 bins, got %d", len(rows[0].Bins))
	}
	if rows[0].Bins[0] != 24 {
		t.Fatalf("expected scaled first bin 24, got %d", rows[0].Bins[0])
	}
}

func TestBuildSuggestionIncludesSpectralScores(t *testing.T) {
	t.Parallel()

	spectral := []SpectralChannel{
		{Channel: 1, Score: 5},
		{Channel: 6, Score: 70},
		{Channel: 11, Score: 90},
	}

	suggestion := buildSuggestion(nil, "", spectral)
	if suggestion == nil {
		t.Fatalf("expected channel suggestion")
	}
	if suggestion.RecommendedChannel != 1 {
		t.Fatalf("expected channel 1 recommendation, got %d", suggestion.RecommendedChannel)
	}
}

func TestNetworkInterferenceScoreFavorsStrongNearbyAPs(t *testing.T) {
	t.Parallel()

	strong := networkInterferenceScore(-45)
	weak := networkInterferenceScore(-85)
	if strong <= weak {
		t.Fatalf("expected strong AP score to exceed weak AP score, got strong=%d weak=%d", strong, weak)
	}
	if strong < weak*4 {
		t.Fatalf("expected strong AP score to grow non-linearly, got strong=%d weak=%d", strong, weak)
	}
}

func TestBuildSuggestionPenalizesDirectChannelMoreThanAdjacent(t *testing.T) {
	t.Parallel()

	networks := []Network{
		{SSID: "loud", Channel: 6, RSSI: -42},
	}

	suggestion := buildSuggestion(networks, "", nil)
	if suggestion == nil {
		t.Fatalf("expected channel suggestion")
	}
	if suggestion.RecommendedChannel == 6 {
		t.Fatalf("expected recommendation to avoid occupied direct channel 6")
	}
	channel6 := suggestion.ChannelScores[5].Score
	channel1 := suggestion.ChannelScores[0].Score
	channel11 := suggestion.ChannelScores[10].Score
	if channel6 <= channel1 || channel6 <= channel11 {
		t.Fatalf("expected direct channel penalty to dominate, got ch1=%d ch6=%d ch11=%d", channel1, channel6, channel11)
	}
}

func makeHT20Record(freq uint16, rssi int8, noise int8, maxMagnitude uint16, maxExp byte, binValue byte) []byte {
	record := make([]byte, 76)
	record[0] = 1
	binary.BigEndian.PutUint16(record[1:3], 73)
	record[3] = maxExp
	binary.BigEndian.PutUint16(record[4:6], freq)
	record[6] = byte(rssi)
	record[7] = byte(noise)
	binary.BigEndian.PutUint16(record[8:10], maxMagnitude)
	record[10] = 28
	record[11] = 8
	for i := 20; i < 76; i++ {
		record[i] = binValue
	}
	return record
}
