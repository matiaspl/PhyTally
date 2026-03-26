package wifisurvey

import "encoding/binary"

type spectralSample struct {
	channel      int
	rssi         int
	noise        int
	avgMagnitude int
	maxMagnitude int
	bins         []int
}

func summarizeSpectralChannel(channel int, raw []byte) (SpectralChannel, bool) {
	samples := make([]spectralSample, 0)
	fallbackSamples := make([]spectralSample, 0)
	for len(raw) >= 3 {
		recordLen := int(binary.BigEndian.Uint16(raw[1:3]))
		totalLen := 3 + recordLen
		if totalLen > len(raw) {
			break
		}

		record := raw[:totalLen]
		raw = raw[totalLen:]

		parsed, ok := parseSpectralRecord(record)
		if !ok {
			continue
		}
		fallbackSamples = append(fallbackSamples, parsed)
		if parsed.channel == channel {
			samples = append(samples, parsed)
		}
	}

	if len(samples) == 0 {
		// Some ath9k_htc spectral captures report a stale or unusable frequency
		// in the sample header even though the interface has already been tuned.
		// In that case, attribute the capture to the channel we explicitly set.
		samples = fallbackSamples
	}
	if len(samples) == 0 {
		return SpectralChannel{}, false
	}

	var totalRSSI, totalNoise, totalMagnitude, maxMagnitude int
	for _, sample := range samples {
		totalRSSI += sample.rssi
		totalNoise += sample.noise
		totalMagnitude += sample.avgMagnitude
		if sample.maxMagnitude > maxMagnitude {
			maxMagnitude = sample.maxMagnitude
		}
	}

	return SpectralChannel{
		Channel:      channel,
		SampleCount:  len(samples),
		AvgRSSI:      totalRSSI / len(samples),
		AvgNoise:     totalNoise / len(samples),
		AvgMagnitude: totalMagnitude / len(samples),
		MaxMagnitude: maxMagnitude,
	}, true
}

func parseSpectralRecord(record []byte) (spectralSample, bool) {
	var parsed spectralSample

	switch record[0] {
	case 1:
		if len(record) < 76 {
			return parsed, false
		}
		maxExp := int(record[3] & 0x0f)
		parsed.channel = freqToChannel(int(binary.BigEndian.Uint16(record[4:6])))
		parsed.rssi = int(int8(record[6]))
		parsed.noise = int(int8(record[7]))
		parsed.maxMagnitude = int(binary.BigEndian.Uint16(record[8:10]))
		parsed.bins = scaledBins(record[20:76], maxExp)
		parsed.avgMagnitude = averageBins(parsed.bins)
		return parsed, parsed.channel != 0
	case 2:
		if len(record) < 155 {
			return parsed, false
		}
		maxExp := int(record[26] & 0x0f)
		parsed.channel = freqToChannel(int(binary.BigEndian.Uint16(record[4:6])))
		parsed.rssi = (int(int8(record[6])) + int(int8(record[7]))) / 2
		parsed.noise = (int(int8(record[16])) + int(int8(record[17]))) / 2
		lowerMax := int(binary.BigEndian.Uint16(record[18:20]))
		upperMax := int(binary.BigEndian.Uint16(record[20:22]))
		parsed.maxMagnitude = maxInt(lowerMax, upperMax)
		parsed.bins = scaledBins(record[27:155], maxExp)
		parsed.avgMagnitude = averageBins(parsed.bins)
		return parsed, parsed.channel != 0
	default:
		return parsed, false
	}
}

func spectralRowsForChannel(channel int, raw []byte) []SpectralRow {
	rows := make([]SpectralRow, 0)
	fallbackRows := make([]SpectralRow, 0)
	for len(raw) >= 3 {
		recordLen := int(binary.BigEndian.Uint16(raw[1:3]))
		totalLen := 3 + recordLen
		if totalLen > len(raw) {
			break
		}

		record := raw[:totalLen]
		raw = raw[totalLen:]

		parsed, ok := parseSpectralRecord(record)
		if !ok {
			continue
		}

		row := SpectralRow{
			Channel:      channel,
			RSSI:         parsed.rssi,
			Noise:        parsed.noise,
			MaxMagnitude: parsed.maxMagnitude,
			Bins:         parsed.bins,
		}
		fallbackRows = append(fallbackRows, row)
		if parsed.channel == channel {
			rows = append(rows, row)
		}
	}

	if len(rows) == 0 {
		rows = fallbackRows
	}
	return rows
}

func scaledBins(rawBins []byte, maxExp int) []int {
	if len(rawBins) == 0 {
		return nil
	}

	bins := make([]int, len(rawBins))
	for i, bin := range rawBins {
		bins[i] = int(bin) << maxExp
	}
	return bins
}

func averageBins(bins []int) int {
	if len(bins) == 0 {
		return 0
	}
	sum := 0
	for _, bin := range bins {
		sum += bin
	}
	return sum / len(bins)
}

func normalizeSpectralScores(channels []SpectralChannel) {
	if len(channels) == 0 {
		return
	}

	raw := make([]int, len(channels))
	minScore := 0
	maxScore := 0
	for i, channel := range channels {
		score := spectralRawScore(channel)
		raw[i] = score
		if i == 0 || score < minScore {
			minScore = score
		}
		if i == 0 || score > maxScore {
			maxScore = score
		}
	}

	for i := range channels {
		if maxScore == minScore {
			channels[i].Score = 1
			continue
		}
		channels[i].Score = 1 + ((raw[i]-minScore)*99)/(maxScore-minScore)
	}
}

func spectralRawScore(channel SpectralChannel) int {
	noiseScore := channel.AvgNoise + 110
	if noiseScore < 0 {
		noiseScore = 0
	}
	return channel.AvgMagnitude/16 + channel.MaxMagnitude/16 + clampRSSIScore(channel.AvgRSSI) + noiseScore
}

func maxInt(a int, b int) int {
	if a > b {
		return a
	}
	return b
}
