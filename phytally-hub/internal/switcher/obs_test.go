package switcher

import "testing"

func TestSceneUUIDToTallyID(t *testing.T) {
	t.Parallel()

	scenes := []obsScene{
		{Name: "Scena 5", UUID: "uuid-5", Index: 0},
		{Name: "Scena 3", UUID: "uuid-3", Index: 1},
		{Name: "Wide", UUID: "uuid-wide", Index: 2},
	}

	if got := sceneUUIDToTallyID("uuid-5", scenes); got != 3 {
		t.Fatalf("expected first scene to map to highest tally in the reversed order, got %d", got)
	}
	if got := sceneUUIDToTallyID("uuid-wide", scenes); got != 1 {
		t.Fatalf("expected last scene to map to tally 1, got %d", got)
	}
	if got := sceneUUIDToTallyID("missing", scenes); got != 0 {
		t.Fatalf("expected unknown scene to map to 0, got %d", got)
	}
}

func TestOBSAuthResponse(t *testing.T) {
	t.Parallel()

	got := obsAuthResponse("secret", "salt", "challenge")
	if got == "" {
		t.Fatalf("expected non-empty auth response")
	}
}

func TestScenesFromOBSResponseOrdersBySceneIndex(t *testing.T) {
	t.Parallel()

	response := map[string]any{
		"scenes": []any{
			map[string]any{"sceneIndex": float64(2), "sceneName": "Bottom", "sceneUuid": "uuid-bottom"},
			map[string]any{"sceneIndex": float64(0), "sceneName": "Top", "sceneUuid": "uuid-top"},
			map[string]any{"sceneIndex": float64(1), "sceneName": "Middle", "sceneUuid": "uuid-middle"},
		},
	}

	scenes, err := scenesFromOBSResponse(response)
	if err != nil {
		t.Fatalf("unexpected error: %v", err)
	}
	if len(scenes) != 3 {
		t.Fatalf("expected 3 scenes, got %d", len(scenes))
	}
	if scenes[0].Name != "Top" || scenes[1].Name != "Middle" || scenes[2].Name != "Bottom" {
		t.Fatalf("unexpected scene order: %#v", scenes)
	}
}
