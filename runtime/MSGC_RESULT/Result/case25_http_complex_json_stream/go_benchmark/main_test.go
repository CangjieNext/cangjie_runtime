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

func TestPersistentBudgetUsesBalancedRootsAndRareLargeObjects(t *testing.T) {
	// Each of the 16 static/stack worker shards must exceed the 64 KiB
	// large-object quantization threshold after the 1% split.
	const totalBytes = int64(1 << 30)
	layout, err := newPersistentLayout(totalBytes, defaultPersistentWorkers)
	if err != nil {
		t.Fatalf("newPersistentLayout failed: %v", err)
	}
	if layout.TotalBytes != totalBytes {
		t.Fatalf("unexpected total bytes: %d", layout.TotalBytes)
	}
	if layout.StaticBytes != totalBytes/2 ||
		layout.StackBytes != totalBytes/2 {
		t.Fatalf("expected balanced roots, got static=%d stack=%d",
			layout.StaticBytes, layout.StackBytes)
	}
	if layout.LargeBytes*1000 < layout.TotalBytes*9 ||
		layout.LargeBytes*100 > layout.TotalBytes {
		t.Fatalf("expected approximately 1%% large objects, got small=%d large=%d",
			layout.SmallBytes, layout.LargeBytes)
	}
	if layout.Workers != defaultPersistentWorkers {
		t.Fatalf("unexpected workers: %d", layout.Workers)
	}
}

func TestPersistentBaselineRetainsExactBytesUntilStopped(t *testing.T) {
	const totalBytes = int64(64 << 20)
	baseline, err := startPersistentBaseline(totalBytes, 2)
	if err != nil {
		t.Fatalf("startPersistentBaseline failed: %v", err)
	}
	defer baseline.Stop()

	stats := baseline.Stats()
	if stats.TotalBytes != totalBytes {
		t.Fatalf("expected %d retained bytes, got %+v", totalBytes, stats)
	}
	if stats.StaticBytes+stats.StackBytes != totalBytes {
		t.Fatalf("root byte accounting mismatch: %+v", stats)
	}
	if stats.StaticBytes == 0 || stats.StackBytes == 0 {
		t.Fatalf("both root kinds must retain objects: %+v", stats)
	}
	if len(persistentStaticRootShards) != 2 {
		t.Fatalf("expected one static root shard per worker, got %d",
			len(persistentStaticRootShards))
	}
	allowed := make(map[int]bool)
	for _, size := range persistentSmallObjectSizes {
		allowed[size] = true
	}
	for _, size := range persistentLargeObjectSizes {
		allowed[size] = true
	}
	sawLargeObject := false
	for shardIndex, shard := range persistentStaticRootShards {
		for objectIndex, object := range shard {
			if !allowed[len(object)] {
				t.Fatalf("unexpected object size %d at shard=%d object=%d",
					len(object), shardIndex, objectIndex)
			}
			if len(object) > 16<<10 {
				sawLargeObject = true
			}
		}
	}
	if !sawLargeObject {
		t.Fatal("expected the smoke baseline to allocate declared large objects")
	}
}
