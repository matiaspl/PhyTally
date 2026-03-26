package api

import (
	"context"
	"embed"
	"encoding/json"
	"errors"
	"net/http"
	"os"
	"reflect"
	"time"

	"github.com/mstarzak/phytally/phytally-hub/internal/hub"
	"github.com/mstarzak/phytally/phytally-hub/internal/wifisurvey"
)

//go:embed web/*
var embeddedAssets embed.FS

type Server struct {
	app    *hub.Hub
	server *http.Server
}

func New(listenAddr string, app *hub.Hub) *Server {
	mux := http.NewServeMux()
	s := &Server{
		app: app,
		server: &http.Server{
			Addr:              listenAddr,
			Handler:           mux,
			ReadHeaderTimeout: 5 * time.Second,
		},
	}

	mux.HandleFunc("/", s.handleIndex)
	mux.HandleFunc("/dashboard.css", s.handleStaticFile("dashboard.css", "text/css; charset=utf-8"))
	mux.HandleFunc("/dashboard.js", s.handleStaticFile("dashboard.js", "application/javascript; charset=utf-8"))
	mux.HandleFunc("/api/v1", s.handleDiscovery)
	mux.HandleFunc("/api/v1/events", s.handleEvents)
	mux.HandleFunc("/api/v1/status", s.handleStatus)
	mux.HandleFunc("/api/v1/config", s.handleConfig)
	mux.HandleFunc("/api/v1/wireless/transport", s.handleWirelessTransport)
	mux.HandleFunc("/api/v1/receivers", s.handleReceivers)
	mux.HandleFunc("/api/v1/simulation", s.handleSimulation)
	mux.HandleFunc("/api/v1/tallies/set", s.handleSetTally)
	mux.HandleFunc("/api/v1/receivers/assign-id", s.handleAssignID)
	mux.HandleFunc("/api/v1/receivers/set-brightness", s.handleSetBrightness)
	mux.HandleFunc("/api/v1/receivers/identify", s.handleIdentify)
	mux.HandleFunc("/api/v1/wifi/survey", s.handleSurvey)

	return s
}

func (s *Server) ListenAndServe(ctx context.Context) error {
	errCh := make(chan error, 1)

	go func() {
		<-ctx.Done()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = s.server.Shutdown(shutdownCtx)
	}()

	go func() {
		errCh <- s.server.ListenAndServe()
	}()

	return <-errCh
}

func (s *Server) handleDiscovery(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeJSONError(w, http.StatusMethodNotAllowed, "Method not allowed.")
		return
	}
	writeJSON(w, http.StatusOK, s.app.Discovery())
}

func (s *Server) handleStatus(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeJSONError(w, http.StatusMethodNotAllowed, "Method not allowed.")
		return
	}
	writeJSON(w, http.StatusOK, s.app.Status())
}

func (s *Server) handleEvents(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeJSONError(w, http.StatusMethodNotAllowed, "Method not allowed.")
		return
	}

	flusher, ok := w.(http.Flusher)
	if !ok {
		writeJSONError(w, http.StatusInternalServerError, "Streaming is not supported.")
		return
	}

	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache, no-store, must-revalidate")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("X-Accel-Buffering", "no")

	subscriptionID, updates := s.app.SubscribeUpdates()
	defer s.app.UnsubscribeUpdates(subscriptionID)

	if err := writeSSEEvent(w, "status", s.app.Status()); err != nil {
		return
	}
	if survey := s.app.WiFiSurvey(); shouldSendInitialSurvey(survey) {
		if err := writeSSEEvent(w, "survey", survey); err != nil {
			return
		}
	}
	flusher.Flush()

	keepalive := time.NewTicker(20 * time.Second)
	defer keepalive.Stop()

	for {
		select {
		case <-r.Context().Done():
			return
		case update, ok := <-updates:
			if !ok {
				return
			}
			if update.Status {
				if err := writeSSEEvent(w, "status", s.app.Status()); err != nil {
					return
				}
			}
			if update.Survey {
				if err := writeSSEEvent(w, "survey", s.app.WiFiSurvey()); err != nil {
					return
				}
			}
			flusher.Flush()
		case <-keepalive.C:
			if _, err := w.Write([]byte(": keepalive\n\n")); err != nil {
				return
			}
			flusher.Flush()
		}
	}
}

func (s *Server) handleConfig(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		writeJSON(w, http.StatusOK, s.app.Config())
	case http.MethodPut:
		var payload map[string]any
		if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
			writeJSONError(w, http.StatusBadRequest, "Invalid JSON body.")
			return
		}
		if err := s.app.UpdateConfig(payload); err != nil {
			writeJSONError(w, http.StatusInternalServerError, err.Error())
			return
		}
		writeJSON(w, http.StatusAccepted, map[string]any{
			"status":  "accepted",
			"message": "Configuration saved and applied.",
		})
	default:
		writeJSONError(w, http.StatusMethodNotAllowed, "Method not allowed.")
	}
}

func (s *Server) handleWirelessTransport(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		writeJSON(w, http.StatusOK, s.app.WirelessTransport())
	case http.MethodPost:
		var payload struct {
			Transport string `json:"transport"`
		}
		if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
			writeJSONError(w, http.StatusBadRequest, "Invalid JSON body.")
			return
		}
		if payload.Transport == "" {
			writeJSONError(w, http.StatusBadRequest, "Body must contain string field 'transport'.")
			return
		}
		if err := s.app.SetWirelessTransport(payload.Transport); err != nil {
			writeJSONError(w, http.StatusBadRequest, err.Error())
			return
		}
		writeJSON(w, http.StatusOK, map[string]any{
			"status":    "ok",
			"message":   "Wireless transport updated.",
			"transport": s.app.WirelessTransport(),
		})
	default:
		writeJSONError(w, http.StatusMethodNotAllowed, "Method not allowed.")
	}
}

func (s *Server) handleReceivers(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeJSONError(w, http.StatusMethodNotAllowed, "Method not allowed.")
		return
	}
	writeJSON(w, http.StatusOK, s.app.Receivers())
}

func (s *Server) handleSimulation(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSONError(w, http.StatusMethodNotAllowed, "Method not allowed.")
		return
	}
	var payload struct {
		Enabled *bool `json:"enabled"`
	}
	if err := json.NewDecoder(r.Body).Decode(&payload); err != nil || payload.Enabled == nil {
		writeJSONError(w, http.StatusBadRequest, "Body must contain boolean field 'enabled'.")
		return
	}
	s.app.SetSimulation(*payload.Enabled)
	writeJSON(w, http.StatusOK, map[string]any{
		"status":  "ok",
		"message": simulationMessage(*payload.Enabled),
	})
}

func (s *Server) handleSetTally(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSONError(w, http.StatusMethodNotAllowed, "Method not allowed.")
		return
	}
	var payload struct {
		TallyID int   `json:"tally_id"`
		On      *bool `json:"on"`
	}
	if err := json.NewDecoder(r.Body).Decode(&payload); err != nil || payload.On == nil {
		writeJSONError(w, http.StatusBadRequest, "Body must contain integer field 'tally_id' and boolean field 'on'.")
		return
	}
	if err := s.app.SetSingleTally(payload.TallyID, *payload.On); err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"status":   "ok",
		"message":  singleTallyMessage(payload.TallyID, *payload.On),
		"tally_id": payload.TallyID,
		"on":       *payload.On,
	})
}

func (s *Server) handleAssignID(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSONError(w, http.StatusMethodNotAllowed, "Method not allowed.")
		return
	}
	var payload struct {
		MAC     string `json:"mac"`
		TallyID int    `json:"tally_id"`
	}
	if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
		writeJSONError(w, http.StatusBadRequest, "Invalid JSON body.")
		return
	}
	if err := s.app.QueueAssignID(payload.MAC, payload.TallyID); err != nil {
		writeJSONError(w, http.StatusNotImplemented, err.Error())
		return
	}
	writeJSON(w, http.StatusAccepted, map[string]any{
		"status":   "queued",
		"message":  "SET_ID queued.",
		"mac":      payload.MAC,
		"tally_id": payload.TallyID,
	})
}

func (s *Server) handleSetBrightness(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSONError(w, http.StatusMethodNotAllowed, "Method not allowed.")
		return
	}
	var payload struct {
		MAC               string `json:"mac"`
		BrightnessPercent int    `json:"brightness_percent"`
	}
	if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
		writeJSONError(w, http.StatusBadRequest, "Invalid JSON body.")
		return
	}
	if err := s.app.QueueSetBrightness(payload.MAC, payload.BrightnessPercent); err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusAccepted, map[string]any{
		"status":             "queued",
		"message":            "Brightness update queued.",
		"mac":                payload.MAC,
		"brightness_percent": payload.BrightnessPercent,
	})
}

func (s *Server) handleIdentify(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeJSONError(w, http.StatusMethodNotAllowed, "Method not allowed.")
		return
	}
	var payload struct {
		MAC string `json:"mac"`
	}
	if err := json.NewDecoder(r.Body).Decode(&payload); err != nil {
		writeJSONError(w, http.StatusBadRequest, "Invalid JSON body.")
		return
	}
	if err := s.app.QueueIdentify(payload.MAC); err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusAccepted, map[string]any{
		"status":  "queued",
		"message": "Identify command queued.",
		"mac":     payload.MAC,
	})
}

func (s *Server) handleSurvey(w http.ResponseWriter, r *http.Request) {
	switch r.Method {
	case http.MethodGet:
		writeJSON(w, http.StatusOK, s.app.WiFiSurvey())
	case http.MethodPost:
		if err := s.app.StartWiFiSurvey(); err != nil {
			writeJSONError(w, http.StatusInternalServerError, err.Error())
			return
		}
		writeJSON(w, http.StatusAccepted, map[string]any{
			"status":  "accepted",
			"message": "Wi-Fi survey queued.",
			"survey":  s.app.WiFiSurvey(),
		})
	default:
		writeJSONError(w, http.StatusMethodNotAllowed, "Method not allowed.")
	}
}

func (s *Server) handleIndex(w http.ResponseWriter, r *http.Request) {
	if r.URL.Path != "/" {
		writeJSONError(w, http.StatusNotFound, "Not found.")
		return
	}
	s.serveStaticAsset(w, "index.html", "text/html; charset=utf-8")
}

func (s *Server) handleStaticFile(name string, contentType string) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		s.serveStaticAsset(w, name, contentType)
	}
}

func (s *Server) serveStaticAsset(w http.ResponseWriter, name string, contentType string) {
	if s.app.StaticDir() != "" {
		path := s.app.StaticDir() + string(os.PathSeparator) + name
		data, err := os.ReadFile(path)
		if err == nil {
			w.Header().Set("Content-Type", contentType)
			_, _ = w.Write(data)
			return
		}
		if !errors.Is(err, os.ErrNotExist) {
			writeJSONError(w, http.StatusInternalServerError, err.Error())
			return
		}
	}

	data, err := embeddedAssets.ReadFile("web/" + name)
	if err != nil {
		writeJSONError(w, http.StatusNotFound, "Static asset not found.")
		return
	}

	w.Header().Set("Content-Type", contentType)
	_, _ = w.Write(data)
}

func simulationMessage(enabled bool) string {
	if enabled {
		return "Simulation enabled."
	}
	return "Simulation disabled."
}

func singleTallyMessage(tallyID int, enabled bool) string {
	if enabled {
		return "Tally set to program."
	}
	return "Tally turned off."
}

func writeJSON(w http.ResponseWriter, status int, payload any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(payload)
}

func writeJSONError(w http.ResponseWriter, status int, message string) {
	writeJSON(w, status, map[string]any{
		"status":  "error",
		"message": message,
	})
}

func shouldSendInitialSurvey(survey map[string]any) bool {
	if scanning, _ := survey["scanning"].(bool); scanning {
		return true
	}
	if errorText, _ := survey["error"].(string); errorText != "" {
		return true
	}
	if count, ok := survey["network_count"].(int); ok && count > 0 {
		return true
	}
	if startedAt, ok := survey["scan_started_at_ms"].(int64); ok && startedAt > 0 {
		return true
	}
	if scannedAt, ok := survey["scanned_at_ms"].(int64); ok && scannedAt > 0 {
		return true
	}
	if suggestion := survey["channel_suggestion"]; !isNilSurveyValue(suggestion) {
		return true
	}
	if channels, ok := survey["spectral_channels"].([]wifisurvey.SpectralChannel); ok && len(channels) > 0 {
		return true
	}
	if rows, ok := survey["spectral_rows"].([]wifisurvey.SpectralRow); ok && len(rows) > 0 {
		return true
	}
	return false
}

func isNilSurveyValue(value any) bool {
	if value == nil {
		return true
	}
	reflected := reflect.ValueOf(value)
	switch reflected.Kind() {
	case reflect.Chan, reflect.Func, reflect.Interface, reflect.Map, reflect.Pointer, reflect.Slice:
		return reflected.IsNil()
	default:
		return false
	}
}

func writeSSEEvent(w http.ResponseWriter, name string, payload any) error {
	data, err := json.Marshal(payload)
	if err != nil {
		return err
	}
	if _, err := w.Write([]byte("event: " + name + "\n")); err != nil {
		return err
	}
	if _, err := w.Write([]byte("data: ")); err != nil {
		return err
	}
	if _, err := w.Write(data); err != nil {
		return err
	}
	_, err = w.Write([]byte("\n\n"))
	return err
}
