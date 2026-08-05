package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
	"time"
)

const (
	defaultPayloadName             = "10K"
	defaultConcurrency             = 8
	defaultDurationMs              = 60_000
	defaultPersistentBytes   int64 = 5 << 30
	defaultPersistentWorkers       = 8
	defaultServerAddr              = "127.0.0.1"
	defaultServerPort              = 18125
	defaultTimeoutMs               = 5_000
	defaultProgressEvery           = 2 * time.Second
	defaultWarmupReqs              = 20
)

type benchConfig struct {
	PayloadName       string
	Concurrency       int
	Duration          time.Duration
	RunForever        bool
	PayloadDir        string
	ServerAddr        string
	ServerPort        int
	ClientTimeout     time.Duration
	Role              string
	ClientMode        string
	SendInterval      time.Duration
	MaxInflight       int
	ProgressInterval  time.Duration
	PersistentBytes   int64
	PersistentWorkers int
}

type runStats struct {
	Total     int64
	OK        int64
	Fail      int64
	BytesSent int64
	BytesRecv int64
	Inflight  int64
}

type atomicStats struct {
	total     atomic.Int64
	ok        atomic.Int64
	fail      atomic.Int64
	bytesSent atomic.Int64
	bytesRecv atomic.Int64
	inflight  atomic.Int64
}

type asyncSlot struct {
	busy   atomic.Int64
	client *http.Client
	id     int64
}

func (s *atomicStats) snapshot() runStats {
	return runStats{
		Total:     s.total.Load(),
		OK:        s.ok.Load(),
		Fail:      s.fail.Load(),
		BytesSent: s.bytesSent.Load(),
		BytesRecv: s.bytesRecv.Load(),
		Inflight:  s.inflight.Load(),
	}
}

func getEnvStr(key, def string) string {
	if value, ok := os.LookupEnv(key); ok {
		return value
	}
	return def
}

func getEnvInt(key string, def int) int {
	raw, ok := os.LookupEnv(key)
	if !ok {
		return def
	}
	value, err := strconv.Atoi(raw)
	if err != nil {
		return def
	}
	return value
}

func getEnvInt64(key string, def int64) int64 {
	raw, ok := os.LookupEnv(key)
	if !ok {
		return def
	}
	value, err := strconv.ParseInt(raw, 10, 64)
	if err != nil {
		return def
	}
	return value
}

func loadConfigFromEnv() (benchConfig, error) {
	cfg := benchConfig{
		PayloadName:       getEnvStr("CASE27_PAYLOAD", defaultPayloadName),
		Concurrency:       getEnvInt("CASE27_CONCURRENCY", defaultConcurrency),
		Duration:          time.Duration(getEnvInt("CASE27_DURATION_MS", defaultDurationMs)) * time.Millisecond,
		PayloadDir:        getEnvStr("CASE27_PAYLOAD_DIR", "./payloads"),
		ServerAddr:        defaultServerAddr,
		ServerPort:        getEnvInt("CASE27_SERVER_PORT", defaultServerPort),
		ClientTimeout:     time.Duration(getEnvInt("CASE27_CLIENT_TIMEOUT_MS", defaultTimeoutMs)) * time.Millisecond,
		Role:              getEnvStr("CASE27_ROLE", "both"),
		ClientMode:        getEnvStr("CASE27_CLIENT_MODE", "sync"),
		SendInterval:      time.Duration(getEnvInt("CASE27_SEND_INTERVAL_MS", 1)) * time.Millisecond,
		MaxInflight:       getEnvInt("CASE27_MAX_INFLIGHT", 32),
		ProgressInterval:  defaultProgressEvery,
		PersistentBytes:   getEnvInt64("CASE27_PERSISTENT_BYTES", defaultPersistentBytes),
		PersistentWorkers: getEnvInt("CASE27_PERSISTENT_WORKERS", defaultPersistentWorkers),
	}
	cfg.RunForever = cfg.Duration <= 0

	if cfg.Concurrency <= 0 {
		return benchConfig{}, errors.New("CASE27_CONCURRENCY must be > 0")
	}
	if cfg.ClientTimeout <= 0 {
		return benchConfig{}, errors.New("CASE27_CLIENT_TIMEOUT_MS must be > 0")
	}
	if cfg.Role != "both" && cfg.Role != "server" {
		return benchConfig{}, fmt.Errorf("unsupported CASE27_ROLE=%s; expected both or server", cfg.Role)
	}
	if cfg.ClientMode != "sync" && cfg.ClientMode != "async" {
		return benchConfig{}, fmt.Errorf("unsupported CASE27_CLIENT_MODE=%s; expected sync or async", cfg.ClientMode)
	}
	if cfg.SendInterval < 0 {
		return benchConfig{}, errors.New("CASE27_SEND_INTERVAL_MS must be >= 0")
	}
	if cfg.MaxInflight <= 0 {
		return benchConfig{}, errors.New("CASE27_MAX_INFLIGHT must be > 0")
	}
	if cfg.PersistentBytes <= 0 {
		return benchConfig{}, errors.New("CASE27_PERSISTENT_BYTES must be > 0")
	}
	if cfg.PersistentWorkers < 2 {
		return benchConfig{}, errors.New("CASE27_PERSISTENT_WORKERS must be >= 2")
	}
	return cfg, nil
}

var (
	persistentSmallObjectSizes = []int{128, 256, 512, 1024, 2048, 4096, 8192, 16384}
	persistentLargeObjectSizes = []int{64 << 10, 256 << 10, 1 << 20}
	persistentStaticRootShards [][][]byte
	persistentTouchSink        atomic.Uint64
)

type persistentLayout struct {
	TotalBytes  int64
	StaticBytes int64
	StackBytes  int64
	SmallBytes  int64
	LargeBytes  int64
	Workers     int
}

func newPersistentLayout(totalBytes int64, workers int) (persistentLayout, error) {
	if totalBytes <= 0 {
		return persistentLayout{}, errors.New("persistent bytes must be > 0")
	}
	if workers < 2 {
		return persistentLayout{}, errors.New("persistent workers must be >= 2")
	}
	alignment := int64(2 * workers * persistentSmallObjectSizes[0])
	if totalBytes%alignment != 0 {
		return persistentLayout{}, fmt.Errorf(
			"persistent bytes must be a multiple of %d for %d workers",
			alignment, workers)
	}
	staticBytes := totalBytes / 2
	var largeBytes int64
	for workerID := 0; workerID < workers; workerID++ {
		largeBytes += persistentLargeBytes(
			splitPersistentBytes(staticBytes, workerID, workers))
		largeBytes += persistentLargeBytes(
			splitPersistentBytes(totalBytes-staticBytes, workerID, workers))
	}
	return persistentLayout{
		TotalBytes:  totalBytes,
		StaticBytes: staticBytes,
		StackBytes:  totalBytes - staticBytes,
		SmallBytes:  totalBytes - largeBytes,
		LargeBytes:  largeBytes,
		Workers:     workers,
	}, nil
}

type persistentStats struct {
	TotalBytes  int64
	StaticBytes int64
	StackBytes  int64
}

type persistentBaseline struct {
	stop            chan struct{}
	done            sync.WaitGroup
	stopOnce        sync.Once
	totalAllocated  atomic.Int64
	staticAllocated atomic.Int64
	stackAllocated  atomic.Int64
}

func splitPersistentBytes(total int64, index int, count int) int64 {
	base := total / int64(count)
	if int64(index) < total%int64(count) {
		return base + 1
	}
	return base
}

func persistentLargeBytes(targetBytes int64) int64 {
	alignment := int64(persistentLargeObjectSizes[0])
	return (targetBytes / 100 / alignment) * alignment
}

func persistentObjectCount(targetBytes int64, sizes []int) int {
	if targetBytes <= 0 {
		return 0
	}
	var count int64
	var allocated int64
	perClass := targetBytes / int64(len(sizes))
	for _, size := range sizes {
		objects := perClass / int64(size)
		count += objects
		allocated += objects * int64(size)
	}
	count += (targetBytes - allocated) / int64(sizes[0])
	return int(count)
}

func makeTouchedPersistentObject(size int, seed byte) []byte {
	object := make([]byte, size)
	for offset := 0; offset < len(object); offset += 4096 {
		object[offset] = seed
	}
	if len(object) > 0 {
		object[len(object)-1] = seed
	}
	return object
}

func appendPersistentTier(roots *[][]byte, targetBytes int64, sizes []int, seed byte) int64 {
	var allocated int64
	perClass := targetBytes / int64(len(sizes))
	for _, size := range sizes {
		fullObjects := perClass / int64(size)
		for objectIndex := int64(0); objectIndex < fullObjects; objectIndex++ {
			*roots = append(*roots, makeTouchedPersistentObject(size, seed))
			allocated += int64(size)
		}
	}
	smallestSize := int64(sizes[0])
	remainingObjects := (targetBytes - allocated) / smallestSize
	for objectIndex := int64(0); objectIndex < remainingObjects; objectIndex++ {
		*roots = append(*roots, makeTouchedPersistentObject(int(smallestSize), seed))
		allocated += smallestSize
	}
	return allocated
}

func allocatePersistentRoots(targetBytes int64, seed byte) ([][]byte, int64) {
	largeBytes := persistentLargeBytes(targetBytes)
	smallBytes := targetBytes - largeBytes
	capacity := persistentObjectCount(smallBytes, persistentSmallObjectSizes) +
		persistentObjectCount(largeBytes, persistentLargeObjectSizes)
	roots := make([][]byte, 0, capacity)
	allocated := appendPersistentTier(&roots, smallBytes, persistentSmallObjectSizes, seed)
	allocated += appendPersistentTier(&roots, largeBytes, persistentLargeObjectSizes, seed)
	return roots, allocated
}

func touchPersistentRoots(roots [][]byte) uint64 {
	var checksum uint64
	for _, object := range roots {
		if len(object) == 0 {
			continue
		}
		checksum += uint64(object[0]) + uint64(object[len(object)-1]) + uint64(len(object))
	}
	return checksum
}

func (baseline *persistentBaseline) allocationWorker(
	workerID int, staticTarget int64, stackTarget int64, ready chan<- struct{},
) {
	defer baseline.done.Done()
	seed := byte(workerID%251 + 1)
	staticRoots, staticBytes := allocatePersistentRoots(staticTarget, seed)
	stackRoots, stackBytes := allocatePersistentRoots(stackTarget, seed)
	persistentStaticRootShards[workerID] = staticRoots
	baseline.staticAllocated.Add(staticBytes)
	baseline.stackAllocated.Add(stackBytes)
	baseline.totalAllocated.Add(staticBytes + stackBytes)
	ready <- struct{}{}

	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-baseline.stop:
			persistentTouchSink.Add(touchPersistentRoots(staticRoots) + touchPersistentRoots(stackRoots))
			return
		case <-ticker.C:
			persistentTouchSink.Add(touchPersistentRoots(staticRoots) + touchPersistentRoots(stackRoots))
		}
	}
}

func startPersistentBaseline(totalBytes int64, workers int) (*persistentBaseline, error) {
	layout, err := newPersistentLayout(totalBytes, workers)
	if err != nil {
		return nil, err
	}
	baseline := &persistentBaseline{stop: make(chan struct{})}
	persistentStaticRootShards = make([][][]byte, workers)
	ready := make(chan struct{}, workers)
	baseline.done.Add(workers)
	for workerID := 0; workerID < workers; workerID++ {
		staticTarget := splitPersistentBytes(layout.StaticBytes, workerID, workers)
		stackTarget := splitPersistentBytes(layout.StackBytes, workerID, workers)
		go baseline.allocationWorker(workerID, staticTarget, stackTarget, ready)
	}
	for workerID := 0; workerID < workers; workerID++ {
		<-ready
	}
	return baseline, nil
}

func (baseline *persistentBaseline) Stats() persistentStats {
	return persistentStats{
		TotalBytes:  baseline.totalAllocated.Load(),
		StaticBytes: baseline.staticAllocated.Load(),
		StackBytes:  baseline.stackAllocated.Load(),
	}
}

func (baseline *persistentBaseline) Stop() {
	baseline.stopOnce.Do(func() {
		close(baseline.stop)
		baseline.done.Wait()
	})
}

func serverURL(cfg benchConfig) string {
	return fmt.Sprintf("http://%s:%d/process", cfg.ServerAddr, cfg.ServerPort)
}

func loadPayload(cfg benchConfig) ([]byte, string, error) {
	payloadPath := filepath.Join(cfg.PayloadDir, cfg.PayloadName+".json")
	payload, err := os.ReadFile(payloadPath)
	if err != nil {
		return nil, payloadPath, fmt.Errorf("read payload %s: %w", payloadPath, err)
	}
	return payload, payloadPath, nil
}

func decodeJSONTree(data []byte) (map[string]any, error) {
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.UseNumber()
	var root any
	if err := decoder.Decode(&root); err != nil {
		return nil, err
	}
	rootObject, ok := root.(map[string]any)
	if !ok {
		return nil, errors.New("root json must be object")
	}
	return rootObject, nil
}

func asObject(v any) (map[string]any, bool) {
	object, ok := v.(map[string]any)
	return object, ok
}

func asArray(v any) ([]any, bool) {
	array, ok := v.([]any)
	return array, ok
}

func toInt64(v any) (int64, bool) {
	switch value := v.(type) {
	case json.Number:
		if iv, err := value.Int64(); err == nil {
			return iv, true
		}
		if fv, err := value.Float64(); err == nil {
			return int64(fv), true
		}
	case float64:
		return int64(value), true
	case float32:
		return int64(value), true
	case int:
		return int64(value), true
	case int64:
		return value, true
	case int32:
		return int64(value), true
	case int16:
		return int64(value), true
	case int8:
		return int64(value), true
	case uint:
		return int64(value), true
	case uint64:
		return int64(value), true
	case uint32:
		return int64(value), true
	case uint16:
		return int64(value), true
	case uint8:
		return int64(value), true
	}
	return 0, false
}

func toFloat64(v any) (float64, bool) {
	switch value := v.(type) {
	case json.Number:
		if fv, err := value.Float64(); err == nil {
			return fv, true
		}
	case float64:
		return value, true
	case float32:
		return float64(value), true
	case int:
		return float64(value), true
	case int64:
		return float64(value), true
	case int32:
		return float64(value), true
	case int16:
		return float64(value), true
	case int8:
		return float64(value), true
	case uint:
		return float64(value), true
	case uint64:
		return float64(value), true
	case uint32:
		return float64(value), true
	case uint16:
		return float64(value), true
	case uint8:
		return float64(value), true
	}
	return 0, false
}

func processRequestJSON(body []byte, seq int64) ([]byte, error) {
	root, err := decodeJSONTree(body)
	if err != nil {
		return nil, err
	}

	if countersRaw, ok := root["counters"]; ok {
		if counters, ok := asObject(countersRaw); ok {
			if inValue, ok := toInt64(counters["in"]); ok {
				counters["in"] = inValue + 1
			}
			if okValue, ok := toInt64(counters["ok"]); ok {
				counters["ok"] = okValue + 1
			}
		}
	}

	root["processed_seq"] = seq
	root["server_ok"] = true

	if eventsRaw, ok := root["events"]; ok {
		if events, ok := asArray(eventsRaw); ok {
			for i, eventRaw := range events {
				event, ok := asObject(eventRaw)
				if !ok {
					continue
				}

				if seqValue, ok := toInt64(event["seq"]); ok {
					event["seq"] = seqValue + 1
				}
				if scoreValue, ok := toFloat64(event["score"]); ok {
					event["score"] = scoreValue * 2.0
				}

				if userRaw, ok := event["user"]; ok {
					if user, ok := asObject(userRaw); ok {
						if attrsRaw, ok := user["attrs"]; ok {
							if attrs, ok := asObject(attrsRaw); ok {
								if credits, ok := toFloat64(attrs["credits"]); ok {
									attrs["credits"] = credits * 2.0
								}
							}
						}
					}
				}

				if customRaw, ok := event["custom"]; ok {
					if custom, ok := asObject(customRaw); ok {
						if idsRaw, ok := custom["product_ids"]; ok {
							if productIDs, ok := asArray(idsRaw); ok {
								custom["product_ids"] = append(productIDs, int64(i)+9999)
							}
						}
					}
				}

				event["server_marker"] = fmt.Sprintf("m-%d", i)
			}
		}
	}

	return json.Marshal(root)
}

func parseServerOK(body []byte) error {
	root, err := decodeJSONTree(body)
	if err != nil {
		return fmt.Errorf("decode response json: %w", err)
	}
	serverOK, ok := root["server_ok"].(bool)
	if !ok {
		return errors.New("response missing server_ok")
	}
	if !serverOK {
		return errors.New("response server_ok=false")
	}
	return nil
}

func newHTTPClient(timeout time.Duration, maxConns int) *http.Client {
	if maxConns <= 0 {
		maxConns = 1
	}
	return &http.Client{
		Timeout: timeout,
		Transport: &http.Transport{
			MaxIdleConns:        maxConns,
			MaxIdleConnsPerHost: maxConns,
			MaxConnsPerHost:     maxConns,
			IdleConnTimeout:     30 * time.Second,
			DisableCompression:  true,
		},
	}
}

func doRequest(client *http.Client, url string, payload []byte, workerID int64, slotID int64, reqID int64) (int64, int64, error) {
	request, err := http.NewRequest(http.MethodPost, url, bytes.NewReader(payload))
	if err != nil {
		return 0, 0, fmt.Errorf("build request: %w", err)
	}
	request.Header.Set("content-type", "application/json")
	request.Header.Set("x-case27-client", "go-benchmark")
	request.Header.Set("x-case27-reqid", strconv.FormatInt(reqID, 10))
	request.Header.Set("x-case27-slotid", strconv.FormatInt(slotID, 10))
	request.Header.Set("x-case27-workerid", strconv.FormatInt(workerID, 10))

	response, err := client.Do(request)
	if err != nil {
		return 0, 0, fmt.Errorf("send request: %w", err)
	}
	defer response.Body.Close()

	body, err := io.ReadAll(response.Body)
	if err != nil {
		return int64(len(payload)), 0, fmt.Errorf("read response body: %w", err)
	}
	if response.StatusCode != http.StatusOK {
		return int64(len(payload)), int64(len(body)), fmt.Errorf("unexpected status: %s", response.Status)
	}
	if err := parseServerOK(body); err != nil {
		return int64(len(payload)), int64(len(body)), err
	}
	return int64(len(payload)), int64(len(body)), nil
}

func startHTTPServer(cfg benchConfig) (*http.Server, net.Listener, error) {
	var serverSeq atomic.Int64
	mux := http.NewServeMux()
	mux.HandleFunc("/process", func(w http.ResponseWriter, r *http.Request) {
		body, err := io.ReadAll(r.Body)
		if err != nil {
			http.Error(w, "read body failed", http.StatusBadRequest)
			return
		}
		response, err := processRequestJSON(body, serverSeq.Add(1)-1)
		if err != nil {
			fmt.Printf("[CASE27-GO][server-fail] reqId=%s slotId=%s err=%v\n",
				r.Header.Get("x-case27-reqid"), r.Header.Get("x-case27-slotid"), err)
			http.Error(w, "process request failed", http.StatusBadRequest)
			return
		}
		w.Header().Set("content-type", "application/json")
		_, _ = w.Write(response)
	})

	server := &http.Server{
		Addr:         fmt.Sprintf("%s:%d", cfg.ServerAddr, cfg.ServerPort),
		Handler:      mux,
		ReadTimeout:  cfg.ClientTimeout,
		WriteTimeout: cfg.ClientTimeout,
	}
	listener, err := net.Listen("tcp", server.Addr)
	if err != nil {
		return nil, nil, err
	}
	go func() {
		if err := server.Serve(listener); err != nil && !errors.Is(err, http.ErrServerClosed) {
			fmt.Printf("[ERROR] HTTP server stopped unexpectedly: %v\n", err)
		}
	}()
	return server, listener, nil
}

func warmup(url string, payload []byte, timeout time.Duration) {
	fmt.Printf("[INFO] Warmup: %d sequential requests...\n", defaultWarmupReqs)
	start := time.Now()
	client := newHTTPClient(timeout, 1)
	defer client.CloseIdleConnections()

	for i := 0; i < defaultWarmupReqs; i++ {
		if _, _, err := doRequest(client, url, payload, -1, -1, int64(i)); err != nil {
			fmt.Printf("[WARN] warmup request failed: %v\n", err)
		}
	}

	fmt.Printf("[INFO] Warmup done in %dms\n", time.Since(start).Milliseconds())
}

func sleepContext(ctx context.Context, duration time.Duration) bool {
	if duration <= 0 {
		select {
		case <-ctx.Done():
			return false
		default:
			return true
		}
	}
	timer := time.NewTimer(duration)
	defer timer.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-timer.C:
		return true
	}
}

func runSyncLoad(ctx context.Context, cfg benchConfig, url string, payload []byte) runStats {
	var stats atomicStats
	var nextReqID atomic.Int64
	nextReqID.Store(1)

	start := time.Now()
	done := make(chan struct{})
	go func() {
		if cfg.ProgressInterval <= 0 {
			<-done
			return
		}
		ticker := time.NewTicker(cfg.ProgressInterval)
		defer ticker.Stop()
		var lastTotal int64
		for {
			select {
			case <-done:
				return
			case <-ticker.C:
				snapshot := stats.snapshot()
				delta := snapshot.Total - lastTotal
				lastTotal = snapshot.Total
				fmt.Printf("[PROGRESS] t=%dms total=%d ok=%d fail=%d inflight=%d +last2s=%d\n",
					time.Since(start).Milliseconds(), snapshot.Total, snapshot.OK, snapshot.Fail, snapshot.Inflight, delta)
			}
		}
	}()

	var workers sync.WaitGroup
	for workerID := 0; workerID < cfg.Concurrency; workerID++ {
		workers.Add(1)
		go func(workerID int64) {
			defer workers.Done()
			client := newHTTPClient(cfg.ClientTimeout, 1)
			defer client.CloseIdleConnections()

			for {
				if ctx.Err() != nil {
					return
				}
				stats.inflight.Add(1)
				reqID := nextReqID.Add(1) - 1
				sentBytes, recvBytes, err := doRequest(client, url, payload, workerID, -1, reqID)
				stats.total.Add(1)
				stats.bytesSent.Add(sentBytes)
				stats.bytesRecv.Add(recvBytes)
				stats.inflight.Add(-1)
				if err != nil {
					stats.fail.Add(1)
					fmt.Printf("[CASE27-GO][client-fail] worker=%d slotId=-1 reqId=%d err=%v\n", workerID, reqID, err)
					continue
				}
				stats.ok.Add(1)
			}
		}(int64(workerID))
	}

	workers.Wait()
	close(done)
	return stats.snapshot()
}

func acquireAsyncSlot(slots []*asyncSlot) *asyncSlot {
	for _, slot := range slots {
		if slot.busy.CompareAndSwap(0, 1) {
			return slot
		}
	}
	return nil
}

func runAsyncLoad(ctx context.Context, cfg benchConfig, url string, payload []byte) runStats {
	var stats atomicStats
	var nextReqID atomic.Int64
	nextReqID.Store(1)

	slots := make([]*asyncSlot, 0, cfg.MaxInflight)
	for i := 0; i < cfg.MaxInflight; i++ {
		slots = append(slots, &asyncSlot{
			client: newHTTPClient(cfg.ClientTimeout, 1),
			id:     int64(i),
		})
	}
	defer func() {
		for _, slot := range slots {
			slot.client.CloseIdleConnections()
		}
	}()

	start := time.Now()
	done := make(chan struct{})
	go func() {
		if cfg.ProgressInterval <= 0 {
			<-done
			return
		}
		ticker := time.NewTicker(cfg.ProgressInterval)
		defer ticker.Stop()
		var lastTotal int64
		for {
			select {
			case <-done:
				return
			case <-ticker.C:
				snapshot := stats.snapshot()
				delta := snapshot.Total - lastTotal
				lastTotal = snapshot.Total
				fmt.Printf("[PROGRESS] t=%dms total=%d ok=%d fail=%d inflight=%d +last2s=%d\n",
					time.Since(start).Milliseconds(), snapshot.Total, snapshot.OK, snapshot.Fail, snapshot.Inflight, delta)
			}
		}
	}()

	var workers sync.WaitGroup
	for {
		if ctx.Err() != nil {
			break
		}
		slot := acquireAsyncSlot(slots)
		if slot != nil {
			reqID := nextReqID.Add(1) - 1
			stats.inflight.Add(1)
			workers.Add(1)
			go func(slot *asyncSlot, reqID int64) {
				defer workers.Done()
				defer slot.busy.Store(0)
				defer stats.inflight.Add(-1)
				sentBytes, recvBytes, err := doRequest(slot.client, url, payload, 0, slot.id, reqID)
				stats.total.Add(1)
				stats.bytesSent.Add(sentBytes)
				stats.bytesRecv.Add(recvBytes)
				if err != nil {
					stats.fail.Add(1)
					fmt.Printf("[CASE27-GO][client-fail] worker=0 slotId=%d reqId=%d err=%v\n", slot.id, reqID, err)
					return
				}
				stats.ok.Add(1)
			}(slot, reqID)
		}
		if !sleepContext(ctx, cfg.SendInterval) {
			break
		}
	}

	workers.Wait()
	close(done)
	return stats.snapshot()
}

func printSummary(cfg benchConfig, stats runStats, total time.Duration) {
	totalMs := total.Milliseconds()
	throughput := int64(0)
	if totalMs > 0 {
		throughput = stats.Total * 1000 / totalMs
	}
	fmt.Printf("[RESULT] payload=%s concurrency=%d duration=%dms client_mode=%s\n",
		cfg.PayloadName, cfg.Concurrency, totalMs, cfg.ClientMode)
	fmt.Printf("[RESULT] total=%d ok=%d fail=%d inflight=%d throughput=%dreq/s\n",
		stats.Total, stats.OK, stats.Fail, stats.Inflight, throughput)
	fmt.Printf("[RESULT] bytes_sent=%dMB bytes_recv=%dMB\n",
		stats.BytesSent/1024/1024, stats.BytesRecv/1024/1024)
	fmt.Println("[DONE]")
}

func runBothMode(parent context.Context, cfg benchConfig) error {
	payload, payloadPath, err := loadPayload(cfg)
	if err != nil {
		return err
	}
	fmt.Printf("[INFO] loaded payload bytes=%d (%dKB) from %s\n", len(payload), len(payload)/1024, payloadPath)

	server, listener, err := startHTTPServer(cfg)
	if err != nil {
		return fmt.Errorf("start http server: %w", err)
	}
	defer listener.Close()
	defer server.Shutdown(context.Background())

	fmt.Printf("[INFO] HTTP Server ready at %s\n", serverURL(cfg))
	if !sleepContext(parent, time.Second) {
		return parent.Err()
	}

	warmup(serverURL(cfg), payload, cfg.ClientTimeout)

	runCtx, cancel := context.WithCancel(parent)
	defer cancel()
	if !cfg.RunForever {
		go func() {
			timer := time.NewTimer(cfg.Duration)
			defer timer.Stop()
			select {
			case <-parent.Done():
				cancel()
			case <-timer.C:
				cancel()
			}
		}()
	}

	if cfg.ClientMode == "async" {
		if cfg.RunForever {
			fmt.Printf("[INFO] Started async sender in async mode; running without time limit...\n")
		} else {
			fmt.Printf("[INFO] Started async sender in async mode; running for %dms...\n", cfg.Duration.Milliseconds())
		}
	} else {
		if cfg.RunForever {
			fmt.Printf("[INFO] Started %d workers in sync mode; running without time limit...\n", cfg.Concurrency)
		} else {
			fmt.Printf("[INFO] Started %d workers in sync mode; running for %dms...\n", cfg.Concurrency, cfg.Duration.Milliseconds())
		}
	}

	start := time.Now()
	var stats runStats
	if cfg.ClientMode == "async" {
		stats = runAsyncLoad(runCtx, cfg, serverURL(cfg), payload)
	} else {
		stats = runSyncLoad(runCtx, cfg, serverURL(cfg), payload)
	}
	printSummary(cfg, stats, time.Since(start))

	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer shutdownCancel()
	_ = server.Shutdown(shutdownCtx)
	return nil
}

func runServerOnly(parent context.Context, cfg benchConfig) error {
	server, listener, err := startHTTPServer(cfg)
	if err != nil {
		return fmt.Errorf("start http server: %w", err)
	}
	defer listener.Close()
	fmt.Printf("[INFO] HTTP Server ready at %s\n", serverURL(cfg))
	fmt.Println("[INFO] Running in server-only mode; waiting for external clients...")
	<-parent.Done()
	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer shutdownCancel()
	return server.Shutdown(shutdownCtx)
}

func main() {
	cfg, err := loadConfigFromEnv()
	if err != nil {
		fmt.Printf("[ERROR] %v\n", err)
		os.Exit(1)
	}

	fmt.Println("[INFO] Case27 Go HTTP Complex-JSON Benchmark")
	fmt.Printf("[CONFIG] payload=%s concurrency=%d duration_ms=%d\n", cfg.PayloadName, cfg.Concurrency, cfg.Duration.Milliseconds())
	fmt.Printf("[CONFIG] payload_dir=%s server_port=%d\n", cfg.PayloadDir, cfg.ServerPort)
	fmt.Printf("[CONFIG] client_timeout_ms=%d\n", cfg.ClientTimeout.Milliseconds())
	fmt.Printf("[CONFIG] run_forever=%t\n", cfg.RunForever)
	fmt.Printf("[CONFIG] role=%s\n", cfg.Role)
	fmt.Printf("[CONFIG] client_mode=%s\n", cfg.ClientMode)
	fmt.Printf("[CONFIG] send_interval_ms=%d max_inflight=%d\n", cfg.SendInterval.Milliseconds(), cfg.MaxInflight)
	fmt.Printf("[CONFIG] persistent_bytes=%d persistent_workers=%d\n",
		cfg.PersistentBytes, cfg.PersistentWorkers)

	rootCtx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	fmt.Printf("[INFO] Creating persistent heap baseline: bytes=%d workers=%d...\n",
		cfg.PersistentBytes, cfg.PersistentWorkers)
	baseline, err := startPersistentBaseline(cfg.PersistentBytes, cfg.PersistentWorkers)
	if err != nil {
		fmt.Printf("[ERROR] start persistent baseline: %v\n", err)
		os.Exit(1)
	}
	defer baseline.Stop()
	persistentStats := baseline.Stats()
	fmt.Printf("[INFO] Persistent heap baseline ready: total=%d static=%d stack=%d\n",
		persistentStats.TotalBytes, persistentStats.StaticBytes, persistentStats.StackBytes)

	if cfg.Role == "server" {
		if err := runServerOnly(rootCtx, cfg); err != nil && !errors.Is(err, http.ErrServerClosed) && !errors.Is(err, context.Canceled) {
			fmt.Printf("[ERROR] %v\n", err)
			os.Exit(1)
		}
		return
	}

	if err := runBothMode(rootCtx, cfg); err != nil && !errors.Is(err, context.Canceled) {
		fmt.Printf("[ERROR] %v\n", err)
		os.Exit(1)
	}
}
