package main

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestProcessRequestJSONMutatesNestedHeapGraph(t *testing.T) {
	input := []byte(`{"counters":{"in":1,"ok":2},"events":[{"seq":7,"score":1.5,"user":{"attrs":{"credits":2.5}},"custom":{"product_ids":[1,2]}}]}`)
	out, err := processRequestJSON(input, 41)
	if err != nil {
		t.Fatalf("processRequestJSON failed: %v", err)
	}

	var root map[string]any
	if err := json.Unmarshal(out, &root); err != nil {
		t.Fatalf("unmarshal output failed: %v", err)
	}

	counters := root["counters"].(map[string]any)
	if counters["in"].(float64) != 2 || counters["ok"].(float64) != 3 {
		t.Fatalf("unexpected counters: %#v", counters)
	}
	if root["processed_seq"].(float64) != 41 {
		t.Fatalf("unexpected processed_seq: %#v", root["processed_seq"])
	}
	if !root["server_ok"].(bool) {
		t.Fatal("expected server_ok=true")
	}

	event := root["events"].([]any)[0].(map[string]any)
	if event["seq"].(float64) != 8 {
		t.Fatalf("unexpected seq: %#v", event["seq"])
	}
	if event["score"].(float64) != 3.0 {
		t.Fatalf("unexpected score: %#v", event["score"])
	}
	credits := event["user"].(map[string]any)["attrs"].(map[string]any)["credits"].(float64)
	if credits != 5.0 {
		t.Fatalf("unexpected credits: %#v", credits)
	}
	ids := event["custom"].(map[string]any)["product_ids"].([]any)
	if len(ids) != 3 || ids[2].(float64) != 9999 {
		t.Fatalf("unexpected product_ids: %#v", ids)
	}
	if event["server_marker"].(string) != "m-0" {
		t.Fatalf("unexpected server_marker: %#v", event["server_marker"])
	}
}

func TestRunSyncLoadHitsHTTPServerAndReportsSuccess(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("content-type", "application/json")
		_, _ = w.Write([]byte(`{"server_ok":true}`))
	}))
	defer server.Close()

	cfg := benchConfig{
		PayloadName:      "test",
		Concurrency:      2,
		Duration:         150 * time.Millisecond,
		ClientTimeout:    time.Second,
		ClientMode:       "sync",
		ProgressInterval: 0,
	}

	ctx, cancel := context.WithTimeout(context.Background(), cfg.Duration)
	defer cancel()
	stats := runSyncLoad(ctx, cfg, server.URL+"/process", []byte(`{"hello":"world"}`))

	if stats.Total == 0 {
		t.Fatal("expected at least one request")
	}
	if stats.Fail != 0 {
		t.Fatalf("expected no failures, got %+v", stats)
	}
	if stats.OK == 0 {
		t.Fatalf("expected successful requests, got %+v", stats)
	}
}
