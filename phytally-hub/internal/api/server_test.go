package api

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/mstarzak/phytally/phytally-hub/internal/config"
	"github.com/mstarzak/phytally/phytally-hub/internal/hub"
	"github.com/mstarzak/phytally/phytally-hub/internal/radio/stub"
)

func TestStatusEndpoint(t *testing.T) {
	t.Parallel()

	app := hub.New(config.Config{
		ListenAddr:     ":8080",
		StaticDir:      t.TempDir(),
		RadioInterface: "wlan1",
		RadioChannel:   1,
		APSSID:         "PhyTally",
		AtemIP:         "0.0.0.0",
		VMixIP:         "0.0.0.0",
		OBSURL:         "ws://127.0.0.1:4455",
	}, filepath.Join(t.TempDir(), "hub.json"), stub.New("wlan1", 1, "PhyTally"))

	server := New(":8080", app)
	req := httptest.NewRequest(http.MethodGet, "/api/v1/status", nil)
	rec := httptest.NewRecorder()

	server.server.Handler.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d", rec.Code)
	}

	var payload map[string]any
	if err := json.Unmarshal(rec.Body.Bytes(), &payload); err != nil {
		t.Fatalf("decode response: %v", err)
	}

	network, ok := payload["network"].(map[string]any)
	if !ok {
		t.Fatalf("missing network block")
	}
	if got := network["ap_ssid"]; got != "PhyTally" {
		t.Fatalf("unexpected ap_ssid: %v", got)
	}
}

func TestStaticIndexServed(t *testing.T) {
	t.Parallel()

	staticDir := t.TempDir()
	indexPath := filepath.Join(staticDir, "index.html")
	if err := os.WriteFile(indexPath, []byte("<!doctype html><title>PhyTally</title>"), 0o644); err != nil {
		t.Fatalf("write index: %v", err)
	}

	app := hub.New(config.Config{
		ListenAddr:     ":8080",
		StaticDir:      staticDir,
		RadioInterface: "wlan1",
		RadioChannel:   1,
		APSSID:         "PhyTally",
		AtemIP:         "0.0.0.0",
		VMixIP:         "0.0.0.0",
		OBSURL:         "ws://127.0.0.1:4455",
	}, filepath.Join(t.TempDir(), "hub.json"), stub.New("wlan1", 1, "PhyTally"))

	server := New(":8080", app)
	req := httptest.NewRequest(http.MethodGet, "/", nil)
	rec := httptest.NewRecorder()

	server.server.Handler.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d", rec.Code)
	}
	if got := rec.Body.String(); got != "<!doctype html><title>PhyTally</title>" {
		t.Fatalf("unexpected body: %q", got)
	}
}

func TestEmbeddedIndexServedWhenStaticDirMissing(t *testing.T) {
	t.Parallel()

	app := hub.New(config.Config{
		ListenAddr:     ":8080",
		StaticDir:      filepath.Join(t.TempDir(), "missing"),
		RadioInterface: "wlan1",
		RadioChannel:   1,
		APSSID:         "PhyTally",
		AtemIP:         "0.0.0.0",
		VMixIP:         "0.0.0.0",
		OBSURL:         "ws://127.0.0.1:4455",
	}, filepath.Join(t.TempDir(), "hub.json"), stub.New("wlan1", 1, "PhyTally"))

	server := New(":8080", app)
	req := httptest.NewRequest(http.MethodGet, "/", nil)
	rec := httptest.NewRecorder()

	server.server.Handler.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d", rec.Code)
	}
	if got := rec.Body.String(); got == "" {
		t.Fatalf("expected embedded asset body")
	}
}

func TestSetSingleTallyEndpoint(t *testing.T) {
	t.Parallel()

	app := hub.New(config.Config{
		ListenAddr:     ":8080",
		StaticDir:      t.TempDir(),
		RadioInterface: "wlan1",
		RadioChannel:   1,
		APSSID:         "PhyTally",
		AtemIP:         "0.0.0.0",
		VMixIP:         "0.0.0.0",
		OBSURL:         "ws://127.0.0.1:4455",
	}, filepath.Join(t.TempDir(), "hub.json"), stub.New("wlan1", 1, "PhyTally"))

	server := New(":8080", app)
	req := httptest.NewRequest(http.MethodPost, "/api/v1/tallies/set", bytes.NewBufferString(`{"tally_id":2,"on":true}`))
	rec := httptest.NewRecorder()

	server.server.Handler.ServeHTTP(rec, req)

	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d", rec.Code)
	}

	statusReq := httptest.NewRequest(http.MethodGet, "/api/v1/status", nil)
	statusRec := httptest.NewRecorder()
	server.server.Handler.ServeHTTP(statusRec, statusReq)

	var payload map[string]any
	if err := json.Unmarshal(statusRec.Body.Bytes(), &payload); err != nil {
		t.Fatalf("decode status response: %v", err)
	}

	simulation, ok := payload["simulation"].(map[string]any)
	if !ok || simulation["enabled"] != false {
		t.Fatalf("expected simulation disabled after manual tally set, got %#v", payload["simulation"])
	}

	tallies, ok := payload["tallies"].([]any)
	if !ok {
		t.Fatalf("missing tallies array")
	}
	if got := tallies[1]; got != float64(2) {
		t.Fatalf("expected tally 2 to be program, got %v", got)
	}
}

func TestSetSingleTallyEndpointRejectsInvalidBody(t *testing.T) {
	t.Parallel()

	app := hub.New(config.Config{
		ListenAddr:     ":8080",
		StaticDir:      t.TempDir(),
		RadioInterface: "wlan1",
		RadioChannel:   1,
		APSSID:         "PhyTally",
		AtemIP:         "0.0.0.0",
		VMixIP:         "0.0.0.0",
		OBSURL:         "ws://127.0.0.1:4455",
	}, filepath.Join(t.TempDir(), "hub.json"), stub.New("wlan1", 1, "PhyTally"))

	server := New(":8080", app)
	req := httptest.NewRequest(http.MethodPost, "/api/v1/tallies/set", bytes.NewBufferString(`{"tally_id":0}`))
	rec := httptest.NewRecorder()

	server.server.Handler.ServeHTTP(rec, req)

	if rec.Code != http.StatusBadRequest {
		t.Fatalf("expected 400, got %d", rec.Code)
	}
}

func TestSetBrightnessEndpoint(t *testing.T) {
	t.Parallel()

	app := hub.New(config.Config{
		ListenAddr:     ":8080",
		StaticDir:      t.TempDir(),
		RadioInterface: "wlan1",
		RadioChannel:   1,
		APSSID:         "PhyTally",
		AtemIP:         "0.0.0.0",
		VMixIP:         "0.0.0.0",
		OBSURL:         "ws://127.0.0.1:4455",
	}, filepath.Join(t.TempDir(), "hub.json"), stub.New("wlan1", 1, "PhyTally"))

	server := New(":8080", app)
	req := httptest.NewRequest(http.MethodPost, "/api/v1/receivers/set-brightness", bytes.NewBufferString(`{"mac":"80:7D:3A:C4:40:60","brightness_percent":35}`))
	rec := httptest.NewRecorder()

	server.server.Handler.ServeHTTP(rec, req)

	if rec.Code != http.StatusAccepted {
		t.Fatalf("expected 202, got %d", rec.Code)
	}
}

func TestIdentifyEndpoint(t *testing.T) {
	t.Parallel()

	app := hub.New(config.Config{
		ListenAddr:     ":8080",
		StaticDir:      t.TempDir(),
		RadioInterface: "wlan1",
		RadioChannel:   1,
		APSSID:         "PhyTally",
		AtemIP:         "0.0.0.0",
		VMixIP:         "0.0.0.0",
		OBSURL:         "ws://127.0.0.1:4455",
	}, filepath.Join(t.TempDir(), "hub.json"), stub.New("wlan1", 1, "PhyTally"))

	server := New(":8080", app)
	req := httptest.NewRequest(http.MethodPost, "/api/v1/receivers/identify", bytes.NewBufferString(`{"mac":"80:7D:3A:C4:40:60"}`))
	rec := httptest.NewRecorder()

	server.server.Handler.ServeHTTP(rec, req)

	if rec.Code != http.StatusAccepted {
		t.Fatalf("expected 202, got %d", rec.Code)
	}
}

func TestWirelessTransportEndpoint(t *testing.T) {
	t.Parallel()

	app := hub.New(config.Config{
		ListenAddr:     ":8080",
		StaticDir:      t.TempDir(),
		RadioInterface: "wlan1",
		RadioChannel:   1,
		RadioBackend:   "stub",
		APSSID:         "PhyTally",
		AtemIP:         "0.0.0.0",
		VMixIP:         "0.0.0.0",
		OBSURL:         "ws://127.0.0.1:4455",
	}, filepath.Join(t.TempDir(), "hub.json"), stub.New("wlan1", 1, "PhyTally"))

	server := New(":8080", app)

	postReq := httptest.NewRequest(http.MethodPost, "/api/v1/wireless/transport", bytes.NewBufferString(`{"transport":"rawinject"}`))
	postRec := httptest.NewRecorder()
	server.server.Handler.ServeHTTP(postRec, postReq)

	if postRec.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d", postRec.Code)
	}

	getReq := httptest.NewRequest(http.MethodGet, "/api/v1/wireless/transport", nil)
	getRec := httptest.NewRecorder()
	server.server.Handler.ServeHTTP(getRec, getReq)

	if getRec.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d", getRec.Code)
	}

	var payload map[string]any
	if err := json.Unmarshal(getRec.Body.Bytes(), &payload); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if got := payload["transport"]; got != "rawinject" {
		t.Fatalf("expected rawinject transport, got %v", got)
	}
}

func TestEventsEndpointStreamsStatusUpdates(t *testing.T) {
	t.Parallel()

	app := hub.New(config.Config{
		ListenAddr:     ":8080",
		StaticDir:      t.TempDir(),
		RadioInterface: "wlan1",
		RadioChannel:   1,
		APSSID:         "PhyTally",
		AtemIP:         "0.0.0.0",
		VMixIP:         "0.0.0.0",
		OBSURL:         "ws://127.0.0.1:4455",
	}, filepath.Join(t.TempDir(), "hub.json"), stub.New("wlan1", 1, "PhyTally"))

	server := New(":8080", app)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	readerPipe, writerPipe := io.Pipe()
	defer readerPipe.Close()

	recorder := &streamingResponseWriter{
		header: http.Header{},
		writer: writerPipe,
	}
	req := httptest.NewRequest(http.MethodGet, "/api/v1/events", nil).WithContext(ctx)
	handlerDone := make(chan struct{})
	go func() {
		defer close(handlerDone)
		defer writerPipe.Close()
		server.server.Handler.ServeHTTP(recorder, req)
	}()

	reader := bufio.NewReader(readerPipe)

	name, data := readSSEEvent(t, reader)
	if name != "status" {
		t.Fatalf("expected initial status event, got %q", name)
	}
	if !strings.Contains(data, "\"radio_channel\":1") {
		t.Fatalf("expected radio channel in initial event, got %s", data)
	}

	if err := app.SetSingleTally(2, true); err != nil {
		t.Fatalf("set single tally: %v", err)
	}

	name, data = readSSEEvent(t, reader)
	if name != "status" {
		t.Fatalf("expected status event after tally change, got %q", name)
	}
	if !strings.Contains(data, "\"tallies\":[0,2") {
		t.Fatalf("expected updated tally payload, got %s", data)
	}

	cancel()
	<-handlerDone
}

func readSSEEvent(t *testing.T, reader *bufio.Reader) (string, string) {
	t.Helper()

	name := ""
	data := ""
	for {
		line, err := reader.ReadString('\n')
		if err != nil {
			if err == io.EOF {
				t.Fatal("stream closed before event was received")
			}
			t.Fatalf("read stream: %v", err)
		}

		trimmed := strings.TrimRight(line, "\r\n")
		if trimmed == "" {
			if name != "" {
				return name, data
			}
			continue
		}
		if strings.HasPrefix(trimmed, ":") {
			continue
		}
		if strings.HasPrefix(trimmed, "event: ") {
			name = strings.TrimPrefix(trimmed, "event: ")
			continue
		}
		if strings.HasPrefix(trimmed, "data: ") {
			data = strings.TrimPrefix(trimmed, "data: ")
		}
	}
}

type streamingResponseWriter struct {
	header http.Header
	writer *io.PipeWriter
	mu     sync.Mutex
	status int
}

func (w *streamingResponseWriter) Header() http.Header {
	return w.header
}

func (w *streamingResponseWriter) Write(data []byte) (int, error) {
	return w.writer.Write(data)
}

func (w *streamingResponseWriter) WriteHeader(statusCode int) {
	w.mu.Lock()
	w.status = statusCode
	w.mu.Unlock()
}

func (w *streamingResponseWriter) Flush() {}
