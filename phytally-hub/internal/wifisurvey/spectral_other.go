//go:build !linux

package wifisurvey

import (
	"context"
	"fmt"
)

func (s Scanner) scanSpectral(ctx context.Context, iwPath string, ipPath string) ([]SpectralChannel, []SpectralRow, error) {
	_ = ctx
	_ = iwPath
	_ = ipPath
	return nil, nil, fmt.Errorf("spectral scan is only supported on linux")
}
