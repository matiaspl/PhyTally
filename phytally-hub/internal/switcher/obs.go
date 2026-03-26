package switcher

import (
	"context"
	"crypto/sha256"
	"encoding/base64"
	"errors"
	"fmt"
	"net"
	"net/url"
	"sort"
	"strings"
	"time"

	"github.com/mstarzak/phytally/phytally-hub/internal/protocol"
)

const (
	obsEventSubscriptionGeneral = 1 << 0
	obsEventSubscriptionScenes  = 1 << 2
)

func normalizeOBSURL(raw string) string {
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return ""
	}
	if strings.Contains(raw, "://") {
		return raw
	}
	return "ws://" + raw
}

func obsAuthResponse(password, salt, challenge string) string {
	first := sha256.Sum256([]byte(password + salt))
	secret := base64.StdEncoding.EncodeToString(first[:])
	second := sha256.Sum256([]byte(secret + challenge))
	return base64.StdEncoding.EncodeToString(second[:])
}

func runOBS(ctx context.Context, rawURL string, password string, tallyCb TallyCallback, statusCb StatusCallback) {
	endpoint := normalizeOBSURL(rawURL)
	if endpoint == "" {
		statusCb("obs", false, "empty URL")
		return
	}

	for {
		select {
		case <-ctx.Done():
			return
		default:
		}

		client, err := dialOBS(ctx, endpoint, password)
		if err != nil {
			statusCb("obs", false, err.Error())
			if !sleepContext(ctx, 3*time.Second) {
				return
			}
			continue
		}

		statusCb("obs", true, "")

		for {
			tallies, err := client.pollTallies()
			if err != nil {
				statusCb("obs", false, err.Error())
				client.Close()
				break
			}
			tallyCb("obs", tallies)

			err = client.waitForNotification()
			if err != nil {
				if ctx.Err() != nil || errors.Is(err, context.Canceled) {
					client.Close()
					return
				}
				statusCb("obs", false, err.Error())
				client.Close()
				break
			}
		}

		if !sleepContext(ctx, 2*time.Second) {
			return
		}
	}
}

type obsClient struct {
	ws            *wsConn
	nextRequestID int
}

type obsScene struct {
	Name  string
	UUID  string
	Index int
}

func dialOBS(ctx context.Context, endpoint string, password string) (*obsClient, error) {
	parsed, err := url.Parse(endpoint)
	if err != nil {
		return nil, err
	}

	ws, err := dialWebsocket(ctx, parsed)
	if err != nil {
		return nil, err
	}

	client := &obsClient{ws: ws}

	var hello struct {
		Op int `json:"op"`
		D  struct {
			RPCVersion     int `json:"rpcVersion"`
			Authentication *struct {
				Challenge string `json:"challenge"`
				Salt      string `json:"salt"`
			} `json:"authentication"`
		} `json:"d"`
	}
	if err := client.ws.readJSON(&hello); err != nil {
		ws.Close()
		return nil, err
	}
	if hello.Op != 0 {
		ws.Close()
		return nil, fmt.Errorf("unexpected OBS hello opcode %d", hello.Op)
	}

	identify := map[string]any{
		"op": 1,
		"d": map[string]any{
			"rpcVersion":         1,
			"eventSubscriptions": obsEventSubscriptionGeneral | obsEventSubscriptionScenes,
		},
	}
	if hello.D.Authentication != nil {
		identify["d"].(map[string]any)["authentication"] = obsAuthResponse(password, hello.D.Authentication.Salt, hello.D.Authentication.Challenge)
	}
	if err := client.ws.writeJSON(identify); err != nil {
		ws.Close()
		return nil, err
	}

	var identified struct {
		Op int `json:"op"`
	}
	if err := client.ws.readJSON(&identified); err != nil {
		ws.Close()
		return nil, err
	}
	if identified.Op != 2 {
		ws.Close()
		return nil, fmt.Errorf("unexpected OBS identify response opcode %d", identified.Op)
	}

	return client, nil
}

func (c *obsClient) Close() {
	if c.ws != nil {
		c.ws.Close()
	}
}

func (c *obsClient) pollTallies() ([protocol.MaxTallies]protocol.TallyState, error) {
	var tallies [protocol.MaxTallies]protocol.TallyState

	scenes, programSceneUUID, previewSceneUUID, err := c.getSceneSnapshot()
	if err != nil {
		return tallies, err
	}

	if tallyID := sceneUUIDToTallyID(programSceneUUID, scenes); tallyID >= 1 {
		tallies[tallyID-1] = protocol.TallyProgram
	}

	if tallyID := sceneUUIDToTallyID(previewSceneUUID, scenes); tallyID >= 1 && tallies[tallyID-1] != protocol.TallyProgram {
		tallies[tallyID-1] = protocol.TallyPreview
	}

	return tallies, nil
}

func (c *obsClient) waitForNotification() error {
	for {
		var message struct {
			Op int `json:"op"`
			D  struct {
				EventType string `json:"eventType"`
			} `json:"d"`
		}
		if err := c.ws.readJSON(&message); err != nil {
			if netErr, ok := err.(net.Error); ok && netErr.Timeout() {
				continue
			}
			return err
		}
		if message.Op == 5 {
			return nil
		}
	}
}

func (c *obsClient) getSceneSnapshot() ([]obsScene, string, string, error) {
	response, err := c.request("GetSceneList", nil)
	if err != nil {
		return nil, "", "", err
	}
	scenes, err := scenesFromOBSResponse(response)
	if err != nil {
		return nil, "", "", err
	}
	programSceneUUID, _ := response["currentProgramSceneUuid"].(string)
	previewSceneUUID, _ := response["currentPreviewSceneUuid"].(string)
	if programSceneUUID == "" {
		return nil, "", "", fmt.Errorf("OBS GetSceneList response missing currentProgramSceneUuid")
	}
	return scenes, programSceneUUID, previewSceneUUID, nil
}

func sceneUUIDToTallyID(sceneUUID string, scenes []obsScene) int {
	for index, scene := range scenes {
		if scene.UUID != sceneUUID {
			continue
		}
		reverseIndex := len(scenes) - 1 - index
		if reverseIndex >= 0 && reverseIndex < protocol.MaxTallies {
			return reverseIndex + 1
		}
	}

	return 0
}

func scenesFromOBSResponse(response map[string]any) ([]obsScene, error) {
	scenesRaw, ok := response["scenes"].([]any)
	if !ok {
		return nil, fmt.Errorf("OBS GetSceneList response missing scenes")
	}

	entries := make([]obsScene, 0, len(scenesRaw))
	for _, sceneRaw := range scenesRaw {
		scene, ok := sceneRaw.(map[string]any)
		if !ok {
			continue
		}
		name, _ := scene["sceneName"].(string)
		uuid, _ := scene["sceneUuid"].(string)
		if name == "" || uuid == "" {
			continue
		}
		index := len(entries)
		if rawIndex, ok := scene["sceneIndex"].(float64); ok {
			index = int(rawIndex)
		}
		entries = append(entries, obsScene{Name: name, UUID: uuid, Index: index})
	}

	sort.SliceStable(entries, func(i, j int) bool {
		return entries[i].Index < entries[j].Index
	})

	return entries, nil
}

func (c *obsClient) request(requestType string, requestData map[string]any) (map[string]any, error) {
	c.nextRequestID++
	requestID := fmt.Sprintf("%d", c.nextRequestID)

	payload := map[string]any{
		"op": 6,
		"d": map[string]any{
			"requestType": requestType,
			"requestId":   requestID,
		},
	}
	if requestData != nil {
		payload["d"].(map[string]any)["requestData"] = requestData
	}

	if err := c.ws.writeJSON(payload); err != nil {
		return nil, err
	}

	for {
		var response struct {
			Op int `json:"op"`
			D  struct {
				RequestID     string         `json:"requestId"`
				RequestStatus map[string]any `json:"requestStatus"`
				ResponseData  map[string]any `json:"responseData"`
			} `json:"d"`
		}
		if err := c.ws.readJSON(&response); err != nil {
			return nil, err
		}
		if response.Op != 7 || response.D.RequestID != requestID {
			continue
		}
		result, _ := response.D.RequestStatus["result"].(bool)
		if !result {
			comment, _ := response.D.RequestStatus["comment"].(string)
			if comment == "" {
				comment = "request failed"
			}
			return nil, fmt.Errorf("OBS %s: %s", requestType, comment)
		}
		return response.D.ResponseData, nil
	}
}
