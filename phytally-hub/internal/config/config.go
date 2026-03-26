package config

import (
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"runtime"
)

type Config struct {
	ListenAddr        string `json:"listen_addr"`
	StaticDir         string `json:"static_dir"`
	EthernetInterface string `json:"ethernet_interface"`
	RadioInterface    string `json:"radio_interface"`
	RadioChannel      int    `json:"radio_channel"`
	RadioBackend      string `json:"radio_backend"`
	MonitorInterface  string `json:"monitor_interface"`
	TxRate500Kbps     int    `json:"tx_rate_500kbps"`
	APSSID            string `json:"ap_ssid"`
	HostapdPath       string `json:"hostapd_path"`
	HostapdCLIPath    string `json:"hostapd_cli_path"`
	HostapdControlDir string `json:"hostapd_control_dir"`
	HostapdConfigPath string `json:"hostapd_config_path"`
	IWPath            string `json:"iw_path"`
	IPPath            string `json:"ip_path"`
	DebugFSRoot       string `json:"debugfs_root"`
	AtemIP            string `json:"atem_ip"`
	AtemEnabled       bool   `json:"atem_enabled"`
	VMixIP            string `json:"vmix_ip"`
	VMixEnabled       bool   `json:"vmix_enabled"`
	TSLListenAddr     string `json:"tsl_listen_addr"`
	TSLEnabled        bool   `json:"tsl_enabled"`
	OBSURL            string `json:"obs_url"`
	OBSPassword       string `json:"obs_password"`
	OBSEnabled        bool   `json:"obs_enabled"`
}

func Default() Config {
	return Config{
		ListenAddr:        ":8080",
		StaticDir:         "",
		EthernetInterface: "eth0",
		RadioInterface:    "wlan1",
		RadioChannel:      1,
		RadioBackend:      defaultRadioBackend(),
		MonitorInterface:  "phytallymon0",
		TxRate500Kbps:     2,
		APSSID:            "PhyTally",
		HostapdPath:       "hostapd",
		HostapdCLIPath:    "hostapd_cli",
		HostapdControlDir: "/tmp/phytally-hostapd",
		HostapdConfigPath: "/tmp/phytally-hostapd/hostapd.conf",
		IWPath:            "iw",
		IPPath:            "ip",
		DebugFSRoot:       "/sys/kernel/debug/ieee80211",
		AtemIP:            "0.0.0.0",
		AtemEnabled:       false,
		VMixIP:            "0.0.0.0",
		VMixEnabled:       false,
		TSLListenAddr:     ":9800",
		TSLEnabled:        false,
		OBSURL:            "ws://127.0.0.1:4455",
		OBSPassword:       "",
		OBSEnabled:        false,
	}
}

func defaultRadioBackend() string {
	if runtime.GOOS == "linux" {
		return "hostapd"
	}
	return "stub"
}

func Load(path string) (Config, error) {
	cfg := Default()

	data, err := os.ReadFile(path)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return cfg, nil
		}
		return Config{}, err
	}

	if err := json.Unmarshal(data, &cfg); err != nil {
		return Config{}, err
	}

	if cfg.ListenAddr == "" {
		cfg.ListenAddr = ":8080"
	}
	if cfg.EthernetInterface == "" {
		cfg.EthernetInterface = "eth0"
	}
	if cfg.RadioInterface == "" {
		cfg.RadioInterface = "wlan1"
	}
	if cfg.RadioChannel < 1 || cfg.RadioChannel > 13 {
		cfg.RadioChannel = 1
	}
	if cfg.RadioBackend == "" {
		cfg.RadioBackend = defaultRadioBackend()
	}
	if cfg.MonitorInterface == "" {
		cfg.MonitorInterface = "phytallymon0"
	}
	if cfg.TxRate500Kbps <= 0 {
		cfg.TxRate500Kbps = 2
	}
	if cfg.APSSID == "" {
		cfg.APSSID = "PhyTally"
	}
	if cfg.HostapdPath == "" {
		cfg.HostapdPath = "hostapd"
	}
	if cfg.HostapdCLIPath == "" {
		cfg.HostapdCLIPath = "hostapd_cli"
	}
	if cfg.HostapdControlDir == "" {
		cfg.HostapdControlDir = "/tmp/phytally-hostapd"
	}
	if cfg.HostapdConfigPath == "" {
		cfg.HostapdConfigPath = "/tmp/phytally-hostapd/hostapd.conf"
	}
	if cfg.IWPath == "" {
		cfg.IWPath = "iw"
	}
	if cfg.IPPath == "" {
		cfg.IPPath = "ip"
	}
	if cfg.DebugFSRoot == "" {
		cfg.DebugFSRoot = "/sys/kernel/debug/ieee80211"
	}
	if cfg.TSLListenAddr == "" {
		cfg.TSLListenAddr = ":9800"
	}

	return cfg, nil
}

func Save(path string, cfg Config) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil && filepath.Dir(path) != "." {
		return err
	}

	data, err := json.MarshalIndent(cfg, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	return os.WriteFile(path, data, 0o644)
}
