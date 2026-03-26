package switcher

import (
	"context"
	"strings"

	"github.com/mstarzak/phytally/phytally-hub/internal/config"
	"github.com/mstarzak/phytally/phytally-hub/internal/protocol"
)

type TallyCallback func(name string, tallies [protocol.MaxTallies]protocol.TallyState)
type StatusCallback func(name string, connected bool, detail string)

func HasAnyEnabled(cfg config.Config) bool {
	return atemEnabled(cfg) || vmixEnabled(cfg) || tslEnabled(cfg) || obsEnabled(cfg)
}

func StartEnabled(ctx context.Context, cfg config.Config, tallyCb TallyCallback, statusCb StatusCallback) {
	if atemEnabled(cfg) {
		go runATEM(ctx, cfg.AtemIP, tallyCb, statusCb)
	}
	if vmixEnabled(cfg) {
		go runVMix(ctx, cfg.VMixIP, tallyCb, statusCb)
	}
	if tslEnabled(cfg) {
		go runTSL(ctx, cfg.TSLListenAddr, tallyCb, statusCb)
	}
	if obsEnabled(cfg) {
		go runOBS(ctx, cfg.OBSURL, cfg.OBSPassword, tallyCb, statusCb)
	}
}

func atemEnabled(cfg config.Config) bool {
	return cfg.AtemEnabled && strings.TrimSpace(cfg.AtemIP) != "" && cfg.AtemIP != "0.0.0.0"
}

func vmixEnabled(cfg config.Config) bool {
	return cfg.VMixEnabled && strings.TrimSpace(cfg.VMixIP) != "" && cfg.VMixIP != "0.0.0.0"
}

func obsEnabled(cfg config.Config) bool {
	return cfg.OBSEnabled && strings.TrimSpace(cfg.OBSURL) != ""
}

func tslEnabled(cfg config.Config) bool {
	return cfg.TSLEnabled && strings.TrimSpace(cfg.TSLListenAddr) != ""
}
