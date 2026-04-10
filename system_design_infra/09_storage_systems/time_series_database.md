# System Design: Time Series Database (Prometheus TSDB / InfluxDB-like)

> **Relevance to role:** Cloud infrastructure platform engineers operate monitoring stacks that ingest millions of metrics per second from bare-metal hosts, Kubernetes clusters, OpenStack services, and Java/Python applications. Designing a TSDB requires understanding write-optimized storage, time-based partitioning, cardinality management, and long-term retention — all critical for observability of large-scale infrastructure.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement | Detail |
|---|-------------|--------|
| FR-1 | Metric ingestion | Accept time-series data points: `{metric_name, labels, timestamp, value}` |
| FR-2 | Query by time range | Retrieve data points for a metric + label set within a time window |
| FR-3 | Aggregation queries | Sum, avg, rate, percentile across time and label dimensions |
| FR-4 | Label-based filtering | Query by arbitrary label combinations (e.g., `job="apiserver", instance="10.0.1.5"`) |
| FR-5 | Alerting rules | Evaluate PromQL expressions periodically and fire alerts |
| FR-6 | Recording rules | Pre-compute expensive queries and store results as new time series |
| FR-7 | Downsampling | Reduce resolution for historical data (1m → 5m → 1h) |
| FR-8 | Long-term storage | Retain metrics beyond local disk capacity (months/years) |
| FR-9 | Remote write/read | Forward metrics to/from external storage (Thanos, Cortex) |

### Non-Functional Requirements
| # | Requirement | Target |
|---|-------------|--------|
| NFR-1 | Write throughput | 10M samples/sec per cluster |
| NFR-2 | Query latency | p50 < 100 ms, p99 < 1 s (for 1h range, single series) |
| NFR-3 | Cardinality | Support 10M active time series |
| NFR-4 | Storage efficiency | < 2 bytes per sample (compressed) |
| NFR-5 | Availability | 99.9% for ingestion, 99.5% for queries |
| NFR-6 | Retention | 15 days local, 1 year long-term |
| NFR-7 | Durability | No sample loss after WAL ACK |

### Constraints & Assumptions
- Bare-metal servers with NVMe SSDs for TSDB storage
- Kubernetes environment: metrics from kube-state-metrics, node-exporter, cadvisor
- Prometheus-compatible ecosystem (PromQL, remote write protocol)
- Java services expose metrics via Micrometer/Prometheus JMX exporter
- Python services use prometheus_client library

### Out of Scope
- Log aggregation (Loki)
- Distributed tracing (Jaeger/Tempo)
- Event/span storage
- Dashboard rendering (Grafana)

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|--------|-------------|-------|
| Infrastructure nodes | 5,000 bare-metal + 20,000 containers | 25,000 sources |
| Metrics per node | ~500 metrics/node (CPU, memory, disk, network, app) | 500 |
| Total active series | 25,000 × 500 | 12.5M series |
| Scrape interval | 15 seconds | - |
| Samples per second | 12.5M / 15 | ~833K samples/sec |
| Peak (burst during deploys) | 3× average | ~2.5M samples/sec |
| Queries per second | 500 dashboard panels × 4 refreshes/min × 10 users | ~333 QPS |
| Alert rule evaluations | 2,000 rules × 1 eval/15s | ~133 evals/sec |

### Latency Requirements

| Operation | p50 | p99 |
|-----------|-----|-----|
| Sample ingestion (WAL write) | 1 ms | 5 ms |
| Instant query (single series, last 5 min) | 10 ms | 50 ms |
| Range query (single series, 1 hour) | 50 ms | 200 ms |
| Range query (1000 series, 1 hour) | 200 ms | 2 s |
| Aggregation (sum by job, 24h) | 500 ms | 5 s |

### Storage Estimates

| Component | Calculation | Value |
|-----------|-------------|-------|
| Raw sample size | 8 bytes (timestamp) + 8 bytes (float64) | 16 bytes |
| Compressed sample size (Gorilla) | ~1.37 bytes/sample (Prometheus empirical) | ~1.4 bytes |
| Samples per day | 833K/sec × 86,400 sec | ~72B samples/day |
| Storage per day (compressed) | 72B × 1.4 bytes | ~100 GB/day |
| 15-day local retention | 100 GB × 15 | ~1.5 TB |
| 1-year long-term (5m downsampled) | 100 GB × 365 / 20 (20× reduction from 15s→5m) | ~1.8 TB |
| WAL size (2 hours in-memory) | 833K/sec × 7200s × 16 bytes | ~96 GB (uncompressed WAL) |
| Index (inverted index for labels) | ~10% of data | ~150 GB |

### Bandwidth Estimates

| Flow | Calculation | Value |
|------|-------------|-------|
| Ingest (scrape traffic) | 833K samples/sec × 16 bytes + overhead | ~20 MB/s |
| Remote write (to Thanos) | Same as ingest, batched/compressed | ~5 MB/s (snappy compressed) |
| Query responses | 333 QPS × avg 50 KB response | ~16 MB/s |
| Compaction I/O (background) | Rewriting 2h blocks into 8h, 48h blocks | ~50 MB/s (burst) |

---

## 3. High Level Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        METRIC SOURCES                                │
│  ┌─────────┐  ┌──────────┐  ┌───────────┐  ┌────────────────────┐  │
│  │ node-   │  │ kube-    │  │ JMX       │  │ Python prometheus  │  │
│  │ exporter│  │ state-   │  │ exporter  │  │ _client            │  │
│  │ (:9100) │  │ metrics  │  │ (Java)    │  │ (:8000/metrics)    │  │
│  └────┬────┘  └────┬─────┘  └─────┬─────┘  └──────┬─────────────┘  │
└───────┼─────────────┼──────────────┼───────────────┼────────────────┘
        │             │              │               │
        └─────────────┴──────────────┴───────────────┘
                              │ HTTP scrape (pull) / remote write (push)
                              ▼
┌─────────────────────────────────────────────────────────────────────┐
│                     PROMETHEUS / TSDB CLUSTER                        │
│                                                                      │
│  ┌──────────────────────────────────────────────────────┐           │
│  │              Prometheus Server                        │           │
│  │  ┌──────────┐  ┌──────────┐  ┌───────────────────┐  │           │
│  │  │ Scrape   │  │ Rule     │  │ PromQL Query      │  │           │
│  │  │ Manager  │  │ Engine   │  │ Engine            │  │           │
│  │  └────┬─────┘  └────┬─────┘  └────────┬──────────┘  │           │
│  │       │              │                 │              │           │
│  │  ┌────▼──────────────▼─────────────────▼──────────┐  │           │
│  │  │                TSDB Engine                      │  │           │
│  │  │  ┌──────┐  ┌───────────┐  ┌────────────────┐  │  │           │
│  │  │  │ WAL  │  │ Head Block│  │ Persistent     │  │  │           │
│  │  │  │(disk)│──│ (memory,  │  │ Blocks (disk)  │  │  │           │
│  │  │  │      │  │  2h window│  │ [2h][2h][8h]   │  │  │           │
│  │  │  └──────┘  └───────────┘  └────────────────┘  │  │           │
│  │  │                                                │  │           │
│  │  │  ┌──────────────────┐  ┌────────────────────┐  │  │           │
│  │  │  │ Compactor        │  │ Inverted Index     │  │  │           │
│  │  │  │ (2h→8h→48h→      │  │ (label→series ID  │  │  │           │
│  │  │  │  stream merge)   │  │  → postings list)  │  │  │           │
│  │  │  └──────────────────┘  └────────────────────┘  │  │           │
│  │  └────────────────────────────────────────────────┘  │           │
│  └──────────────┬──────────────────────┬────────────────┘           │
│                 │ remote write          │ remote read                │
│                 ▼                       ▼                            │
│  ┌──────────────────────────────────────────────────────┐           │
│  │              Thanos / Cortex (Long-Term)              │           │
│  │  ┌──────────┐  ┌──────────┐  ┌───────────────────┐  │           │
│  │  │ Thanos   │  │ Thanos   │  │ Thanos            │  │           │
│  │  │ Sidecar  │  │ Store    │  │ Compactor         │  │           │
│  │  │ (uploads │  │ Gateway  │  │ (downsampling +   │  │           │
│  │  │  blocks) │  │ (serves  │  │  block merging)   │  │           │
│  │  │          │  │  queries)│  │                    │  │           │
│  │  └────┬─────┘  └────┬─────┘  └──────┬────────────┘  │           │
│  │       │              │               │               │           │
│  │       ▼              ▼               ▼               │           │
│  │  ┌──────────────────────────────────────────┐       │           │
│  │  │  Object Storage (S3-compatible)           │       │           │
│  │  │  [raw blocks] [5m downsampled] [1h down.] │       │           │
│  │  └──────────────────────────────────────────┘       │           │
│  └──────────────────────────────────────────────────────┘           │
│                                                                      │
│  ┌──────────────────────────────────────────────────────┐           │
│  │              Thanos Query (Federated)                 │           │
│  │  Queries across all Prometheus instances + long-term  │           │
│  │  Deduplicates overlapping data from HA pairs          │           │
│  └──────────────────────────────────────────────────────┘           │
└─────────────────────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **Scrape Manager** | Discovers targets (Kubernetes SD, file SD, DNS SD); pulls metrics at configured intervals (15s default); manages scrape pools |
| **TSDB Engine** | Core storage: WAL for durability, Head block for recent in-memory data, persistent blocks for older data |
| **WAL (Write-Ahead Log)** | Append-only on-disk log of all incoming samples; replayed on crash recovery |
| **Head Block** | In-memory block holding the last ~2 hours of data; samples are appended to per-series chunk buffers |
| **Persistent Blocks** | Immutable 2-hour blocks on disk; each block contains chunks, index, metadata, tombstones |
| **Compactor** | Merges adjacent blocks (2h → 8h → 48h); reduces index overhead; applies tombstones |
| **Inverted Index** | Maps label pairs → posting lists (series IDs); supports efficient label-based queries |
| **Rule Engine** | Evaluates alerting/recording rules at configured intervals; writes recording rule results back to TSDB |
| **PromQL Query Engine** | Parses and executes PromQL queries; accesses Head block + persistent blocks + remote read |
| **Thanos Sidecar** | Runs alongside Prometheus; uploads completed blocks to object storage; serves StoreAPI for queries |
| **Thanos Store Gateway** | Reads blocks from object storage; serves historical data via StoreAPI |
| **Thanos Compactor** | Merges/deduplicates blocks in object storage; generates downsampled blocks (5m, 1h) |
| **Thanos Query** | Federated PromQL query across multiple Prometheus instances and Store Gateways |

### Data Flows

**Ingestion (Scrape):**
1. Scrape Manager HTTP GETs `/metrics` from each target
2. Parses Prometheus exposition format → `{metric_name, labels, value}`
3. For each sample: append to WAL (fsync per batch, ~10ms)
4. Append to Head block's in-memory chunk for that series
5. Head block cuts a new chunk when current chunk reaches ~120 samples

**Query (Range Query):**
1. Client sends PromQL query with time range `[start, end]`
2. Query Engine parses PromQL → execution plan
3. Identifies which blocks overlap `[start, end]`: Head block + persistent blocks
4. For each block: lookup series matching label matchers via inverted index
5. Read chunks for matching series; decompress; apply PromQL functions (rate, sum, etc.)
6. Return result as instant vector or matrix

**Block Lifecycle:**
1. Head block accumulates 2 hours of data
2. Head is "cut" → new persistent block written to disk (chunks + index + meta.json)
3. WAL segments older than the cut block are deleted
4. Compactor merges adjacent 2h blocks → 8h blocks → 48h blocks
5. Thanos Sidecar uploads completed blocks to object storage
6. After upload, local blocks older than retention are deleted

---

## 4. Data Model

### Core Entities & Schema

```
Time Series:
  Unique identity: metric_name + sorted label set (fingerprint hash)
  Example: http_requests_total{method="GET", handler="/api/v1/query", status="200"}
  
  Internal representation:
  ├── series_id: uint64 (hash of label set, or sequential in TSDB)
  ├── labels: []Label{name: string, value: string}  (sorted by name)
  └── chunks: []Chunk (time-ordered, non-overlapping)

Chunk:
  ├── min_time: int64 (milliseconds since epoch)
  ├── max_time: int64
  ├── encoding: enum {XOR, Histogram}
  ├── num_samples: uint16
  └── data: []byte (compressed samples)

Sample:
  ├── timestamp: int64 (milliseconds since epoch)
  └── value: float64

Block (on-disk directory):
  block_id/
  ├── meta.json        # {minTime, maxTime, stats, compaction level}
  ├── chunks/
  │   └── 000001       # concatenated chunk data (up to 512MB per segment)
  ├── index            # inverted index: label → postings list → chunk refs
  └── tombstones       # deleted series/time ranges (applied during compaction)

WAL (on-disk):
  wal/
  ├── 000001           # WAL segment (up to 128MB)
  ├── 000002
  └── checkpoint.000001 # Periodic snapshot of in-memory series state
```

### Database/Storage Selection

| Component | Technology | Rationale |
|-----------|-----------|-----------|
| In-memory samples | Custom Go mmap'd chunks (Prometheus) | Zero-copy reads; efficient gorilla compression |
| On-disk blocks | Custom format (chunks + index files) | Optimized for time-range scans; immutable for cache-friendliness |
| WAL | Append-only segment files on NVMe | Durability with minimal write amplification |
| Long-term storage | S3-compatible object storage | Infinite scale; cheap; immutable blocks fit perfectly |
| Index | Custom inverted index per block | Supports multi-label intersection queries efficiently |

### Indexing Strategy

**Inverted Index Structure (per block):**
```
Symbol Table:    [0: "method", 1: "GET", 2: "handler", 3: "/api/v1/query", ...]
Postings Lists:  method="GET"     → [series_1, series_5, series_12, ...]
                 handler="/api/*" → [series_1, series_3, series_12, ...]
Series Data:     series_1 → {labels: [...], chunks: [ref1, ref2, ...]}
```

- **Label name index:** `__name__` → all series with that metric name
- **Label value index:** `label_name=label_value` → posting list (sorted series IDs)
- **Multi-label query:** intersect posting lists (sorted merge)
- **Regular expression:** enumerate matching label values, union their posting lists

---

## 5. API Design

### Storage APIs

**Prometheus Remote Write (push):**
```protobuf
// Prometheus remote write protocol (protobuf, snappy compressed)
message WriteRequest {
  repeated TimeSeries timeseries = 1;
  repeated MetricMetadata metadata = 2;
}

message TimeSeries {
  repeated Label labels = 1;
  repeated Sample samples = 2;
}

message Label {
  string name  = 1;
  string value = 2;
}

message Sample {
  double value    = 1;
  int64  timestamp = 2;  // milliseconds since epoch
}
```

```
POST /api/v1/write
Content-Type: application/x-protobuf
Content-Encoding: snappy

Body: snappy(proto(WriteRequest))
Response: 200 OK (or 400/500 on error)
```

**PromQL Query API:**
```
# Instant query
GET /api/v1/query?query=up{job="apiserver"}&time=2024-01-15T00:00:00Z

# Range query
GET /api/v1/query_range?query=rate(http_requests_total[5m])&start=...&end=...&step=15s

# Label values
GET /api/v1/label/job/values

# Series metadata
GET /api/v1/series?match[]=up{job="apiserver"}

# Alerts
GET /api/v1/alerts
```

**Thanos StoreAPI (gRPC):**
```protobuf
service Store {
  rpc Series(SeriesRequest) returns (stream SeriesResponse);
  rpc LabelNames(LabelNamesRequest) returns (LabelNamesResponse);
  rpc LabelValues(LabelValuesRequest) returns (LabelValuesResponse);
}

message SeriesRequest {
  int64 min_time = 1;
  int64 max_time = 2;
  repeated LabelMatcher matchers = 3;
  int64 max_resolution_window = 4;  // 0 = raw, 300000 = 5m, 3600000 = 1h
  Aggrs aggregates = 5;             // MIN, MAX, SUM, COUNT, COUNTER
}
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Chunk Encoding (Gorilla Compression)

**Why it's hard:** A naive TSDB storing 16 bytes per sample (8-byte timestamp + 8-byte float64) would consume 100 GB/day for 833K samples/sec. At scale, storage costs explode. Time series data has strong temporal locality: timestamps are nearly regular, and values change slowly. Exploiting this requires a specialized encoding that achieves < 2 bytes/sample while supporting fast decoding.

**Approaches:**

| Approach | Bytes/Sample | Encode Speed | Decode Speed | Complexity |
|----------|-------------|-------------|-------------|-----------|
| Raw (8+8) | 16.0 | N/A | N/A | None |
| General compression (gzip) | ~4-6 | Medium | Medium | Low |
| Delta encoding + varint | ~4-8 | Fast | Fast | Low |
| Gorilla (delta-of-delta + XOR) | 1.37 | Fast | Fast | Medium |
| Gorilla + dictionary (for int series) | < 1.0 | Fast | Fast | Medium |
| Facebook Gorilla (original paper) | 1.37 | Fast | Fast | Medium |

**Selected approach:** Gorilla encoding (Facebook 2015 paper), as used by Prometheus TSDB.

**Justification:**
- Achieves 1.37 bytes/sample empirically on real-world metrics
- Timestamp encoding exploits regular scrape intervals (delta-of-delta is often 0)
- Value encoding exploits slow-changing floats (XOR with previous value has many leading/trailing zeros)
- Fast encode/decode: no dictionary, no Huffman trees; bit-level operations only

**Implementation Detail:**

```python
import struct
import math

class GorillaEncoder:
    """
    Gorilla encoding for time-series samples.
    Based on Facebook's "Gorilla: A Fast, Scalable, In-Memory Time Series Database" (2015).
    
    Timestamp encoding: delta-of-delta
    - If delta-of-delta == 0: write '0' (1 bit)
    - If fits in [-63, 64]: write '10' + 7 bits (9 bits)
    - If fits in [-255, 256]: write '110' + 9 bits (12 bits)
    - If fits in [-2047, 2048]: write '1110' + 12 bits (16 bits)
    - Else: write '1111' + 32 bits (36 bits)
    
    Value encoding: XOR with previous value
    - If XOR == 0: write '0' (1 bit) — value unchanged
    - If leading zeros >= previous leading zeros and
      trailing zeros >= previous trailing zeros:
      write '10' + meaningful bits (compact)
    - Else: write '11' + 5 bits (leading zeros) + 6 bits (meaningful length)
      + meaningful bits
    """

    def __init__(self):
        self.bits = BitWriter()
        self.num_samples = 0
        
        # Timestamp state
        self.t_prev = 0
        self.t_delta_prev = 0
        
        # Value state
        self.v_prev_bits = 0  # float64 as uint64
        self.v_leading_zeros_prev = 64
        self.v_trailing_zeros_prev = 0

    def add_sample(self, timestamp_ms: int, value: float):
        if self.num_samples == 0:
            # First sample: write raw timestamp (64 bits) and value (64 bits)
            self.bits.write_bits(timestamp_ms, 64)
            self.bits.write_bits(float_to_uint64(value), 64)
            self.t_prev = timestamp_ms
            self.v_prev_bits = float_to_uint64(value)
        elif self.num_samples == 1:
            # Second sample: write delta (14 bits for typical 15s interval)
            delta = timestamp_ms - self.t_prev
            self.bits.write_bits(delta, 14)
            self._encode_value(value)
            self.t_delta_prev = delta
            self.t_prev = timestamp_ms
        else:
            # Subsequent samples: delta-of-delta encoding for timestamp
            delta = timestamp_ms - self.t_prev
            delta_of_delta = delta - self.t_delta_prev
            self._encode_timestamp_dod(delta_of_delta)
            self._encode_value(value)
            self.t_delta_prev = delta
            self.t_prev = timestamp_ms

        self.num_samples += 1

    def _encode_timestamp_dod(self, dod: int):
        """Encode delta-of-delta with variable-length prefix coding."""
        if dod == 0:
            self.bits.write_bit(0)                    # 1 bit
        elif -63 <= dod <= 64:
            self.bits.write_bits(0b10, 2)             # prefix
            self.bits.write_signed(dod, 7)            # 9 bits total
        elif -255 <= dod <= 256:
            self.bits.write_bits(0b110, 3)
            self.bits.write_signed(dod, 9)            # 12 bits total
        elif -2047 <= dod <= 2048:
            self.bits.write_bits(0b1110, 4)
            self.bits.write_signed(dod, 12)           # 16 bits total
        else:
            self.bits.write_bits(0b1111, 4)
            self.bits.write_signed(dod, 32)           # 36 bits total

    def _encode_value(self, value: float):
        """XOR encoding for float64 values."""
        v_bits = float_to_uint64(value)
        xor = v_bits ^ self.v_prev_bits

        if xor == 0:
            # Value unchanged — 1 bit
            self.bits.write_bit(0)
        else:
            self.bits.write_bit(1)

            leading = count_leading_zeros(xor)
            trailing = count_trailing_zeros(xor)

            # Can we reuse the previous window?
            if (leading >= self.v_leading_zeros_prev and
                trailing >= self.v_trailing_zeros_prev):
                # Reuse window — write '0' + meaningful bits
                self.bits.write_bit(0)
                meaningful_bits = 64 - self.v_leading_zeros_prev - self.v_trailing_zeros_prev
                meaningful = (xor >> self.v_trailing_zeros_prev) & ((1 << meaningful_bits) - 1)
                self.bits.write_bits(meaningful, meaningful_bits)
            else:
                # New window — write '1' + 5 bits leading + 6 bits length + meaningful
                self.bits.write_bit(1)
                self.bits.write_bits(leading, 5)       # 0-31 (capped)
                meaningful_bits = 64 - leading - trailing
                self.bits.write_bits(meaningful_bits, 6)  # 1-64
                meaningful = (xor >> trailing) & ((1 << meaningful_bits) - 1)
                self.bits.write_bits(meaningful, meaningful_bits)

                self.v_leading_zeros_prev = leading
                self.v_trailing_zeros_prev = trailing

        self.v_prev_bits = v_bits

    def finish(self) -> bytes:
        """Return compressed chunk bytes."""
        return self.bits.to_bytes()


class GorillaDecoder:
    """Decode a Gorilla-encoded chunk back to samples."""

    def __init__(self, data: bytes, num_samples: int):
        self.bits = BitReader(data)
        self.num_samples = num_samples

    def decode_all(self) -> list[tuple[int, float]]:
        samples = []

        # First sample
        t = self.bits.read_bits(64)
        v = uint64_to_float(self.bits.read_bits(64))
        samples.append((t, v))

        if self.num_samples < 2:
            return samples

        # Second sample
        t_delta = self.bits.read_bits(14)
        t = t + t_delta
        v = self._decode_value(v)
        samples.append((t, v))

        # Remaining samples
        v_prev_bits = float_to_uint64(v)
        leading_prev = 64
        trailing_prev = 0

        for _ in range(2, self.num_samples):
            # Decode timestamp delta-of-delta
            dod = self._decode_timestamp_dod()
            t_delta = t_delta + dod
            t = t + t_delta

            # Decode value
            v, v_prev_bits, leading_prev, trailing_prev = \
                self._decode_value_xor(v_prev_bits, leading_prev, trailing_prev)
            samples.append((t, v))

        return samples

    def _decode_timestamp_dod(self) -> int:
        if self.bits.read_bit() == 0:
            return 0
        if self.bits.read_bit() == 0:
            return self.bits.read_signed(7)
        if self.bits.read_bit() == 0:
            return self.bits.read_signed(9)
        if self.bits.read_bit() == 0:
            return self.bits.read_signed(12)
        return self.bits.read_signed(32)

    def _decode_value_xor(self, prev_bits, leading_prev, trailing_prev):
        if self.bits.read_bit() == 0:
            return uint64_to_float(prev_bits), prev_bits, leading_prev, trailing_prev

        if self.bits.read_bit() == 0:
            # Reuse previous window
            meaningful_bits = 64 - leading_prev - trailing_prev
            meaningful = self.bits.read_bits(meaningful_bits)
            xor = meaningful << trailing_prev
        else:
            # New window
            leading = self.bits.read_bits(5)
            meaningful_bits = self.bits.read_bits(6)
            if meaningful_bits == 0:
                meaningful_bits = 64
            trailing = 64 - leading - meaningful_bits
            meaningful = self.bits.read_bits(meaningful_bits)
            xor = meaningful << trailing
            leading_prev = leading
            trailing_prev = trailing

        v_bits = prev_bits ^ xor
        return uint64_to_float(v_bits), v_bits, leading_prev, trailing_prev


def float_to_uint64(f: float) -> int:
    return struct.unpack('>Q', struct.pack('>d', f))[0]

def uint64_to_float(u: int) -> float:
    return struct.unpack('>d', struct.pack('>Q', u))[0]

def count_leading_zeros(x: int) -> int:
    if x == 0: return 64
    n = 0
    if x & 0xFFFFFFFF00000000 == 0: n += 32; x <<= 32
    if x & 0xFFFF000000000000 == 0: n += 16; x <<= 16
    if x & 0xFF00000000000000 == 0: n += 8;  x <<= 8
    if x & 0xF000000000000000 == 0: n += 4;  x <<= 4
    if x & 0xC000000000000000 == 0: n += 2;  x <<= 2
    if x & 0x8000000000000000 == 0: n += 1
    return n

def count_trailing_zeros(x: int) -> int:
    if x == 0: return 64
    n = 0
    if x & 0x00000000FFFFFFFF == 0: n += 32; x >>= 32
    if x & 0x000000000000FFFF == 0: n += 16; x >>= 16
    if x & 0x00000000000000FF == 0: n += 8;  x >>= 8
    if x & 0x000000000000000F == 0: n += 4;  x >>= 4
    if x & 0x0000000000000003 == 0: n += 2;  x >>= 2
    if x & 0x0000000000000001 == 0: n += 1
    return n
```

**Compression Effectiveness (Real-World Metrics):**

| Metric Type | Avg bits/timestamp | Avg bits/value | Total bytes/sample |
|------------|-------------------|---------------|--------------------|
| Counter (monotonically increasing) | 1.0 (dod=0) | 2-4 (XOR has few changing bits) | ~0.5 |
| Gauge (slowly changing) | 1.0 | 8-16 (moderate XOR) | ~1.5 |
| Gauge (rapidly changing) | 1.0 | 20-40 (large XOR) | ~3.0 |
| Histogram bucket | 1.0 | 1-2 (often unchanged) | ~0.3 |
| **Weighted average** | **~1.0** | **~10** | **~1.37** |

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Bit-level corruption in chunk | Decoded values garbage from corruption point onward | Per-chunk CRC32 checksum; reject and re-scrape if recent |
| Encoder overflow (timestamp jump) | delta-of-delta doesn't fit in 32 bits | Fall back to raw 64-bit encoding; start new chunk |
| Decoder drift (accumulating deltas) | Timestamp drift if a single dod is wrong | Chunk boundary resets all state; 120-sample chunks limit drift |
| NaN/Inf values | XOR of special float values may not compress well | Handle NaN/Inf as special cases; they're rare in practice |

**Interviewer Q&As:**

**Q1: Why does Gorilla achieve 1.37 bytes/sample on average?**
A: Most infrastructure metrics have: (1) perfectly regular timestamps (dod=0, encoded in 1 bit), (2) slowly changing values (XOR with previous has many leading/trailing zeros). A typical counter sample needs ~1 bit for timestamp + ~4 bits for value = ~0.6 bytes. Gauges with more variation need more bits but are less common.

**Q2: What are the limitations of Gorilla encoding?**
A: (1) Sequential access only — cannot seek to a specific timestamp within a chunk (must decode from the beginning). (2) Poor compression for random/noisy data (approaching 16 bytes/sample). (3) No random writes — append-only within a chunk. (4) Chunk boundary creates a fixed overhead (first sample is always 128 bits).

**Q3: How does Prometheus decide when to cut a new chunk?**
A: When the chunk reaches ~120 samples or when the time span exceeds 2 hours. The Head block's "chunk pool" manages chunk lifecycle. A new chunk is started in the Head when the old one is full, and the old one becomes read-only and part of the block that will be compacted.

**Q4: How does this compare to InfluxDB's TSM encoding?**
A: TSM (Time Structured Merge Tree) uses a similar approach but with column-oriented storage: timestamps and values are stored separately. Timestamps use delta encoding + simple-8b (batch integer compression). Values use Gorilla-style XOR for floats, run-length for integers, and dictionary encoding for strings. TSM achieves similar compression ratios.

**Q5: Can you encode histogram samples with Gorilla?**
A: Prometheus 2.40+ introduces native histograms with a dedicated chunk encoding. Instead of exploding a histogram into multiple series (one per bucket), the entire histogram is stored as a single sample using a sparse representation. This dramatically reduces cardinality and improves compression.

**Q6: How does chunk encoding interact with queries?**
A: A range query reads entire chunks and decodes all samples in the range. If the query range is [t1, t2] and a chunk spans [t0, t3] where t0 < t1 and t3 > t2, the decoder must decode from t0 (sequential access), skip samples before t1, and stop after t2. This is why small chunks (120 samples = ~30 minutes at 15s interval) are preferred.

---

### Deep Dive 2: Write Path (WAL → Head Block → Persistent Block)

**Why it's hard:** TSDB must accept millions of samples per second with durability guarantees, while simultaneously serving queries against recently ingested data. The write path must balance write throughput (batch WAL writes), memory efficiency (compressed chunks), and query performance (fast series lookup). Crash recovery must restore in-memory state from WAL without losing samples.

**Approaches:**

| Approach | Write Throughput | Query on Recent Data | Crash Recovery | Memory Usage |
|----------|-----------------|---------------------|----------------|-------------|
| Write directly to immutable blocks | Low (block creation is expensive) | Poor (block must be complete) | Simple | Low |
| In-memory buffer → periodic flush | Very high | Good (query memory) | Must replay WAL | High |
| LSM-tree (InfluxDB TSM) | High | Good | Replay WAL | Medium |
| Prometheus TSDB (WAL + Head block) | High | Good | Replay WAL + checkpoint | Medium-High |

**Selected approach:** Prometheus TSDB architecture (WAL + Head block).

**Implementation Detail:**

```python
class TSDB:
    """Simplified Prometheus TSDB write path."""

    def __init__(self, data_dir: str, retention: timedelta = timedelta(days=15)):
        self.data_dir = data_dir
        self.retention = retention

        # WAL: append-only log for durability
        self.wal = WAL(os.path.join(data_dir, 'wal'))

        # Head block: in-memory, mutable, holds recent data (~2h)
        self.head = HeadBlock()

        # Persistent blocks: immutable, on-disk, time-ordered
        self.blocks = self._load_blocks()

        # Series index: label set → series_id
        self.series_index = InvertedIndex()

        # Recover from WAL on startup
        self._recover_from_wal()

    def append(self, labels: dict, timestamp_ms: int, value: float):
        """
        Ingest a single sample. Hot path — must be fast.
        
        Steps:
        1. Resolve or create series (label set → series_id)
        2. Append to WAL (durability)
        3. Append to Head block (query-ability)
        """
        # Step 1: Resolve series
        series_id = self.series_index.get_or_create(labels)

        # Step 2: WAL write (batched; fsync every 10ms or 1000 samples)
        self.wal.log_sample(series_id, timestamp_ms, value)

        # Step 3: Append to Head block's in-memory chunk
        series = self.head.get_or_create_series(series_id, labels)
        series.append(timestamp_ms, value)

    def cut_block(self):
        """
        Called every ~2 hours. Converts Head block data into a 
        persistent on-disk block.
        
        Steps:
        1. Freeze current Head block (no more writes)
        2. Create new Head block for incoming data
        3. Write frozen Head to disk as a persistent block
        4. Truncate WAL (delete segments before the block's min_time)
        """
        frozen_head = self.head
        self.head = HeadBlock()  # New head for incoming data

        # Write block to disk
        block_dir = os.path.join(self.data_dir, ulid.new().str)
        block_writer = BlockWriter(block_dir)

        for series_id, series in frozen_head.all_series():
            for chunk in series.chunks:
                block_writer.add_chunk(series_id, series.labels, chunk)

        # Write index (inverted index for this block)
        block_writer.write_index()

        # Write meta.json
        block_writer.write_meta(
            min_time=frozen_head.min_time,
            max_time=frozen_head.max_time,
            num_samples=frozen_head.num_samples,
            num_series=frozen_head.num_series
        )

        block_writer.finalize()
        self.blocks.append(Block(block_dir))

        # Truncate WAL: remove segments before frozen_head.min_time
        self.wal.truncate_before(frozen_head.min_time)

    def compact(self):
        """
        Merge adjacent blocks to reduce index overhead and
        apply tombstones (deletions).
        
        Compaction levels:
        - Level 0: 2h blocks (from Head)
        - Level 1: 8h blocks (4× 2h merged)
        - Level 2: 48h blocks (6× 8h merged)
        """
        # Find compaction candidates
        candidates = self._find_compactable_blocks()

        for group in candidates:
            merged_dir = os.path.join(self.data_dir, ulid.new().str)
            block_writer = BlockWriter(merged_dir)

            # Streaming merge of all series across blocks
            iterators = [block.series_iterator() for block in group]
            merged = heap_merge(iterators, key=lambda s: s.series_id)

            for series_id, chunks_from_all_blocks in merged:
                # Merge overlapping chunks (dedup by timestamp)
                merged_chunks = merge_overlapping_chunks(chunks_from_all_blocks)
                for chunk in merged_chunks:
                    block_writer.add_chunk(series_id, chunk.labels, chunk)

            block_writer.write_index()
            block_writer.write_meta(
                min_time=min(b.min_time for b in group),
                max_time=max(b.max_time for b in group),
                compaction_level=max(b.compaction_level for b in group) + 1
            )
            block_writer.finalize()

            # Atomic swap: add new block, remove old blocks
            self.blocks.append(Block(merged_dir))
            for old_block in group:
                self.blocks.remove(old_block)
                old_block.mark_for_deletion()

    def _recover_from_wal(self):
        """Replay WAL on startup to rebuild Head block."""
        checkpoint = self.wal.load_latest_checkpoint()
        if checkpoint:
            self.head.restore_from_checkpoint(checkpoint)
            self.series_index.restore_from_checkpoint(checkpoint)

        # Replay WAL segments after checkpoint
        for record in self.wal.replay_from(checkpoint):
            if record.type == 'series':
                self.series_index.register(record.series_id, record.labels)
                self.head.get_or_create_series(record.series_id, record.labels)
            elif record.type == 'sample':
                series = self.head.get_series(record.series_id)
                if series:
                    series.append(record.timestamp, record.value)


class HeadBlock:
    """In-memory block holding recent samples (~2 hours)."""

    def __init__(self):
        self.series = {}  # series_id → MemSeries
        self.min_time = float('inf')
        self.max_time = float('-inf')
        self.num_samples = 0
        self.num_series = 0

    def get_or_create_series(self, series_id: int, labels: dict):
        if series_id not in self.series:
            self.series[series_id] = MemSeries(series_id, labels)
            self.num_series += 1
        return self.series[series_id]


class MemSeries:
    """In-memory series with Gorilla-encoded chunks."""

    def __init__(self, series_id: int, labels: dict):
        self.series_id = series_id
        self.labels = labels
        self.chunks = []
        self.active_chunk = GorillaEncoder()  # Current writable chunk
        self.active_chunk_samples = 0

    def append(self, timestamp_ms: int, value: float):
        self.active_chunk.add_sample(timestamp_ms, value)
        self.active_chunk_samples += 1

        # Cut chunk at ~120 samples
        if self.active_chunk_samples >= 120:
            self.chunks.append(self.active_chunk.finish())
            self.active_chunk = GorillaEncoder()
            self.active_chunk_samples = 0
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Crash before WAL fsync | Samples in buffer lost (up to 10ms / 1000 samples) | Acceptable: Prometheus re-scrapes in 15s anyway |
| WAL corruption | Cannot replay some samples | WAL records have CRC32; skip corrupt records; re-scrape fills gaps |
| Head block OOM | Prometheus crashes | Limit series count (`storage.tsdb.max-block-chunk-segment-size`); cardinality enforcement |
| Disk full during block write | Block partially written | Write to temp dir, atomic rename; incomplete blocks detected by meta.json absence |
| Compaction fails mid-way | Old blocks still valid; retried | Compaction is idempotent; old blocks are only deleted after new block is complete |
| Clock skew in samples | Out-of-order samples rejected | Prometheus TSDB (2.39+) supports out-of-order ingestion within a configurable window |

**Interviewer Q&As:**

**Q1: Why does Prometheus use a 2-hour block window?**
A: 2 hours balances: (1) Memory: longer windows need more RAM for in-memory chunks. (2) WAL replay time: longer windows mean longer recovery on crash. (3) Compaction: 2h blocks are small enough to compact quickly. (4) Query: most queries are over recent data, which is in the fast in-memory Head.

**Q2: How does out-of-order ingestion work?**
A: Before Prometheus 2.39, samples with timestamps older than the latest sample in a series were rejected. Since 2.39, an out-of-order (OOO) ingestion window (configurable, e.g., 30 minutes) accepts late samples. They're stored in a separate "OOO head" and merged during compaction. This supports use cases like remote-write from different time zones or late-arriving metrics.

**Q3: What's the WAL checkpoint and why is it needed?**
A: The WAL grows continuously. A checkpoint is a snapshot of the current series state (labels and chunk references) at a point in time. On recovery, instead of replaying the entire WAL from the beginning, Prometheus loads the checkpoint and replays only the WAL segments after it. This makes recovery fast even after long uptime.

**Q4: How does Prometheus handle metric relabeling during scrape?**
A: Relabeling rules (`metric_relabel_configs`) are applied after scraping but before ingestion. Labels can be added, modified, or dropped. Series can be dropped entirely. This happens in the Scrape Manager before the samples reach the TSDB. Critical for controlling cardinality.

**Q5: What happens when you delete a metric series?**
A: A tombstone is created in the Head block (or a `tombstones` file in persistent blocks). Tombstones are ranges `[series_id, min_time, max_time]` that mark data as deleted. The data is physically removed during compaction. Until compaction, the space is not reclaimed.

**Q6: How does the TSDB handle series churn (new series constantly being created)?**
A: Series churn (e.g., from pod restarts in Kubernetes) creates many short-lived series. Each series consumes memory in the Head block and index entries. Mitigation: (1) Track "active series" count, alert on spikes. (2) Use `sample_limit` in scrape config. (3) Relabeling to drop high-churn labels. (4) Compaction garbage-collects series with no recent samples.

---

### Deep Dive 3: Cardinality and the Label Explosion Problem

**Why it's hard:** Cardinality = number of unique time series. Each unique combination of metric name + label values creates a new series. A metric like `http_request_duration_seconds{method, handler, status, instance, pod}` with 4 methods × 200 handlers × 10 statuses × 50 instances × 100 pods = 400 million series. Each series consumes memory in the Head block (~300 bytes for metadata + chunks). Unchecked cardinality can OOM the TSDB.

**Approaches:**

| Approach | Effectiveness | User Impact | Complexity |
|----------|-------------|-------------|-----------|
| No limits (crash on OOM) | None | Catastrophic | None |
| Global series limit | High (prevents OOM) | Drops all new series when limit hit | Low |
| Per-scrape `sample_limit` | Medium | Drops entire scrape if limit exceeded | Low |
| Per-metric cardinality limit | High | Fine-grained control | Medium |
| Label value allowlist | Very high | Requires maintenance | High |
| Pre-aggregation (recording rules) | Very high (reduces stored cardinality) | Must design upfront | Medium |

**Selected approach:** Multi-layered: per-scrape `sample_limit` + global series limit + recording rules for pre-aggregation + monitoring cardinality dashboards.

**Implementation Detail:**

```python
class CardinalityEnforcer:
    """
    Multi-layered cardinality control.
    Prevents label explosion from crashing the TSDB.
    """

    def __init__(self, config):
        self.global_series_limit = config.get('global_series_limit', 10_000_000)
        self.per_metric_limit = config.get('per_metric_limit', 100_000)
        self.per_scrape_sample_limit = config.get('sample_limit', 50_000)

        self.series_count = 0
        self.per_metric_counts = defaultdict(int)  # metric_name → count
        self.violations = Counter()  # metric_name → violation count

    def check_sample(self, metric_name: str, labels: dict) -> bool:
        """
        Returns True if the sample should be accepted.
        Called on every new series creation (not on every sample).
        """
        # Layer 1: Global series limit
        if self.series_count >= self.global_series_limit:
            self.violations['global_limit_exceeded'] += 1
            return False

        # Layer 2: Per-metric cardinality limit
        if self.per_metric_counts[metric_name] >= self.per_metric_limit:
            self.violations[f'metric_limit:{metric_name}'] += 1
            return False

        # Layer 3: Label value validation (reject high-cardinality labels)
        for label_name, label_value in labels.items():
            if self._is_high_cardinality_value(label_name, label_value):
                self.violations[f'high_cardinality_label:{label_name}'] += 1
                return False

        self.series_count += 1
        self.per_metric_counts[metric_name] += 1
        return True

    def _is_high_cardinality_value(self, name: str, value: str) -> bool:
        """Heuristic: reject labels that look like UUIDs, timestamps, etc."""
        # UUID pattern
        if len(value) == 36 and value.count('-') == 4:
            return True
        # Numeric-only (likely a timestamp or request ID)
        if value.isdigit() and len(value) > 10:
            return True
        return False

    def check_scrape_batch(self, samples: list) -> list:
        """
        Per-scrape sample_limit: reject entire scrape if too many samples.
        This prevents a misconfigured target from creating unlimited series.
        """
        if len(samples) > self.per_scrape_sample_limit:
            self.violations['scrape_limit_exceeded'] += 1
            # Return empty — entire scrape is dropped
            # Log which target exceeded the limit
            return []
        return samples

    def get_top_offenders(self, n: int = 10) -> list:
        """Return top N metrics by cardinality for dashboarding."""
        return sorted(self.per_metric_counts.items(),
                      key=lambda x: x[1], reverse=True)[:n]


class RecordingRulePreAggregator:
    """
    Recording rules: pre-compute expensive aggregations.
    Reduces query-time cardinality by storing aggregated results.
    
    Example: Instead of querying across 50 instances × 100 pods:
      sum by (method, handler, status) (
        rate(http_request_duration_seconds_count[5m])
      )
    
    Store the result as a new metric with fewer label dimensions:
      job:http_request_duration_seconds_count:rate5m{method, handler, status}
    
    This reduces cardinality from 400M to 8K (4 × 200 × 10).
    """

    def __init__(self, tsdb, rules_config):
        self.tsdb = tsdb
        self.rules = self._parse_rules(rules_config)

    def evaluate_rules(self):
        """Called every evaluation_interval (default 15s)."""
        for rule in self.rules:
            # Execute PromQL aggregation
            result = self.tsdb.query(rule.expr, time.now())

            # Write aggregated samples back to TSDB
            for series in result:
                labels = {
                    '__name__': rule.record_name,
                    **series.labels  # Only the "by" labels remain
                }
                self.tsdb.append(labels, time.now_ms(), series.value)

    def _parse_rules(self, config):
        """
        Example rules.yml:
        groups:
          - name: pre_aggregation
            interval: 15s
            rules:
              - record: job:http_request_duration_seconds_count:rate5m
                expr: >
                  sum by (method, handler, status) (
                    rate(http_request_duration_seconds_count[5m])
                  )
              - record: instance:node_cpu_seconds_total:rate5m
                expr: >
                  avg by (instance, mode) (
                    rate(node_cpu_seconds_total[5m])
                  )
        """
        return [Rule(**r) for r in config['groups'][0]['rules']]
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Undetected cardinality spike | Head block OOM → Prometheus crash | Cardinality dashboard; alerting on active series growth rate |
| sample_limit drops entire scrape | Gap in metrics for that target | Alert on `scrape_samples_post_metric_relabeling` → `scrape_sample_limit` |
| Recording rule creates new high-cardinality metric | Self-inflicted cardinality | Validate recording rules before deployment; `by` clause must reduce dimensions |
| Label relabeling bug (e.g., dropping __name__) | All samples for that target invalid | Prometheus config reload validation; `promtool check config` |

**Interviewer Q&As:**

**Q1: What causes label explosion in Kubernetes environments?**
A: Common causes: (1) Pod name includes random suffix (e.g., `pod="my-app-7b8c4f5d6-x2k3j"`) — every pod restart creates new series. (2) Trace/request ID in labels. (3) User-facing URLs as label values (every unique URL = new series). (4) Large number of histogram buckets (each bucket is a separate series). Fix: relabel to drop or aggregate high-cardinality labels.

**Q2: How do you detect cardinality problems before they cause outages?**
A: (1) Monitor `prometheus_tsdb_head_series` (total active series). (2) Alert on growth rate > N/hour. (3) Use `tsdb` admin API: `GET /api/v1/status/tsdb` returns top 10 metrics by cardinality. (4) Grafana dashboard showing cardinality per job/namespace. (5) `promtool tsdb analyze` on block data.

**Q3: How do recording rules reduce query load?**
A: A dashboard panel querying `sum by (status) (rate(http_requests_total[5m]))` across 10M series must read and aggregate 10M samples per evaluation. A recording rule pre-computes this to a few series (one per status code). The dashboard queries the recording rule result, reading only ~5 series. This reduces query time from seconds to milliseconds.

**Q4: What is the memory cost per active time series?**
A: In Prometheus TSDB, approximately: 300 bytes for series metadata (labels, hash, chunk references) + ~1 KB for the active in-memory chunk. So roughly 1-2 KB per active series. At 10M series: 10-20 GB just for series data, plus WAL buffers and query working memory.

**Q5: How does Thanos handle cardinality across multiple Prometheus instances?**
A: Thanos Querier deduplicates series from HA pairs (same labels except `replica` label). Thanos Compactor deduplicates in long-term storage. For query-time cardinality, Thanos Store Gateway streams series lazily. However, "query across all Prometheus instances" can still be expensive if combined cardinality is high — use recording rules per-instance to pre-aggregate.

**Q6: What's the difference between "active series" and "total series"?**
A: Active series have received a sample in the last scrape interval (roughly). Total series includes all series in the Head block (including those that haven't received samples recently but haven't been garbage-collected). A series is garbage-collected from the Head when its latest sample is older than the Head's min_time (i.e., it has been compacted to a persistent block and is no longer receiving samples).

---

### Deep Dive 4: Long-Term Storage with Thanos

**Why it's hard:** Prometheus stores data locally with limited retention (15 days typical). For capacity planning, historical analysis, and compliance, you need months or years of metrics. Scaling a single Prometheus instance vertically is impractical. You need horizontal scaling (multiple Prometheus instances) with a global query view and cost-effective long-term storage.

**Selected approach:** Thanos (sidecar + store gateway + compactor + querier).

**Implementation Detail:**

```yaml
# Thanos architecture with Prometheus

# 1. Prometheus + Thanos Sidecar (per Prometheus instance)
# Sidecar uploads completed blocks to object storage
# and serves StoreAPI for live data

apiVersion: apps/v1
kind: StatefulSet
metadata:
  name: prometheus
spec:
  replicas: 2  # HA pair
  template:
    spec:
      containers:
        - name: prometheus
          image: prom/prometheus:v2.48.0
          args:
            - --config.file=/etc/prometheus/prometheus.yml
            - --storage.tsdb.path=/prometheus
            - --storage.tsdb.retention.time=24h   # Short local retention
            - --storage.tsdb.min-block-duration=2h
            - --storage.tsdb.max-block-duration=2h  # Disable local compaction
            - --web.enable-lifecycle
          volumeMounts:
            - name: data
              mountPath: /prometheus

        - name: thanos-sidecar
          image: thanosio/thanos:v0.32.0
          args:
            - sidecar
            - --tsdb.path=/prometheus
            - --prometheus.url=http://localhost:9090
            - --objstore.config-file=/etc/thanos/bucket.yml
            - --grpc-address=0.0.0.0:10901
          volumeMounts:
            - name: data
              mountPath: /prometheus
            - name: bucket-config
              mountPath: /etc/thanos

---
# 2. Thanos Store Gateway (reads from object storage)
apiVersion: apps/v1
kind: Deployment
metadata:
  name: thanos-store
spec:
  replicas: 3  # Shard by time range or block count
  template:
    spec:
      containers:
        - name: store
          image: thanosio/thanos:v0.32.0
          args:
            - store
            - --objstore.config-file=/etc/thanos/bucket.yml
            - --index-cache-size=2GB
            - --chunk-pool-size=4GB
            - --grpc-address=0.0.0.0:10901

---
# 3. Thanos Compactor (runs as singleton)
apiVersion: apps/v1
kind: Deployment
metadata:
  name: thanos-compactor
spec:
  replicas: 1  # Must be singleton
  template:
    spec:
      containers:
        - name: compactor
          image: thanosio/thanos:v0.32.0
          args:
            - compact
            - --objstore.config-file=/etc/thanos/bucket.yml
            - --retention.resolution-raw=90d
            - --retention.resolution-5m=365d
            - --retention.resolution-1h=0d  # Keep forever
            - --downsample.concurrency=4
            - --wait  # Run continuously

---
# 4. Thanos Query (global PromQL endpoint)
apiVersion: apps/v1
kind: Deployment
metadata:
  name: thanos-query
spec:
  replicas: 3
  template:
    spec:
      containers:
        - name: query
          image: thanosio/thanos:v0.32.0
          args:
            - query
            - --store=dnssrv+_grpc._tcp.thanos-sidecar.monitoring.svc
            - --store=dnssrv+_grpc._tcp.thanos-store.monitoring.svc
            - --query.replica-label=replica  # Deduplicate HA pairs
            - --query.auto-downsampling       # Use downsampled data for long ranges
            - --grpc-address=0.0.0.0:10901
            - --http-address=0.0.0.0:9090
```

```python
class ThanosDownsampler:
    """
    Downsampling: reduce resolution for historical data.
    
    Raw (15s) → 5-minute aggregates → 1-hour aggregates
    
    For each 5-minute window, store 5 aggregated values:
    - min, max, sum, count, counter (for rate calculations)
    
    This allows accurate query results at lower resolution:
    - rate() uses counter aggregate
    - avg() uses sum/count
    - min()/max() use min/max aggregates
    """

    def downsample_block(self, block: Block, resolution: int) -> Block:
        """
        resolution: 300000 (5 min) or 3600000 (1 hour) in milliseconds
        """
        new_block = BlockWriter(new_dir)

        for series in block.series_iterator():
            downsampled_chunks = self._downsample_series(
                series.chunks, resolution)
            new_block.add_series(series.labels, downsampled_chunks)

        new_block.write_meta(
            min_time=block.min_time,
            max_time=block.max_time,
            resolution=resolution
        )
        return new_block.finalize()

    def _downsample_series(self, chunks, resolution):
        """Aggregate raw samples into fixed windows."""
        windows = defaultdict(lambda: AggrValue())

        for chunk in chunks:
            for timestamp, value in decode_chunk(chunk):
                window_start = (timestamp // resolution) * resolution
                windows[window_start].add(value)

        # Encode aggregated values
        result_chunks = []
        encoder = AggrChunkEncoder()
        for window_start in sorted(windows):
            aggr = windows[window_start]
            encoder.add_aggr_sample(
                timestamp=window_start,
                min_val=aggr.min,
                max_val=aggr.max,
                sum_val=aggr.sum,
                count=aggr.count,
                counter=aggr.counter  # Last value for counter metrics
            )
        result_chunks.append(encoder.finish())
        return result_chunks


class AggrValue:
    """Accumulates min, max, sum, count for a downsampling window."""
    def __init__(self):
        self.min = float('inf')
        self.max = float('-inf')
        self.sum = 0.0
        self.count = 0
        self.counter = 0.0  # Last value (for counter resets)

    def add(self, value: float):
        self.min = min(self.min, value)
        self.max = max(self.max, value)
        self.sum += value
        self.count += 1
        self.counter = value  # Track last value
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|-----------|
| Sidecar upload fails | Block not in object storage; data at risk if Prometheus disk fails | Retry with exponential backoff; alert on upload lag |
| Object storage unavailable | No long-term queries; no uploads | Store Gateway caches recent blocks; Prometheus retains local data |
| Compactor crashes | No downsampling; blocks accumulate | Compactor is idempotent; restart picks up where it left off |
| Query timeout on large range | User gets no data | `auto-downsampling`: use 5m or 1h resolution for large ranges |
| Duplicate data from HA pair | Double-counted in aggregations | `replica` label deduplication in Thanos Query |

**Interviewer Q&As:**

**Q1: Why Thanos over Cortex (now Mimir)?**
A: Thanos: simpler architecture (sidecar model, uploads blocks to object storage). Cortex/Mimir: more complex (requires a write path with ingesters, distributors, compactors). Thanos is better for "bolt-on" long-term storage to existing Prometheus. Mimir is better for multi-tenant managed Prometheus-as-a-service.

**Q2: How does downsampling preserve accuracy for rate() queries?**
A: The downsampled block stores a `counter` aggregate (last value in each window). rate() across downsampled data uses these counter values to compute the per-second rate, just as it would with raw samples. Counter resets are detected by comparing consecutive counter values. This is why 5 aggregates are stored, not just the average.

**Q3: How does Thanos Query deduplicate HA pairs?**
A: Two Prometheus instances scraping the same targets produce identical series (same labels except a `replica` label). Thanos Query is configured with `--query.replica-label=replica`. When merging results from both sidecars, it picks one instance's data per time window and drops the other's. If one instance is down, the other's data fills the gap seamlessly.

**Q4: What's the storage cost of long-term retention?**
A: Raw data at 1.4 bytes/sample, 72B samples/day = 100 GB/day = 36.5 TB/year. With 5-minute downsampling (20× reduction): 1.8 TB/year. With 1-hour downsampling (240× reduction): 152 GB/year. Object storage at $0.023/GB/month (S3 Standard): $503/month for raw year, $41/month for 5m downsampled.

**Q5: How do you handle queries that span both local and long-term data?**
A: Thanos Query fans out to both Sidecar (live data) and Store Gateway (long-term data). The query engine merges results, preferring higher-resolution data when available. For the most recent 24 hours, data comes from Sidecar (raw resolution). For older data, Store Gateway serves from object storage (potentially downsampled).

**Q6: How does Thanos Store Gateway handle the cold-start problem (loading index from object storage)?**
A: On startup, Store Gateway downloads block metadata (meta.json) and index headers from object storage. The full index is loaded lazily on first query. The `index-cache-size` parameter controls how much index data is cached in memory. For large datasets, a block-level time-based sharding strategy distributes blocks across multiple Store Gateway replicas.

---

## 7. Scaling Strategy

**Scaling dimensions:**

| Dimension | Approach |
|-----------|---------|
| Ingestion throughput | Shard by target (multiple Prometheus instances, each scraping a subset) |
| Active series (cardinality) | Shard by namespace/team; each Prometheus handles a partition |
| Query throughput | Thanos Query scales horizontally; each replica handles queries independently |
| Long-term storage | Object storage (infinite); Store Gateway scales horizontally |
| Alerting evaluation | Distribute rules across Prometheus instances; Thanos Ruler for cross-instance rules |

**Interviewer Q&As:**

**Q1: How do you shard Prometheus for high-cardinality environments?**
A: Functional sharding: one Prometheus per Kubernetes cluster or per team/namespace. Each instance scrapes only its assigned targets via service discovery. Thanos Query provides a unified view across all instances. This naturally limits cardinality per instance.

**Q2: What's the theoretical maximum write throughput per Prometheus instance?**
A: On NVMe SSD, Prometheus can sustain ~5-10M samples/sec per instance (limited by CPU for chunk encoding and WAL I/O). At 833K samples/sec, a single instance handles our entire cluster. For 10× scale, we need 2-3 Prometheus instances.

**Q3: How do you handle cross-instance queries (e.g., "sum across all clusters")?**
A: Thanos Query. It queries all Sidecar and Store Gateway endpoints in parallel, merges results, and applies the PromQL aggregation. For very expensive cross-instance queries, pre-aggregate with recording rules on each instance and query the pre-aggregated metric via Thanos.

**Q4: How do you scale alert rule evaluation?**
A: Distribute rules across Prometheus instances. Each instance evaluates rules for its local data. For rules that need cross-instance data (rare), use Thanos Ruler, which evaluates rules against Thanos Query. Thanos Ruler is stateless and can be scaled horizontally for throughput.

**Q5: How do you handle a Prometheus instance falling behind on scrapes?**
A: Monitor `prometheus_target_scrape_pool_exceeded_target_limit` and `prometheus_target_interval_length_seconds` (actual vs configured interval). If scrape duration exceeds the interval, the instance is overloaded. Solutions: (1) reduce targets (reshard), (2) increase scrape interval, (3) reduce sample count per scrape (relabeling), (4) upgrade hardware (more CPU, faster disk).

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure | Detection | Impact | Recovery | RTO | RPO |
|---|---------|-----------|--------|----------|-----|-----|
| 1 | Prometheus instance crash | Kubernetes liveness probe | Metrics gap until restart; WAL replay | Pod restart; WAL replay restores in-memory state | < 2 min | < 10s (WAL batch interval) |
| 2 | NVMe disk failure on Prometheus | I/O errors in logs | TSDB corrupted; data loss | Restore from Thanos (long-term); restart on new disk | Minutes | Up to 2h (last uploaded block) |
| 3 | Both HA Prometheus instances down | Alertmanager detects missing heartbeat | No ingestion; no alerting | Fix root cause; HA pair restarts independently | Minutes | Gap in metrics |
| 4 | Object storage unavailable | Store Gateway errors | No long-term queries; uploads queue | Wait for S3 recovery; Sidecar retries uploads | S3 recovery | 0 (data buffered locally) |
| 5 | Thanos Query overloaded | Query timeouts, 5xx errors | Dashboard failures | Horizontal scale Query replicas; add query caching | < 5 min | 0 |
| 6 | Compactor stuck | Growing block count in object storage | Storage cost increases; query slows (many blocks) | Restart compactor; investigate stuck block | Minutes | 0 |
| 7 | Cardinality explosion | OOM alert on Prometheus | Prometheus crash → metrics gap | Relabel to drop offending labels; restart | Minutes | Gap duration |

### Data Durability and Replication

- **Local durability:** WAL on NVMe with periodic fsync. Crash recovery replays WAL to restore Head block.
- **HA replication:** Two Prometheus instances scrape the same targets independently. No data loss if one fails.
- **Long-term durability:** Thanos Sidecar uploads completed blocks to S3 (11-nines durability). Blocks are immutable and checksummed.
- **Object storage replication:** S3 handles cross-AZ replication internally. Thanos blocks can also be replicated to a second bucket in another region.

---

## 9. Security

| Layer | Mechanism |
|-------|-----------|
| **Scrape authentication** | TLS client certificates for scrape targets; bearer token auth for API endpoints |
| **Remote write authentication** | Bearer token or mTLS between Prometheus and Thanos/Cortex |
| **Query authentication** | OAuth2/OIDC in front of Thanos Query (via reverse proxy); Grafana handles user auth |
| **Multi-tenancy** | Separate Prometheus instances per tenant; Thanos Query with tenant header enforcement |
| **Encryption at rest** | NVMe disk encryption (dm-crypt/LUKS); S3 SSE for object storage |
| **Encryption in transit** | TLS for all gRPC (Thanos StoreAPI); TLS for HTTP (scrape, query API) |
| **Network isolation** | Prometheus in monitoring namespace; network policies restrict egress to scrape targets only |
| **RBAC** | Grafana RBAC controls which teams see which dashboards/data sources |

---

## 10. Incremental Rollout Strategy

| Phase | Scope | Duration | Validation |
|-------|-------|----------|-----------|
| 1 | Single Prometheus + node-exporter on 10 hosts | 1 week | Ingest rate, query latency, WAL recovery test |
| 2 | Full cluster scraping (all node-exporters + kube-state-metrics) | 2 weeks | Cardinality monitoring; storage growth rate |
| 3 | HA pair + Thanos Sidecar + object storage | 2 weeks | Failover test; block upload verification |
| 4 | Thanos Store Gateway + Compactor | 2 weeks | Long-range query correctness; downsampling validation |
| 5 | Application metrics (JMX exporter, Python client) | 3 weeks | Cardinality control; recording rules |
| 6 | Alerting rules migration from legacy system | 2 weeks | Alert parity check; false positive/negative analysis |
| 7 | Full production; decommission legacy monitoring | 2 weeks | SLA monitoring; on-call runbook updates |

**Rollout Q&As:**

**Q1: How do you validate that no metrics are lost during migration?**
A: Run both old and new monitoring in parallel for 2 weeks. Compare key metrics (node CPU, request rate, error rate) between systems. Automate comparison using a script that queries both endpoints and computes relative error. Accept < 1% deviation (due to scrape timing differences).

**Q2: How do you test Thanos Compactor correctness?**
A: After compaction, query the same time range from raw blocks (pre-compaction) and compacted blocks. Verify sample-level equality. Also verify that downsampled blocks produce rate() results within 1% of raw-data rate() results over the same range.

**Q3: How do you handle the cardinality change when adding application metrics?**
A: Before enabling application scraping, profile each application's metrics endpoint: `curl /metrics | promtool tsdb create-blocks-from-rules`. Estimate cardinality increase. Add `metric_relabel_configs` to drop high-cardinality metrics before enabling scrape. Monitor `prometheus_tsdb_head_series` after each onboarding.

**Q4: What's your rollback plan if Prometheus causes too much scrape traffic?**
A: Prometheus scrape is pull-based, so disabling it is instant: delete the scrape config or stop the pod. Unlike push-based systems, there's no risk of thundering herd on rollback. The scrape targets continue running unaffected.

**Q5: How do you migrate alerting rules from a legacy system?**
A: (1) Translate legacy rules to PromQL. (2) Run both systems in parallel for 2 weeks. (3) Compare alert firings (Alertmanager logs vs legacy system alerts). (4) Tune PromQL thresholds to match legacy behavior. (5) Switch alerting to Prometheus + Alertmanager. (6) Keep legacy in read-only mode for 30 days as fallback.

---

## 11. Trade-offs & Decision Log

| # | Decision | Alternatives | Rationale |
|---|----------|-------------|-----------|
| 1 | Prometheus + Thanos over InfluxDB | InfluxDB, VictoriaMetrics, Mimir | Prometheus is the Kubernetes-native standard; massive ecosystem (exporters, Grafana); Thanos adds long-term storage |
| 2 | Pull-based scraping over push-based | Push (Graphite, InfluxDB line protocol) | Pull is simpler for service discovery; Prometheus controls scrape interval; easy to detect down targets |
| 3 | Gorilla encoding over general compression | gzip, lz4, zstd | Gorilla is designed for time-series; 1.37 bytes/sample vs ~4-6 for general compression; fast encode/decode |
| 4 | 2-hour block size over 1h or 4h | 1h (more blocks, more compaction), 4h (more memory, longer WAL replay) | 2h balances memory usage, WAL replay time, and compaction overhead |
| 5 | Thanos over Cortex/Mimir for long-term | Cortex (Mimir), VictoriaMetrics, M3 | Thanos is simpler (sidecar model); no additional write path; less operational overhead for our scale |
| 6 | Recording rules over query-time aggregation | On-the-fly aggregation, materialized views | Recording rules are predictable (constant cost), cached in TSDB; query-time aggregation is expensive for high cardinality |
| 7 | Per-scrape sample_limit over per-metric limit | Per-metric limit (more granular but harder to configure) | sample_limit is simple, catches most explosion scenarios; per-metric limit as second layer for known problem metrics |
| 8 | NVMe for TSDB over HDD | HDD (cheaper), SAN (shared) | TSDB workload is write-heavy with random reads during queries; NVMe provides the IOPS needed; local NVMe avoids SAN latency |

---

## 12. Agentic AI Integration

| Use Case | Agentic AI Application | Implementation |
|----------|----------------------|----------------|
| **Anomaly detection on metrics** | Agent learns normal metric behavior; detects deviations without static thresholds | Train per-metric models (Prophet, DeepAR) on 30-day history; agent evaluates live metrics against model; fires adaptive alerts |
| **Automatic cardinality management** | Agent detects cardinality spikes and applies relabeling rules | Monitor `prometheus_tsdb_head_series`; when growth rate exceeds threshold, agent identifies offending metric/label and proposes (or auto-applies) `metric_relabel_configs` |
| **Intelligent downsampling** | Agent selects optimal downsampling resolution per metric based on query patterns | Analyze Thanos Query audit log; metrics never queried below 5m resolution get aggressively downsampled; frequently queried metrics retain raw resolution |
| **Root cause analysis** | Agent correlates metric anomalies across services to identify root cause | When alert fires, agent queries correlated metrics (upstream/downstream services), builds a dependency-aware timeline, and surfaces probable root cause |
| **Recording rule generation** | Agent analyzes slow queries and proposes recording rules | Parse Prometheus query log; identify queries with high cardinality fan-out and long execution time; generate recording rules that pre-aggregate the bottleneck |
| **Capacity forecasting** | Agent predicts storage, cardinality, and query load growth | Time-series forecasting on TSDB meta-metrics; agent generates capacity reports and provisions additional Prometheus instances before limits are reached |

**Example: Adaptive Anomaly Detection Agent**

```python
class MetricAnomalyAgent:
    """
    Replaces static alert thresholds with learned baselines.
    Uses Prophet for metrics with seasonality (daily/weekly),
    simple statistical models for others.
    """

    def __init__(self, prometheus_client, alertmanager_client, model_store):
        self.prom = prometheus_client
        self.am = alertmanager_client
        self.models = model_store

    async def train_models(self):
        """Train/retrain models weekly for key infrastructure metrics."""
        metrics_to_model = [
            'node_cpu_seconds_total',
            'node_memory_MemAvailable_bytes',
            'http_request_duration_seconds',
            'container_cpu_usage_seconds_total',
        ]

        for metric in metrics_to_model:
            # Query 30 days of 5-minute data
            data = await self.prom.query_range(
                f'avg({metric})',
                start='-30d', end='now', step='5m')

            model = ProphetModel()
            model.fit(data)
            self.models.save(metric, model)

    async def evaluate(self):
        """Run every 1 minute."""
        for metric, model in self.models.all():
            current = await self.prom.query(f'avg({metric})')
            prediction = model.predict(datetime.utcnow())

            # Compute z-score relative to predicted value
            z_score = (current - prediction.yhat) / prediction.yhat_std

            if abs(z_score) > 3.0:  # 3-sigma deviation
                severity = 'critical' if abs(z_score) > 5.0 else 'warning'
                await self.am.fire_alert(
                    alertname=f'{metric}_anomaly',
                    severity=severity,
                    description=(
                        f'{metric} is {z_score:.1f} sigma from expected. '
                        f'Current: {current:.2f}, '
                        f'Expected: {prediction.yhat:.2f} '
                        f'± {prediction.yhat_std:.2f}'),
                    labels={'metric': metric, 'agent': 'anomaly_detector'})
```

---

## 13. Complete Interviewer Q&A Bank

**Architecture:**

**Q1: Explain the Prometheus TSDB write path in detail.**
A: (1) Scrape Manager HTTP GETs /metrics from target. (2) Parses exposition format. (3) For each sample: resolve series by label set (hash lookup in Head). (4) Append sample to WAL (batch fsync every 10ms). (5) Append to MemSeries active chunk (Gorilla-encoded). (6) Chunk cut at ~120 samples. (7) Head block cut every 2h → persistent block on disk. (8) WAL segments before the cut block are deleted. (9) Compactor merges blocks (2h→8h→48h).

**Q2: How does PromQL evaluate a range query like `rate(http_requests_total[5m])`?**
A: (1) Parse PromQL → AST. (2) Identify all series matching `http_requests_total` (inverted index lookup). (3) For each evaluation timestamp (based on step): fetch samples in `[t-5m, t]` window from matching series. (4) Apply `rate()`: compute per-second increase over the window, handling counter resets. (5) Return instant vector at each step timestamp.

**Q3: What's the difference between instant vectors and range vectors in PromQL?**
A: Instant vector: one sample per series at a single timestamp (e.g., `http_requests_total` evaluated at time T). Range vector: a series of samples per series over a time window (e.g., `http_requests_total[5m]`). Range vectors are used as input to functions like `rate()`, `increase()`, `avg_over_time()`. You cannot graph a range vector directly.

**Q4: How does Prometheus handle counter resets in rate()?**
A: `rate()` detects counter resets by checking if a sample's value is less than the previous sample's value. When a reset is detected, rate() treats the current value as an increment from zero (rather than a decrease). This gives correct per-second rates even when a counter-exporting process restarts.

**Compression & Storage:**

**Q5: Why is 1.37 bytes/sample achievable and what metrics does it depend on?**
A: 1.37 bytes is the empirical average across real-world Prometheus data. It depends on: (1) regularity of scrape interval (perfectly regular → 1 bit per timestamp), (2) how slowly values change (slow → XOR has many zeros → few bits per value). Counters and static gauges compress best (~0.5 bytes). Rapidly changing gauges compress worst (~3 bytes).

**Q6: How does Prometheus TSDB differ from InfluxDB's TSM engine?**
A: Prometheus TSDB: row-oriented chunks (timestamp+value interleaved per series), single-writer, block-based immutable storage. InfluxDB TSM: column-oriented (timestamps and values stored separately), LSM-tree with WAL → cache → TSM files, supports multiple writers. TSM supports tag-based indexing with TSI (Time Series Index). Both achieve similar compression ratios.

**Q7: What is the compaction strategy and why is it important?**
A: Compaction merges adjacent blocks (2h→8h→48h). Benefits: (1) Fewer blocks → faster query startup (fewer index lookups). (2) Merged index is more efficient (deduplicated symbols, merged posting lists). (3) Tombstones are applied (deleted data is physically removed). (4) Reduces file descriptor usage. Risk: compaction is CPU and I/O intensive; must be throttled.

**Scalability:**

**Q8: How would you design a TSDB for 100M active series?**
A: Shard across 10-20 Prometheus instances (functional sharding by service/namespace). Each instance handles 5-10M series. Use Thanos Query for unified view. Pre-aggregate with recording rules to reduce cross-instance query cardinality. Consider VictoriaMetrics or Mimir for higher single-instance cardinality (they use different storage engines optimized for very high cardinality).

**Q9: How do you handle multi-tenancy in a TSDB?**
A: (1) Separate Prometheus instances per tenant (strongest isolation). (2) Cortex/Mimir: built-in multi-tenancy with per-tenant limits and isolation. (3) Thanos: no native multi-tenancy; use separate object storage prefixes per tenant with RBAC in Thanos Query. For our use case: separate Prometheus per Kubernetes cluster, which maps to team boundaries.

**Q10: What's the query scalability limit?**
A: Query scalability depends on: (1) Series fan-out (how many series match the query), (2) Time range (how many chunks to read), (3) Function complexity (rate is cheap; quantile is expensive). A single Prometheus can handle ~100 QPS for typical dashboard queries. Thanos Query scales horizontally. For extreme query load, add Thanos Query Frontend with query caching and splitting.

**Operations:**

**Q11: How do you debug a slow PromQL query?**
A: (1) Check `prometheus_engine_query_duration_seconds` for query latency distribution. (2) Use Prometheus's query log (`--query.log-file`) to see exact queries and durations. (3) Use `EXPLAIN` (Prometheus 2.45+) to see the query plan. (4) Check cardinality: `count(metric_name)`. If high, add label selectors to narrow the fan-out. (5) Convert to recording rule if the query is a dashboard staple.

**Q12: How do you monitor the monitoring system?**
A: (1) Separate "meta-monitoring" Prometheus that scrapes the main Prometheus instances. (2) Alert on: `up{job="prometheus"} == 0`, `prometheus_tsdb_head_series > 8M`, `prometheus_tsdb_compaction_failed_total > 0`, `thanos_bucket_store_series_data_size_fetched_bytes`. (3) Dead man's switch: an alert that always fires; Alertmanager sends heartbeat to PagerDuty — if heartbeat stops, alerting is broken.

**Q13: How do you handle a Prometheus upgrade that changes TSDB format?**
A: Prometheus maintains backward compatibility for TSDB format. Upgrades are rolling: (1) Upgrade one HA instance, verify data continuity. (2) Upgrade the other. (3) Thanos components are version-independent (blocks in object storage use a stable format). For major version upgrades, test on a staging instance first with a copy of production data.

**Q14: What's the impact of clock skew on TSDB?**
A: Prometheus uses its own clock for timestamps, not the target's clock. If Prometheus's clock is skewed, all scraped timestamps are skewed. If a target's clock is skewed (for push-based systems), samples may be rejected as out-of-order. Mitigation: NTP on all hosts; Prometheus's out-of-order ingestion window (30 min default) absorbs minor skew.

**Q15: How do you size NVMe SSDs for Prometheus TSDB?**
A: Calculate: (1) WAL: 2 hours × ingest rate × 16 bytes/sample (uncompressed) = buffer. (2) Blocks: retention_days × daily_compressed_size. (3) Compaction headroom: 2× the largest block being compacted. (4) Total: WAL + blocks + compaction overhead + 20% free. For our case: 96 GB WAL + 1.5 TB blocks + 500 GB compaction = ~2.1 TB. Use a 4 TB NVMe with 50% headroom.

**Q16: How do you handle a "query of death" that OOMs Prometheus?**
A: (1) Set `--query.max-samples` (default 50M) to limit per-query memory. (2) Set `--query.timeout` (default 2m). (3) Use Thanos Query Frontend with query splitting (splits large range queries into smaller sub-queries). (4) Investigate and optimize the offending query. (5) If recurring, convert to a recording rule.

---

## 14. References

| # | Resource | Relevance |
|---|----------|-----------|
| 1 | [Gorilla: A Fast, Scalable, In-Memory Time Series Database (Pelkonen et al., 2015)](http://www.vldb.org/pvldb/vol8/p1816-teller.pdf) | Gorilla compression algorithm |
| 2 | [Prometheus TSDB Design](https://fabxc.org/tsdb/) | Prometheus storage engine internals (by Fabian Reinartz) |
| 3 | [Thanos: Highly Available Prometheus](https://thanos.io/tip/thanos/design.md/) | Long-term storage architecture |
| 4 | [PromQL Documentation](https://prometheus.io/docs/prometheus/latest/querying/basics/) | Query language reference |
| 5 | [InfluxDB TSM Engine](https://docs.influxdata.com/influxdb/v1/concepts/storage_engine/) | Alternative TSDB storage engine |
| 6 | [Cortex/Mimir Architecture](https://grafana.com/docs/mimir/latest/references/architecture/) | Horizontally scalable Prometheus |
| 7 | [Prometheus Histograms and Summaries](https://prometheus.io/docs/practices/histograms/) | Cardinality implications of histograms |
| 8 | [VictoriaMetrics Design](https://docs.victoriametrics.com/Single-server-VictoriaMetrics.html) | High-performance TSDB alternative |
