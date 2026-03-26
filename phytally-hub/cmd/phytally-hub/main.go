package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"net/http"
	"os/signal"
	"syscall"

	"github.com/mstarzak/phytally/phytally-hub/internal/api"
	"github.com/mstarzak/phytally/phytally-hub/internal/config"
	"github.com/mstarzak/phytally/phytally-hub/internal/hub"
	"github.com/mstarzak/phytally/phytally-hub/internal/radio"
	"github.com/mstarzak/phytally/phytally-hub/internal/radio/hostapd"
	"github.com/mstarzak/phytally/phytally-hub/internal/radio/rawinject"
	"github.com/mstarzak/phytally/phytally-hub/internal/radio/stub"
)

func main() {
	configPath := flag.String("config", "./phytally-hub.json", "Path to the hub config file")
	flag.Parse()

	cfg, err := config.Load(*configPath)
	if err != nil {
		log.Fatalf("load config: %v", err)
	}

	radioBackend, err := radioBackendFromConfig(cfg)
	if err != nil {
		log.Fatalf("radio backend: %v", err)
	}
	app := hub.NewWithFactory(cfg, *configPath, radioBackend, radioBackendFromConfig)

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	if err := app.Start(ctx); err != nil {
		log.Fatalf("start hub: %v", err)
	}

	server := api.New(cfg.ListenAddr, app)

	log.Printf("PhyTally hub listening on %s", cfg.ListenAddr)
	if err := server.ListenAndServe(ctx); err != nil && err != http.ErrServerClosed {
		log.Fatalf("serve: %v", err)
	}
}

func radioBackendFromConfig(cfg config.Config) (radio.Backend, error) {
	switch cfg.RadioBackend {
	case "", "stub":
		return stub.New(cfg.RadioInterface, cfg.RadioChannel, cfg.APSSID), nil
	case "hostapd":
		return hostapd.New(hostapd.Config{
			Interface:        cfg.RadioInterface,
			Channel:          cfg.RadioChannel,
			SSID:             cfg.APSSID,
			BinaryPath:       cfg.HostapdPath,
			CLIPath:          cfg.HostapdCLIPath,
			ControlDir:       cfg.HostapdControlDir,
			ConfigPath:       cfg.HostapdConfigPath,
			MonitorInterface: cfg.MonitorInterface,
			IWPath:           cfg.IWPath,
			IPPath:           cfg.IPPath,
		}), nil
	case "rawinject":
		return rawinject.New(rawinject.Config{
			Interface:        cfg.RadioInterface,
			MonitorInterface: cfg.MonitorInterface,
			Channel:          cfg.RadioChannel,
			SSID:             cfg.APSSID,
			TxRate500Kbps:    cfg.TxRate500Kbps,
			IWPath:           cfg.IWPath,
			IPPath:           cfg.IPPath,
		}), nil
	default:
		return nil, fmt.Errorf("unknown radio backend %q", cfg.RadioBackend)
	}
}
