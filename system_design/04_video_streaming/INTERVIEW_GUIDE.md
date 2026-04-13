# Pattern 4: Video Streaming — Interview Study Guide

Reading Pattern 4: Video Streaming — 4 problems, 9 shared components

---

## STEP 1 — ORIENTATION

This pattern covers four related but meaningfully different problems: **YouTube** (user-generated VOD + search + recommendations), **Netflix** (curated VOD + proprietary CDN + DRM), **Twitch** (real-time live streaming + chat + clips), and **Video Upload Pipeline** (the upload-to-publish infrastructure as a standalone design). Together they represent the full lifecycle of video on the internet: upload, process, store, deliver, and watch.

The reason these four are grouped: they all share the same underlying mechanics — **object storage, transcoding pipelines, CDN edge delivery, HLS/DASH manifests, and ABR players** — but each one surfaces a completely different set of hard constraints. YouTube's hard problem is search and recommendations at 800M video scale. Netflix's hard problem is CDN economics at 250 Tbps and per-scene encoding quality. Twitch's hard problem is real-time ingest and sub-5-second latency to millions of concurrent viewers. The Upload Pipeline's hard problem is making a 50 GB file transfer robust, resumable, and secure.

If you can explain why each one is *different* from the others, you will immediately separate yourself from candidates who memorize a generic "video platform" template.

---

## STEP 2 — MENTAL MODEL

**Core idea**: A video system is a two-sided factory. On the ingest side, raw video comes in (whether uploaded by a user or ingested live from a camera). The factory chews it up, re-encodes it at multiple quality levels, encrypts it, and stores it as thousands of small immutable segments. On the delivery side, a CDN edge network keeps the most popular segments close to every viewer on earth, and a small smart manifest file tells the player which segments to fetch and in what order.

**Real-world analogy**: Think of a large publishing house that receives manuscripts in any format — handwritten, typed, in 20 different languages. They always convert every manuscript into standardized paperback, hardcover, and audiobook editions. The editions are then distributed to regional warehouses (CDN PoPs) so that when a bookstore orders a copy, it comes from the nearest warehouse in hours, not from the publisher's vault in days. The publisher's vault never sees most orders — only warehouse replenishment requests. The video CDN is exactly this warehouse network. The transcoding pipeline is the format conversion factory. The video segments are the editions. The HLS manifest is the table of contents that tells you which edition pages to read and in what order.

**Why it is hard**:
- **Scale**: 200+ Tbps of egress, 800M video corpus, 89,000 view events per second at YouTube. No single machine or database can hold or serve this.
- **Heterogeneity**: Billions of devices, from 4K HDR televisions to 2G mobile phones, each needing a different version of the same content.
- **Real-time constraint for live**: Twitch must encode and distribute a video stream that does not yet exist (future segments are being generated right now). Every other software pipeline operates on data that already exists. Live streaming operates on data that arrives one second at a time.
- **Durability vs. speed**: Once a creator uploads a video, losing it is unacceptable. But also, the first viewer should not have to wait 3 hours for transcoding. These goals pull against each other.
- **CDN economics**: At Netflix's scale, paying a commercial CDN for 250 Tbps would cost hundreds of millions per year. Building your own CDN saves money but requires hardware deployment in 1,000+ ISPs globally.

---

## STEP 3 — INTERVIEW FRAMEWORK

### 3a. Clarifying Questions

Ask these in the first 3-5 minutes. Do not start drawing boxes before you have the answers.

**Q1: Is this VOD (pre-uploaded content) or live streaming, or both?**
- What changes: Live streaming eliminates the ability to parallelize transcoding across future segments. You need one real-time transcoder per active stream. The latency target drops from "minutes to process" to "< 5 seconds glass-to-glass." Manifest structure changes (no `EXT-X-ENDLIST` tag, rolling window playlist).
- Red flag: Assuming VOD when the interviewer said "live" or vice versa is a fundamental miss.

**Q2: Who creates the content — users (UGC) or a content team (curated)?**
- What changes: UGC (YouTube, Twitch) means arbitrary input formats, arbitrary file sizes, content moderation at scale, DMCA/copyright detection, no pre-knowledge of what will be uploaded. Curated content (Netflix) means you control the encoding chain end-to-end, masters are delivered in known formats, and you can take 24 hours to encode without user pressure.
- Red flag: Proposing per-scene VMAF optimization for a UGC platform would be wrong — the complexity is only justified when you control the content catalog.

**Q3: What are the key latency targets — specifically TTFF (Time to First Frame) and, if live, glass-to-glass delay?**
- What changes: TTFF < 2 seconds demands: CDN-resident segments, signed URL generation in < 50ms, manifest served from edge cache, first 2-3 segments pre-fetched. Glass-to-glass < 5 seconds (standard HLS) or < 3 seconds (LL-HLS) demands segment sizes of 2 seconds or less and HTTP/2 multiplexing.

**Q4: Do we need DRM / content protection?**
- What changes: DRM adds an entire subsystem: key generation at upload time, CENC encryption of segments, PSSH box embedding in manifests, a license server at playback time, device capability detection for Widevine/FairPlay/PlayReady selection. Without DRM, you can use signed URLs (simpler). With DRM, the playback flow adds 1-2 round trips for license acquisition.

**Q5: What is the upload size limit and do we need resumable uploads?**
- What changes: Any file above ~100 MB on a real-world connection needs resumable uploads. Chunked upload with a server-side session token, chunk tracking in Redis, and S3 Multipart Upload as the backend adds significant complexity to the upload path. If max file size is 10 MB (social short-form), you can skip all of this.

---

### 3b. Functional Requirements

State these clearly at the start. For a generic "video streaming platform" (most common framing), use this base set and trim based on clarifying answers:

**Core (always include):**
- Upload video files (specify max size and format after clarifying questions)
- Transcode into multiple quality levels for adaptive bitrate streaming
- Deliver video via HLS or DASH with adaptive bitrate
- Play video on any device with < 2s TTFF

**Scope additions by problem type:**
- If search is needed: full-text search by title, description, tags; autocomplete
- If recommendations: personalized home feed + "up next" sidebar
- If live: RTMP ingest, real-time transcoding, < 5s latency to viewer
- If social: likes, comments, subscriptions
- If enterprise: DRM encryption, concurrent stream limits per account
- If UGC: content moderation (CSAM detection, copyright fingerprinting)

**How to state them clearly in the interview**: Write a numbered list on the whiteboard, then say "I'm going to focus deep on [transcoding pipeline / CDN delivery / live ingest] since that seems to be the hardest constraint here. Is that the right area?" This signals prioritization judgment.

---

### 3c. Non-Functional Requirements (NFRs)

Derive these from first principles rather than memorizing numbers. Here is how to reason through each one:

**Availability**: Video playback is user-facing and revenue-generating. 99.99% is the right number (~52 min/year downtime). The upload/transcoding pipeline can be slightly lower (99.9%) because failures are recoverable via queue retry without user impact. State this split explicitly — it shows architectural maturity.

**Durability**: Once a chunk is acknowledged during upload, it must never be lost. S3 gives 11 nines of durability. The trade-off: durability costs replication (S3 copies data across 3+ AZs). State this as non-negotiable.

**Latency (trade-offs baked in)**:
- TTFF < 2s broadband: requires CDN-resident first segments, sub-50ms manifest generation. ✅ achievable with signed URL caching. ❌ hard if every play request hits origin cold.
- Transcoding: a 7-minute video in < 5 minutes for first available quality. ✅ achievable with segment-parallel workers. ❌ sequential single-worker approach takes 30+ minutes.
- Search: < 200ms p99. ✅ Elasticsearch + Redis cache. ❌ naive DB full-text scan fails at scale.

**Scale (derive, don't memorize)**:
- 500 hours uploaded per minute → ~71 uploads/sec → ~71 × number_of_segments × number_of_qualities transcode jobs/sec
- 80M peak concurrent viewers × 2.5 Mbps avg = 200 Tbps peak egress. This number alone explains why CDN is the central architectural concern.

**Consistency**: Engagement metrics (view counts, likes, viewer counts) are eventually consistent — buffered in Redis, flushed to DB every 10-30 seconds. Financial transactions (subscriptions, payments) require strong consistency. State this split out loud — it is a common differentiator question.

---

### 3d. Capacity Estimation

**The key formula to remember**:
```
Peak egress = concurrent viewers × avg streaming bitrate
             = [DAU × avg_session_minutes × 60 / (avg_segment_duration)] × bitrate
```

For YouTube scale:
- 89,000 views/sec × average 7 min × 2.5 Mbps → ~200 Tbps peak egress
- This single number tells you: **commercial CDN is the only viable delivery mechanism at scale**; direct origin serving is impossible.

**Upload / Storage math**:
```
New storage per day = uploads/sec × avg_file_size × quality_levels × avg_rendition_size
                    = 71 uploads/sec × 86,400 × 1.6 GB ≈ 9.8 PB/day transcoded
```
This tells you: **object storage must be infinitely scalable** (GCS/S3-class), not a traditional SAN or NAS.

**Transcoding capacity math**:
```
Transcode jobs/sec = uploads/sec × segments_per_video × quality_levels
                   = 71 × 14 × 8 = ~7,952 jobs/sec at peak
```
This tells you: **K8s GPU auto-scaling is mandatory**; a fixed-size fleet would either be perpetually over-provisioned or constantly overwhelmed.

**Live ingest math (Twitch)**:
```
RTMP ingest bandwidth = concurrent_streams × avg_bitrate = 50,000 × 4 Mbps = 200 Gbps
Worker count = concurrent_streams / streams_per_worker = 50,000 / 4 = 12,500 T4 GPU instances
```
This tells you: **one transcoding worker per live stream** (not segment-parallel), because future segments do not yet exist.

**Time budget**: spend 5-7 minutes on estimation. Stop when you have derived the two or three numbers that most influence your architecture (usually: peak egress, storage growth rate, and transcoding job throughput).

---

### 3e. High-Level Design (HLD)

Draw these in this order on the whiteboard — each addition is justified by the numbers you just calculated:

**1. Client and Load Balancer**
- Clients: browser, iOS, Android, Smart TV, CTV. Routed via **Anycast IP + GeoDNS** to nearest regional endpoint.

**2. Upload Path** (if UGC)
- Upload API Service → validates auth, creates session token → writes chunks to Object Store
- Publishes `video-uploaded` event to **Kafka/Pub/Sub** on completion

**3. Object Store (S3 / GCS)**
- Raw video storage. Lifecycle rules delete raw files after transcoding + 30-day retention.
- Processed HLS/DASH segments. Key structure: `/{video_id}/{resolution}/{segment_n}.m4s`

**4. Transcoding Pipeline**
- **Segment Splitter** → FFmpeg, 30-second GOP-aligned segments
- **Job Queue** (SQS/Pub/Sub) → one message per (segment × quality)
- **GPU Worker Pool** (K8s, NVENC) → parallel FFmpeg encode, writes to temp S3 prefix
- **Coordinator** (Redis DECR counter per video_id) → triggers Assembler when all jobs complete
- **Assembler** → writes final HLS manifest, moves segments to permanent prefix

**5. CDN Edge Network**
- This box serves 200 Tbps. Draw it large. CDN fetches from origin on cache miss; serves from edge on hit. Segments: immutable, 1-year cache TTL. Manifests: short TTL (5 min for VOD, 2s for live).

**6. Stream / Playback API**
- Receives play request → validates auth + geo-restrictions → generates signed CDN URL → returns manifest URL to client

**7. Metadata Services** (as a grouped box)
- Video Metadata (Postgres/MySQL/Cassandra depending on scale)
- Search (Elasticsearch, fed by Kafka CDC)
- Recommendations (Redis cache of pre-computed ML results)
- Comment/Like/View Count services

**Key data flow to narrate** ("Watch a video" path):
1. Client requests `/watch?v=XYZ` → Web API returns metadata + thumbnail
2. Client requests `/stream/XYZ/manifest` → Playback API validates, returns signed CDN URL for `master.m3u8`
3. Client fetches `master.m3u8` from CDN → lists quality variants
4. ABR player picks quality based on bandwidth estimate → fetches variant playlist → fetches first 2-3 segments → playback begins
5. ABR continuously monitors buffer health and bandwidth; switches quality at segment boundary

---

### 3f. Deep Dive Areas

**Deep Dive 1: Transcoding Pipeline (most probed for VOD)**

The problem: a 256 GB raw 4K file needs to become 8 quality-level HLS streams within minutes of upload.

The solution: **segment-parallel GPU transcoding**:
1. FFmpeg splits raw file into 30-second GOP-aligned segments using `-c copy` (fast, no re-encode). A 2-hour movie becomes 240 segments.
2. One Kafka/SQS message per `(segment_index, quality_level)`. 240 × 8 = 1,920 jobs per video, all independently dispatchable.
3. GPU workers (NVENC, T4) consume from queue, encode, write to S3 temp prefix with idempotency token. A T4 GPU encodes H.264 1080p at ~800 fps — far faster than real-time.
4. Each worker emits a completion event. A Redis counter `DECR pending:{video_id}:{quality}` counts down to zero. When zero, Assembler runs FFmpeg concat and writes the final HLS manifest.
5. S3 `If-None-Match: *` conditional PUT makes all writes idempotent (safe to retry).

**Unprompted trade-offs to raise**:
- ✅ Segment-parallel: 15× speedup over single-machine encode. ❌ Adds coordination overhead and a stitching step.
- ✅ GPU (NVENC): 10× faster than CPU FFmpeg for H.264. ❌ Higher instance cost; not all codecs have GPU paths (AV1 is CPU-only, 100× slower — only worth it for high-view-count videos).
- ✅ Idempotent writes (conditional PUT): safe re-submission. ❌ Slightly higher S3 API cost.

**Deep Dive 2: CDN Strategy and Cache Architecture (most probed for Netflix)**

The problem: 250 Tbps peak, commercial CDN at that scale costs hundreds of millions per year.

The solution for Netflix: **Open Connect Appliances (OCAs)** — proprietary 1U/2U servers with 16-36 TB NVMe SSD embedded directly inside ISP facilities. Content is not served on-demand from Netflix's cloud; it is pre-positioned nightly based on view statistics. The fill algorithm: top-N titles in each ISP's region (based on 7-day history) are pushed to that ISP's OCAs via dedicated fiber. 95%+ of all Netflix traffic is served without ever touching the public internet or AWS.

For YouTube: **Google Global Cache (GGC)** nodes at 200+ ISP PoPs. Same concept but without proprietary hardware — Google installs GGC servers at ISP colos. CDN cache hit rate >97% for popular content.

**Key architectural insight**: Segments are content-addressed (`/{video_id}/{quality}/{segment_n}.m4s`) and immutable. `Cache-Control: public, max-age=31536000, immutable`. Once a segment is cached at a CDN PoP, it never needs to be re-fetched. All "freshness" in the video experience comes from updating the manifest (short TTL), not the segments.

**Unprompted trade-offs to raise**:
- ✅ Own CDN (Netflix OCA): dramatically lower egress cost, lower latency (1-2 hops). ❌ Capital cost of hardware, deployment relationships with 1,000+ ISPs, operational burden.
- ✅ Commercial CDN (Akamai/Cloudflare): zero CAPEX, instant global reach. ❌ Very expensive at 250 Tbps; no content pre-positioning control.
- ✅ Long segment cache TTL: CDN never re-fetches immutable content. ❌ Any encoding error in a segment is permanently cached — QA gates before publishing are critical.

**Deep Dive 3: Live Streaming Ingest (most probed for Twitch)**

The problem: 50,000 concurrent RTMP streams, each needing real-time transcoding to 6 quality levels with segments available to viewers within 2 seconds of generation.

The solution: **one GPU transcoding worker per live stream**. Unlike VOD (where you split a file), live segments do not exist yet — you cannot parallelize across future content. The parallelism is across streams (one worker per stream), not within a stream.

Flow:
1. Streamer's OBS connects to nearest **Ingest Edge PoP** via Anycast. RTMP server validates stream key (Redis lookup), buffers 2 seconds of video.
2. Ingest PoP relays RTMP to **Transcoding Cluster**. Redis `RPOPLPUSH available_workers busy_workers` atomically assigns a GPU worker.
3. Single FFmpeg process with `-filter_complex` pipes one decode to 6 simultaneous NVENC encoders (one per quality). Outputs 2-second HLS segments written to S3 continuously.
4. **Manifest Service** appends each new segment to the rolling playlist, maintains last 45 segments (90-second window), issues atomic S3 PUT.
5. CDN polls manifest every 2 seconds per quality per active stream; caches segments with 30-second TTL.

**Unprompted trade-offs to raise**:
- ✅ One worker per stream: fault isolation, simple assignment logic. ❌ Worker crash drops one stream for ~10s (recovery time from Redis pool reassignment).
- ✅ Standard HLS (2s segments, 8s latency): simpler, higher CDN cache hit ratio. ❌ Viewer sees chat 8 seconds behind streamer, kills synchronous interaction.
- ✅ LL-HLS (0.2s parts, 3s latency): much better interactivity. ❌ 10× more HTTP requests, requires HTTP/2, higher CDN origin load.
- ✅ WebRTC (< 1s latency): perfect for interactive. ❌ Does not scale to millions of viewers (WebRTC is point-to-point or SFU, not CDN-distributed).

---

### 3g. Failure Scenarios

**Frame failures at the senior level**: do not describe what breaks — describe how the system *detects, contains, and recovers* with minimal user impact.

**Transcoding worker crash mid-job (VOD)**:
- Detection: SQS/Pub/Sub visibility timeout expires (10 minutes). Message becomes visible again.
- Containment: Idempotent S3 write (temp prefix + atomic rename) means partial output is invisible to users.
- Recovery: New worker picks up the message, re-runs the same job. Output is bit-for-bit identical (deterministic FFmpeg parameters).
- User impact: Video status stays `processing` longer. No data loss. SLA: < 10 min recovery.

**Transcoding worker crash mid-live-stream (Twitch)**:
- Detection: Ingest server detects relay TCP disconnect within 1-2 seconds.
- Containment: HLS player has 45-segment buffer (90 seconds). Viewers stall only if recovery exceeds 90 seconds.
- Recovery: Redis pool manager pops a new worker, ingest server re-establishes relay. Worker resumes from current live position (missed frames are not recoverable — this is a known trade-off of live).
- User impact: Brief rebuffer (typically < 10 seconds). Streamer may see "preview disconnected" in OBS.

**CDN PoP outage**:
- Detection: GeoDNS health checks remove unhealthy PoP within 30-60 seconds.
- Containment: Traffic reroutes to next-nearest PoP. Viewers may see brief rebuffer as player fetches from further edge.
- Recovery: Transparent to users after rerouting. Segments are immutable and multi-PoP replicated.

**Database primary failure (MySQL/Vitess)**:
- Detection: VTOrc (MySQL orchestrator) detects missed heartbeat within 5-10 seconds, promotes replica.
- Containment: Write queue buffers operations during 10-30s failover window. Reads continue on replicas uninterrupted.
- Recovery: New primary ready, topology updated, application clients reconnect.
- **Senior framing**: Note that video playback is not blocked by a MySQL failure — all segments are on CDN. Only metadata writes (view counts, comments) are delayed. This is the payoff of separating the API plane from the media plane.

**Viral video causes CDN origin overwhelm**:
- Detection: CDN miss ratio spikes, origin latency/error rate increases.
- Containment: CDN **request coalescing** (collapsed forwarding) — if 10,000 concurrent requests arrive for the same uncached segment, CDN makes one origin request and serves all 10,000 clients from the single response.
- Recovery: After first fill, cache hit rate → 99.9% for that segment. Origin pressure drops to near zero.

**AWS region outage (Netflix)**:
- Detection: Route 53 health checks + CloudWatch within 60 seconds.
- Containment: Cassandra data is fully replicated across 3 regions (us-east-1, us-west-2, eu-west-1). OCAs are not AWS-dependent — they continue serving video.
- Recovery: Route 53 failover routes traffic to healthy regions. MySQL (subscriptions) fails over via RDS Multi-AZ (60-120s RTO).

---

## STEP 4 — COMMON COMPONENTS

These 9 components appear across all four problems. Memorize what they do, why each is used, the key configuration, and what breaks without it.

### CDN (Content Delivery Network)

**Why used**: 200 Tbps of video egress cannot come from your data center. CDN distributes segments to 200+ global PoPs; each PoP serves local viewers without cross-internet hops. Cache hit rate > 90% means origin servers see < 10% of total requests.

**Key configuration**: Segments are immutable → `Cache-Control: public, max-age=31536000, immutable`. Manifests are short-lived → 2-5 minute TTL for VOD, 2-second TTL for live. Signed URLs (HMAC-SHA256) prevent unauthorized access without CDN key sharing.

**What breaks without it**: Origin servers receive 200 Tbps → immediate bankruptcy-level infrastructure cost plus unbounded latency. The math makes CDN non-negotiable above ~1 Gbps.

### Object Storage (S3 / GCS)

**Why used**: Unlimited scale, 11-nines durability, no capacity planning, built-in lifecycle rules for automatic deletion of temp/raw files. Video segments are perfect blobs: write-once, read-many, never updated.

**Key configuration**: S3 multipart upload for files > 100 MB (enables parallel chunk writes, required for files up to 50 GB). Lifecycle rules: delete raw uploads after 30 days post-transcoding; delete live HLS rolling segments 2 hours after stream end. Cross-region replication for disaster recovery.

**What breaks without it**: Traditional NAS/SAN at petabyte scale is operationally unmanageable and astronomically expensive. You need object storage semantics (content-addressed keys, no directories, HTTP GET) to integrate cleanly with CDN origin pulls.

### HLS + DASH Adaptive Bitrate Streaming

**Why used**: Viewers span 2G mobile phones to 4K fiber connections. A fixed-bitrate stream either buffers on slow connections or wastes bandwidth on fast ones. HLS/DASH manifests let the client's ABR algorithm select the highest quality the current connection can sustain, switching per-segment without rebuffering (using CMAF fMP4 with aligned timestamps).

**Key configuration**: 2-second segments for fast ABR reaction. CMAF (fragmented MP4) containers allow one set of segments to be referenced by both HLS (`.m3u8`) and DASH (`.mpd`) manifests, avoiding double storage. ABR algorithm: measure throughput after each segment download → select quality where `bitrate ≤ throughput × 0.85` (safety margin) → additionally downgrade if buffer < 10 seconds regardless of throughput.

**What breaks without it**: Fixed-bitrate streaming results in constant buffering on mobile or wasted bandwidth on fast connections. Without HLS/DASH, you cannot serve video to iOS devices (Safari requires HLS) or to non-Google browsers without HLS (DASH).

### GPU-Accelerated Transcoding with FFmpeg

**Why used**: CPU-only H.264 encoding runs at ~80 fps on a modern server core. A single NVIDIA T4 GPU running NVENC encodes H.264 at ~800 fps. A 2-hour movie at 1080p takes ~45 minutes CPU-only, ~5 minutes with NVENC. At 71 uploads/second with YouTube-scale throughput, CPU-only transcoding would require an order of magnitude more servers.

**Key configuration**: `ffmpeg -hwaccel cuda -c:v h264_nvenc -preset p5` for GPU encode. `-g 60 -keyint_min 60 -sc_threshold 0` for fixed keyframe intervals (required for HLS segment boundaries). `-force_key_frames expr:gte(t,n_forced*30)` during segment splitting pre-pass.

**What breaks without it**: Transcoding queue backlog grows faster than it can be drained at scale. Creators wait hours for their video to be published. Real-time live transcoding is impossible — GPU is a hard requirement for Twitch's one-worker-per-stream model.

### Segment-Parallel Transcoding Pipeline

**Why used**: Sequential single-machine transcoding of a 2-hour movie at 8 quality levels takes hours. Splitting the movie into 240 thirty-second segments and dispatching 240 × 8 = 1,920 parallel jobs reduces wall-clock time to < 20 minutes. The speedup is linear in the number of workers up to the number of segments.

**Key configuration**: Split pass uses `-c copy` (no re-encode, just demux to segments) → very fast O(filesize/disk_speed). Each segment job is fully independent (reads only its own input, writes to a temp S3 prefix). Redis DECR counter per `video_id` tracks completion — when counter reaches 0, triggers Assembler. S3 conditional PUT (`If-None-Match: *`) makes all writes idempotent.

**What breaks without it**: A 2-hour video upload cannot be processed in < 30 minutes without parallelism. The SLA from "upload complete" to "first quality available" fails for any normal-length content.

### Kafka / Pub-Sub Event Bus

**Why used**: The transcoding pipeline has ~8 independent stages (metadata extraction, moderation, DRM key provisioning, segment splitting, parallel transcoding, assembly, thumbnail generation, search indexing). Without an event bus, these must be chained via synchronous RPC calls, creating a brittle sequential pipeline where one slow stage blocks all others. Kafka decouples each stage; each publishes an event on completion and subscribes to upstream events.

**Key configuration**: `video-uploaded` topic (key = `video_id`, 7-day retention for replay). `transcode-jobs` topic for individual segment-quality jobs (or SQS for per-job visibility timeout). Dead-letter queues (DLQ) for jobs that fail 3 times. `DECR` in Redis coordinates completion detection without polling.

**What breaks without it**: Tightly coupled synchronous pipeline means any stage failure blocks the entire video processing. Recovery from transient failures requires replaying the entire pipeline from the beginning. Re-processing old videos for a new codec becomes operationally impossible without event replay.

### Redis

**Why used**: Everything in video streaming needs sub-millisecond ephemeral state: active upload session tracking, concurrent stream counts, viewer counts, trending sorted sets, worker pool management, rate limiting, recommendation caches, and ABR state. Redis is the right tool for all of these because they require atomic operations (INCR, ZADD, RPOPLPUSH), sub-millisecond latency, and are small enough to fit in RAM.

**Key configuration by use case**:
- View/viewer counts: `INCR view:{video_id}`, flush to DB every 30s
- Concurrent streams (Netflix): `SET active_streams:{account_id} {count} EX 90` — 90-second TTL means dead streams auto-expire without explicit cleanup
- Transcoding worker pool (Twitch): `RPOPLPUSH available_workers busy_workers` — atomic claim without double-assignment
- Directory sorted set (Twitch): `ZADD live:{game_id} {viewer_count} {channel_id}` — `ZREVRANGE` for paginated browse
- Upload session state: `HSET session:{id} received_bytes {n}` + Redis Bitfield for chunk tracking
- Upload chunk completion: `SETBIT chunks:{session_id} {chunk_number} 1`

**What breaks without it**: Without Redis, all of the above either go to the main DB (RDBMS cannot handle 89,000 INCR/second for view counts) or require distributed coordination protocols that are far more complex.

### DRM (Digital Rights Management)

**Why used**: Netflix, studios, and any platform with licensed content cannot allow free copying of encrypted streams. DRM binds decryption to a licensed device, making downloaded/intercepted segments unplayable without a valid license from the license server.

**Key configuration**: CENC (Common Encryption) encrypts segment bytes once with a per-video AES-128 content key. Three DRM "wrappers" are supported via separate PSSH boxes in the manifest: Widevine (Google/Android/Chrome), FairPlay (Apple iOS/Safari), PlayReady (Microsoft Windows/Xbox). AWS KMS envelope encryption: content key encrypted by KMS master key, stored as ciphertext in DB — KMS is only called at key generation time, not per-license (license server decrypts the stored ciphertext using the master key, which KMS never reveals in plaintext).

**What breaks without it**: DRM-free platforms violate licensing agreements with studios; content becomes trivially extractable from any stream intercept. For offline downloads specifically, DRM is the only mechanism that binds a downloaded file's playability to license validity.

### Pre-Signed URLs + Signed Manifests

**Why used**: Video segments on CDN are public URLs by path. Without signature enforcement, anyone who guesses or extracts a segment URL can download it freely — bypassing subscription requirements, geo-restrictions, and DRM. Pre-signed URLs add an expiring HMAC-SHA256 signature to every manifest and streaming URL, validated at the CDN edge.

**Key configuration**: `HMAC-SHA256(video_id + user_id + expiry + region + secret_key)`. CDN validates signature before serving. Expiry typically 1-4 hours for streaming manifests; 24 hours for upload session tokens. For upload: pre-signed S3 upload URLs allow the client to PUT directly to S3 without routing through the upload API server (saves API server bandwidth for large files).

**What breaks without it**: Geo-restrictions cannot be enforced at the CDN edge. Subscription-gated content becomes accessible to non-subscribers. Without signed upload URLs, the upload API service must proxy all upload data through itself, creating a massive bandwidth bottleneck.

---

## STEP 5 — PROBLEM-SPECIFIC DIFFERENTIATORS

### YouTube

**Unique thing 1: Scale of search and recommendations across 800M videos.** YouTube has ~800M indexed videos each with auto-generated captions (via Speech-to-Text) in 125+ languages, creating a full-text search index many times larger than any non-video platform's. The two-tower neural net + ScaNN ANN index serves 500 video candidates in < 10ms against 800M embeddings. No other problem in this folder comes close to this ML infrastructure complexity.

**Unique thing 2: Content ID copyright fingerprinting.** Every uploaded video is fingerprinted using spectral audio analysis + perceptual visual hashing (Photon algorithm), compared via LSH against a database of rights-holder assets. Matches trigger automated actions (block/monetize/track) configured per rights holder. This runs in parallel with transcoding and adds significant pipeline complexity absent from Netflix (which controls its own content) and Twitch (which handles this differently for VODs).

**2-sentence differentiator**: YouTube's defining architectural challenge is the combination of UGC scale (71 uploads/second in arbitrary formats) with the world's largest video search and recommendation engine — requiring Bigtable counter sharding for billions of daily view events, a two-tower ML recommendation model with ScaNN ANN index over 800M video embeddings, Speech-to-Text caption generation per upload, and a real-time Content ID copyright fingerprinting system. These analytics and ML subsystems do not appear in any other problem in this pattern, making YouTube the most "full-stack data engineering" problem of the four.

---

### Netflix

**Unique thing 1: Open Connect CDN — proprietary hardware inside ISPs.** Netflix built their own CDN rather than using Akamai or Cloudflare because at 250 Tbps, commercial CDN costs hundreds of millions per year and latency from ISP-embedded hardware (1-2 hops from user) is measurably lower than commercial CDN transit. 1,000+ ISP locations receive nightly content fill (top-N titles by ISP region), and 95%+ of traffic never leaves the ISP network.

**Unique thing 2: Per-scene dynamic bitrate encoding (VMAF-targeted).** Rather than a fixed bitrate ladder (e.g., "1080p always at 5 Mbps"), Netflix analyzes each scene for spatial and temporal complexity, uses convex hull optimization to find the minimum bitrate that achieves VMAF=93, and encodes each scene at a different bitrate. A static documentary scene that hits VMAF=93 at 1 Mbps is not encoded at 5 Mbps. This saves ~30% bandwidth vs. fixed ladders — at Netflix's scale, 30% of 250 Tbps is 75 Tbps, or hundreds of millions per year in CDN costs.

**2-sentence differentiator**: Netflix's defining architectural choices are the Open Connect proprietary CDN (embedded inside ISPs, pre-filled nightly with the most popular titles, serving 95%+ of global traffic without any commercial CDN vendor) and VMAF-targeted per-scene encoding (which treats each scene as having its own optimal bitrate rather than applying a one-size-fits-all ladder). Every other design decision at Netflix — multi-DRM, Chaos Monkey, EVCache, 1,000+ A/B experiments — exists in service of maximizing streaming quality while minimizing the cost of those 250 Tbps.

---

### Twitch

**Unique thing 1: Real-time RTMP ingest with one GPU worker per live stream.** Twitch is the only problem in this folder where the source video does not exist yet. Unlike VOD (where you can split a file), a live stream arrives one second at a time, making segment-parallel transcoding impossible. One dedicated GPU worker per stream is the correct architecture — it provides fault isolation (a worker crash affects only one channel), predictable latency, and linear scaling (add workers to add stream capacity).

**Unique thing 2: IRC-based live chat fan-out at scale.** Twitch chat is an IRC protocol implementation over WebSockets with Redis Pub/Sub fan-out per channel. A single popular channel with 100,000 concurrent chatters generates ~83 chat messages/second each requiring delivery to 100,000 WebSocket connections — 8.3 million write operations per second for that one channel. The chat system is as complex an engineering challenge as the video system itself, and it does not appear in any other problem in this folder.

**2-sentence differentiator**: Twitch is the only problem in this folder where the video is being generated in real time (RTMP ingest → live transcoding → 2-second HLS segment delivery → CDN edge → viewer), making the ingest edge server fleet, one-worker-per-stream transcoding model, and LHLS latency optimization the central architectural challenges, with chat fan-out via IRC over WebSocket being an equally complex second system that no other video platform in this folder has to design. The key insight is that live streaming and VOD are architecturally distinct at every layer — ingest, transcoding, manifest generation, and CDN TTLs — not just "the same thing but faster."

---

### Video Upload Pipeline

**Unique thing 1: Resumable chunked upload with TUS 1.0 + S3 Multipart backend.** This problem uniquely treats the upload mechanism itself as the hard system design problem. A 50 GB file over a residential connection takes hours. The TUS protocol (HEAD to get last received byte, PATCH per chunk with offset, automatic completion detection) combined with Redis Bitfield chunk tracking and S3 Multipart Upload as durable backend creates a system where any failure — browser close, network drop, mobile app backgrounding — can be resumed exactly from the last byte without data loss.

**Unique thing 2: Full DRM key lifecycle and pipeline audit trail.** This is the only problem that designs the DRM infrastructure end-to-end: CENC encryption by Shaka Packager, KMS envelope encryption for content key storage (key is never in plaintext in the DB), PSSH box generation for Widevine/FairPlay/PlayReady, and a stateless license server at playback time. Combined with a full `pipeline_events` audit table that tracks every stage transition (uploading → assembling → moderating → transcoding → published) with worker IDs and durations, this problem is about **operational correctness and auditability** in a way the other three problems are not.

**2-sentence differentiator**: The Video Upload Pipeline is the only problem in this folder that treats the upload and processing infrastructure itself as the product — focusing on resumable chunked uploads (TUS 1.0 + S3 Multipart), 2D parallel transcoding coordination (Redis DECR counter triggers assembly), DRM key lifecycle management (CENC + KMS envelope encryption + PSSH boxes), and a full moderation pipeline running in parallel with transcoding (CSAM hash matching + ML violence/hate speech classifiers + audio fingerprinting). Unlike the other three problems which are about serving content to consumers, the Upload Pipeline is entirely about the creator's upload-to-publish experience — robustness, resumability, correct status tracking, and pipeline audit trails are the primary NFRs rather than egress bandwidth.

---

## STEP 6 — Q&A BANK

### Tier 1 — Surface Questions (5-7 questions, 2-4 sentence answers)

**Q: What is adaptive bitrate streaming and how does it work?**
**Answer**: ABR streaming encodes video at multiple quality levels (e.g., 144p through 4K) and stores each as small 2-4 second segments. An HLS or DASH manifest file lists all available quality variants. The player downloads a few segments, measures available bandwidth and buffer health, then selects the highest quality level whose bitrate fits within available throughput. When network conditions change, the player switches quality at the next segment boundary without buffering — because all quality levels share the same timestamp alignment (CMAF).

**Q: Why do we split videos into 2-second segments instead of serving the whole file?**
**Answer**: Segments enable two critical behaviors. First, the player can start playback after buffering just 2-3 segments (~6 seconds of video) rather than waiting for the entire file to download. Second, the ABR algorithm can react to network changes in 4-6 seconds rather than waiting until the entire file download reveals a bandwidth change. The trade-off: more HTTP requests (mitigated by HTTP/2 multiplexing) and more objects to cache, but the latency and quality benefits overwhelmingly justify it.

**Q: What is the role of the CDN in video streaming and why is it the most important component?**
**Answer**: The CDN caches immutable video segments at 200+ global PoPs so that each viewer fetches from a server within tens of milliseconds of their location rather than from a central data center. At 200 Tbps peak egress, there is no alternative — origin servers simply cannot handle per-viewer load. The 90-97%+ CDN cache hit rate means origin servers see < 10% of actual traffic, making the entire system economically viable.

**Q: How do you handle a video that goes viral and suddenly has millions of concurrent viewers?**
**Answer**: The CDN edge handles this through **request coalescing** (collapsed forwarding) — if 10,000 users simultaneously request an uncached segment, the CDN issues a single origin request and serves all waiting clients from that one response. After the first fill, cache hit rate is essentially 100% for that segment. The only system that sees elevated load is the metadata API, which is protected by Redis cache (5-minute TTL) and circuit breakers that fall back to trending content on failure.

**Q: How does HLS work at a protocol level?**
**Answer**: HLS uses two types of manifest files served as plain text. A master playlist (`master.m3u8`) lists all quality variants with their bandwidth requirements and codec information. Each quality variant has its own playlist (`720p.m3u8`) that lists the sequential 2-second segment files and their URLs. The client fetches the master playlist once, picks a quality variant, then polls the variant playlist every segment duration (2 seconds for live, as-needed for VOD) to get the next segment URL. For live streams, new segments are appended as they are generated; the `EXT-X-ENDLIST` tag indicates a VOD (complete) stream.

**Q: What database would you use for video metadata and why?**
**Answer**: For video metadata that is read-heavy, written infrequently after publication, and must support queries by video_id and uploader_id, MySQL with Vitess sharding (YouTube's actual choice) or PostgreSQL is appropriate. For view counts (write-heavy, high frequency, eventual consistency acceptable), Bigtable or Redis INCR with periodic flush is better. For watch history (time-series append, per-user query), Cassandra with profile_id as partition key works well. The polyglot approach — different stores for different access patterns — is intentional and correct.

**Q: What is the difference between HLS and DASH?**
**Answer**: HLS (HTTP Live Streaming) was created by Apple and is required for iOS/Safari. DASH (Dynamic Adaptive Streaming over HTTP) is an open standard used on Android, Chrome, and most non-Apple platforms. Both work the same way (manifest + segments), but they use different manifest formats (`.m3u8` for HLS, `.mpd` for DASH) and historically used different container formats. CMAF (Common Media Application Format) introduced in 2016 allows a single set of fMP4 segment files to be referenced by both manifests, so modern platforms encode once and serve both protocols from the same segments.

---

### Tier 2 — Deep Dive Questions (5-7 questions, reason through trade-offs)

**Q: How does Netflix's per-scene encoding work and why is it better than a fixed bitrate ladder?**
**Answer**: Fixed-bitrate encoding applies the same bitrate to every scene regardless of content complexity — a static talking-head scene at 5 Mbps is wasteful; a fast-motion action scene at 5 Mbps may look poor. Netflix's per-scene approach: (1) Pre-process each scene to compute spatial information (SI) and temporal information (TI) using FFmpeg `signalstats`. (2) Encode that scene at many bitrate/resolution combinations. (3) Score each encoding with VMAF — Netflix's quality metric trained on human opinion scores. (4) Select the minimum bitrate that achieves VMAF=93 (their quality target). The result: a documentary might stream at 1 Mbps while matching the quality of a fixed 3 Mbps encode, while an action film allocates more bits where they're perceptually needed. This saves ~30% bandwidth globally without any perceived quality reduction — at Netflix's scale, 30% of 250 Tbps is 75 Tbps.
- Trade-off ✅: massive bandwidth savings, consistent perceived quality. ❌: 10,000+ CPU-hours per title, 100× encoding cost over simple VBR encoding. Only viable when you control the content catalog (not UGC).

**Q: How would you design a system that ensures a 50 GB upload is resumable after network failures?**
**Answer**: The TUS protocol (open standard) combined with S3 Multipart Upload backend is the right architecture. On initiation, the server creates an upload session record in PostgreSQL and initializes S3 Multipart Upload (`CreateMultipartUpload` returns an `uploadId`). The client uploads 5 MB chunks via PATCH requests with `Upload-Offset` header. The server forwards each chunk to S3 `UploadPart`, stores the returned ETag in Redis, and increments a Redis Bitfield to mark the chunk received. On failure, the client sends `HEAD` to get the current `Upload-Offset`; the server reads from Redis (or calls S3 `ListParts` if Redis state was lost) and returns the exact byte from which to resume. S3's `CompleteMultipartUpload` assembles all parts atomically on final chunk receipt. S3 Lifecycle Rules abort any incomplete multipart upload older than 2 days to prevent orphaned storage charges.
- Key insight: the server never stores file data locally — it streams each chunk directly to S3, making the upload service stateless and horizontally scalable.

**Q: How do you prevent a viral video from overwhelming your origin storage?**
**Answer**: Three layers of protection. First, CDN **request coalescing** (collapsed forwarding): the CDN edge collapses simultaneous cache misses for the same segment into a single origin request. Second, **origin rate limiting**: the origin responds to CDN miss requests with controlled rate (e.g., max 100 concurrent origin fetches per segment), queuing excess with `Retry-After`. Third, **pre-warming for anticipated events**: for a scheduled premiere, the CDN is pre-warmed by issuing synthetic requests to all PoPs before the release time, so the first real viewer request hits a warm cache. The key insight is that video segments are immutable and content-addressed — once a PoP has cached a segment, it never needs to re-fetch it for the lifetime of that TTL, which for segments is 1 year.

**Q: How does Twitch's live chat scale to 100,000 concurrent chatters in a single channel?**
**Answer**: The IRC-over-WebSocket architecture with Redis Pub/Sub fan-out handles this as follows. A viewer's chat message arrives at an IRC Ingress Server, which applies rate limiting, runs AutoMod spam/harassment detection, assigns a message ID, and publishes to a Redis Pub/Sub channel keyed by channel_id. Chat Distribution Servers — dedicated to this channel based on consistent hashing — subscribe to this Redis channel and receive every message. Each Distribution Server maintains WebSocket connections to ~50,000 viewers; receiving a Pub/Sub message triggers a WebSocket push to all local connections. The fan-out math: 83 messages/second × 100,000 WebSocket connections = 8.3 million write operations per second for a single top channel. The trade-offs: Redis Pub/Sub delivers in-memory with sub-millisecond latency but does not persist messages — if a Distribution Server is down when a message is published, that subscriber's clients miss the message (acceptable for live chat). The 50,000-connections-per-server limit means 2 million concurrent viewers require ~40 servers per region at 60% utilization.

**Q: How do you implement concurrent stream limits for Netflix (e.g., Standard plan: max 2 simultaneous streams)?**
**Answer**: Active stream sessions are tracked in Redis (not the primary DB) because the check must be sub-millisecond. On every play request, the Playback API does: (1) Query `active_streams:{account_id}` in Redis — a hash of `{stream_id: last_heartbeat_timestamp}`. (2) Expire entries where `last_heartbeat_timestamp < now - 90 seconds` (dead streams that didn't send a stop event). (3) Count remaining active entries. If count >= `max_streams` for the tier, reject with HTTP 409. Heartbeats are sent every 60 seconds from the client; the 90-second TTL handles crashes and network drops without requiring explicit stop events. Trade-offs: ✅ sub-millisecond check, no DB write on heartbeat path. ❌ edge case: if Redis fails, the check fails open (allow streaming) or closed (deny streaming); Netflix fails open to avoid blocking paying users, accepting occasional tier limit bypass.

**Q: How would you handle re-encoding all videos when a new codec (AV1) becomes widely supported?**
**Answer**: Do not re-encode everything simultaneously — the compute cost is enormous and storage doubles during transition. Instead: maintain a `codec_version` field in `video_renditions`. Run a background re-encode job queue that processes videos in priority order: view count descending (top videos generate the most bandwidth, so savings per video are highest). Set a threshold — videos with fewer than, say, 100,000 views may never justify re-encoding (CDN bandwidth savings don't exceed compute cost). The re-encode jobs are identical to the original pipeline (segment-parallel, GPU, idempotent S3 writes) but use `libaom-av1` or a hardware AV1 encoder. A/B test: serve AV1 renditions only to devices that report AV1 hardware decode support (via device capability database populated at app installation) — this avoids the high CPU decode cost on older devices. At YouTube scale, AV1 encoding saves ~30% over VP9, which at 22 Tbps average egress is ~6.6 Tbps in CDN savings, easily justifying multi-year re-encoding campaigns.

**Q: How does DRM prevent someone from downloading Netflix videos by intercepting HTTPS traffic?**
**Answer**: DRM does not rely on HTTPS confidentiality alone. Video segments are encrypted at the application layer with AES-128 CBC/CTR (CENC) using a per-video content key. The content key is never transmitted — it is acquired via a separate Widevine/FairPlay/PlayReady license request to the license server. The license binds the content key to a specific device via hardware-backed key storage: on Android, the content key is stored in the Widevine Trusted Execution Environment (TEE); on iOS, in the Secure Enclave. Even if an attacker intercepts all HTTPS traffic, they receive: (1) encrypted segments (useless without the key), and (2) a license containing the key encrypted by the device's hardware key (useless on any other device). The only successful attack vector is extracting the key from the TEE itself, which requires physical hardware access and is impractical at scale.

---

### Tier 3 — Staff+ Stress Test Questions (3-5 questions, reason aloud)

**Q: Netflix is launching a globally simultaneous release of a major show in all 190 countries at midnight UTC. Walk me through your architecture to ensure zero failures at launch.**

Reason aloud: The biggest risks are OCA fill failure (content not reaching all 1,000+ ISP locations in time) and surge traffic before pre-warming completes.

Pre-launch actions (48+ hours before): Trigger OCA fill for all regions simultaneously, not sequentially. Monitor fill completion per OCA via management network — alert on-call if any OCA < 100% fill by T-4 hours. Verify Playback API can correctly route to OCAs for the new title_id (metadata published to all services before launch). Run a "pre-flight" synthetic playback request from 10 global test clients at T-1 hour.

At launch: The DNS/route change happens at UTC midnight — the first requests arrive at Playback API. Playback API has already validated all OCAs for this title. All OCA requests are cache hits (content was pre-filled). Origin (AWS S3) is not involved in any playback request — OCAs are fully loaded. The bottleneck is Playback API request volume: 50M potential subscribers × possible launch surge → ensure Playback API auto-scaling groups are pre-scaled to 2× expected capacity 30 minutes before launch (based on historical premiere data).

What I would instrument: Playback API error rate per region (Atlas alert if >0.1%), OCA fill percentage per ISP (100% by T-4h or escalate), first-segment-served latency p99 per region, client rebuffer ratio in first 10 minutes.

Failure mode I would pre-mitigate: One ISP's OCA goes down at T-10 minutes. Playback API has fallback routing to the nearest Open Connect IX cluster, then AWS CloudFront. The ISP's subscribers see slightly higher latency but continue playing. This is why we maintain the fallback hierarchy and never rely solely on a single OCA.

**Q: YouTube's transcoding farm is falling behind — the processing queue is 48 hours deep and growing. You're on-call. What do you do and how do you redesign to prevent this?**

Reason aloud: Immediate response vs. systemic fix are different tracks. Run them in parallel.

Immediate (minutes): Confirm the nature of the bottleneck. Is it GPU capacity (queue depth growing, workers saturated), or something upstream (workers are idle but jobs aren't being dispatched — queue coordination bug, dead-letter queue full)? Check Pub/Sub queue depth metrics, GPU worker CPU/memory utilization, DLQ depth. If workers are saturated, emergency-scale GPU fleet (K8s `kubectl scale deployment transcoding-workers --replicas=N`). Spot instances first; on-demand as backup. Triage: prioritize highest-impact videos (creators with large audiences, trending topics) using priority queues — `HIGH_PRIORITY` queue jumped ahead of `STANDARD` queue.

Medium-term (hours): Investigate root cause. Was there a sudden traffic spike (a major world event where millions of creators uploaded simultaneously)? Did a dependency go down (GCS unavailable for an hour causing job retries to stack up)? Was there a config push that reduced worker count?

Systemic redesign: Implement predictive auto-scaling — monitor upload rate as a leading indicator. If upload rate spikes, begin GPU fleet expansion before the transcoding queue grows. Add rate limiting on the upload API that gracefully slows acceptance during queue saturation (return 429 with `Retry-After` header) — this buys time without dropping uploads. Implement a "fast track" pipeline that makes lower-quality renditions (720p, 480p) available within 5 minutes while higher qualities (4K, AV1) process on the standard queue. Creator priority tiers: verified channels above 1M subscribers get fast-track on all qualities.

**Q: You're designing the Twitch chat system from scratch. 2 million users are connected. A popular streamer ends their stream and instantly starts a new one — all 200,000 viewers of that channel need to reconnect. How do you handle the reconnection surge?**

Reason aloud: This is a thundering-herd problem at the WebSocket connection layer.

The stream-end event is published to a notification service which must fan out to 200,000 connected clients. Two failure modes: (1) all 200,000 clients simultaneously poll for stream status and simultaneously attempt new WebSocket connections, overwhelming chat servers; (2) the load balancer/connection limit on chat servers becomes a bottleneck.

Mitigation at the stream-end event: Instead of sending a "stream ended" event that triggers immediate reconnect, send a "stream restarted in X seconds" event where X is jittered per client (e.g., `hash(user_id + stream_id) mod 30` seconds). This spreads 200,000 reconnects over 30 seconds (6,667 reconnects/second) rather than simultaneously (200,000 at once). The client holds the existing WebSocket open during this wait period.

For the new stream's chat server assignment: Chat Distribution Servers use consistent hashing for channel → server assignment. The new stream's channel_id maps to the same server cluster as the old stream (same channel), so viewers reconnect to the same servers they were already connected to. No re-balancing needed. This is a critical design choice: chat server assignment must be by channel_id, not stream_id, to survive stream restarts.

Redis Pub/Sub subscriptions are re-established by the Chat Distribution Servers automatically when a new stream begins for a channel they host. This is a server-side operation invisible to viewers.

**Q: Walk me through how you would add AV1 codec support to an existing YouTube-scale system that currently only serves H.264 and VP9, without breaking any existing viewers or requiring a full re-architecture.**

Reason aloud: This is an incremental adoption problem. The constraints are: AV1 encoding is 100× slower than H.264 (CPU-only, no stable hardware encoder at launch), only newer devices can decode AV1, and you have 800M existing videos already encoded as H.264/VP9.

Step 1 — Device capability detection: Add AV1 MIME type check to the player SDK on app launch. Report `supported_codecs: ["av1", "vp9", "h264"]` to the Video Metadata service, stored in a per-device capability registry (Redis, keyed by device fingerprint, 30-day TTL). No new video delivery yet.

Step 2 — New uploads only: Modify the transcoding pipeline to optionally add an AV1 rendition for videos above a quality threshold (≥720p source). Add to the job queue after H.264/VP9 jobs complete (not blocking initial publication). AV1 rendition uses `libaom-av1` or SVT-AV1 (faster, better for high-throughput). New `video_renditions` rows with `codec=av1`. Manifest generator adds AV1 variant entries with correct CODECS string (`"av01.0.08M.08"`).

Step 3 — ABR player update: Update Stream API to check device capability registry. For AV1-capable devices, return a master manifest that includes AV1 variants listed first (highest preference). Player picks AV1. For non-capable devices, master manifest contains only VP9/H.264 — AV1 variants not exposed. No A/B testing risk: fallback is automatic via manifest filtering.

Step 4 — Background re-encode of existing videos: Run a re-encode batch job sorted by `view_count DESC`. Only encode videos above threshold (e.g., > 500K views — below this, bandwidth savings don't justify CPU cost). Each re-encode job is identical to the new-upload pipeline (segment-parallel, idempotent S3 write). Timeline: at YouTube's scale, re-encoding the top 50M videos (80% of view traffic) takes months — acceptable as a gradual rollout.

Step 5 — Monitoring: Track AV1 session ratio, bitrate savings (AV1 bitrate / VP9 bitrate should be ~0.7), rebuffer ratio (ensure AV1 decode doesn't cause stalls on borderline-capable devices), and DLQ depth (AV1 jobs are slow — ensure they don't block H.264 jobs via separate queue or explicit priority).

---

## STEP 7 — MNEMONICS

### The "SECAMP" Mnemonic (for HLD components)

**S** — Segments + Object Storage (S3/GCS — where all video lives)
**E** — Encoding / Transcoding Pipeline (segment-parallel GPU workers)
**C** — CDN Edge (the dominant scaling mechanism; 90-97% cache hit rate)
**A** — ABR + Manifest (the protocol that lets clients adapt quality)
**M** — Metadata Services (search, recommendations, video info)
**P** — Playback/Stream API (auth, signed URLs, geo-restriction gateway)

Draw these left-to-right: S → E → C → A (client-facing), with M and P as vertical layers that serve the client during playback. Every video streaming HLD is a variation on this six-component skeleton.

### The "Two Worlds" Mental Separation

Always frame the system as two completely separate planes:

**The API plane** (stateful, metadata, auth): small payloads, needs DB reads, must be authenticated, serves ~89,000 QPS. Protected by load balancers, Redis cache, circuit breakers.

**The media plane** (stateless, binary segments, no auth per-byte): massive payloads (200 Tbps), served entirely by CDN, no DB reads at all, cache hit rate 97%. A CDN PoP and a MySQL database have nothing to do with each other at serving time.

Interviewers love candidates who say: "The beauty of this architecture is that the media plane is completely CDN-driven — a MySQL outage does not affect video playback, only metadata." This is a senior-level insight.

### Opening One-liner

When the interviewer says "design YouTube" or "design a video streaming platform," open with:

> "At its core, this is a two-sided system: an ingest factory that converts arbitrary raw video into standardized HLS/DASH segments at multiple quality levels, and a CDN-first delivery network that keeps those immutable segments close to every viewer. The interesting engineering is in making the factory fast enough that creators don't wait, and making the CDN smart enough that 200 Tbps never touches our origin servers. Can you tell me a bit about scope — are we doing VOD or live streaming, and is this user-generated content or curated?"

This one-liner demonstrates architectural understanding, invites the clarifying question naturally, and signals you know what makes video hard.

---

## STEP 8 — CRITIQUE

### What is well-covered in these source files

- **Transcoding pipeline**: extremely thorough. Segment splitting, parallel jobs, GPU worker configuration, idempotent writes, coordinator patterns with Redis DECR — all present with actual FFmpeg commands and S3 conditional PUTs.
- **CDN architecture**: both the YouTube GGC and Netflix OCA approaches are covered in exceptional detail, including OCA hardware specs, fill strategy, OCA selection algorithm, and failure handling.
- **Live streaming mechanics**: Twitch's one-worker-per-stream model, LL-HLS vs standard HLS latency trade-offs, the chat fan-out math, and the clip 90-second rolling buffer are all well explained.
- **Data models**: full SQL schemas with indexes, partition strategies, and sharding decisions for all four problems — unusually complete for study material.
- **Capacity estimation**: all four problems have end-to-end math with labeled calculations — storage, bandwidth, latency, and derived architectural implications.
- **Failure scenarios**: YouTube and Netflix reliability tables with RTO values are solid. Netflix's Chaos Engineering suite (Monkey through Kong) is well described.

### What is missing or shallow

- **WebRTC for ultra-low-latency delivery**: Twitch mentions WebRTC for < 1 second mode but does not explain how an SFU (Selective Forwarding Unit) works or why WebRTC cannot replace HLS for millions of concurrent viewers. This is a common follow-up question.
- **QUIC / HTTP/3 impact on video delivery**: CDN adoption of HTTP/3 meaningfully reduces connection setup latency and handles packet loss better than TCP (which underlying HLS uses). Not mentioned anywhere.
- **Edge computing / edge transcoding**: The question "could you do some transcoding at the CDN edge rather than at origin?" is not addressed. For live streaming, this is increasingly relevant.
- **Video quality vs. encoding cost trade-off for long-tail content**: The source files discuss AV1 re-encoding for top videos but don't quantify when it stops being worth it. The breakeven math (CDN bandwidth savings per video vs. GPU cost per encode) is a good Staff+ calculation to prepare.
- **Multi-CDN failover strategy**: Twitch mentions Akamai + Fastly but doesn't explain how traffic is split between them or how failover is triggered. The mechanics of multi-CDN routing (DNS-based, BGP anycast, application-layer health checks) are not covered.
- **Audio-only and podcast-style streaming**: Not relevant for these four problems but comes up as a follow-up ("how would your system change for audio-only podcasts?").

### Senior-level probes you should be ready for

1. "Your CDN vendor has an outage. Walk me through the user impact and your mitigation." — Requires understanding of CDN failover, multi-CDN routing, and which content is on-device buffer vs. needs network fetch.

2. "How do you ensure video quality is consistent when two encoders of the same segment produce slightly different bitstream outputs?" — About deterministic encoding (fixed seed for random parameters, fixed FFmpeg version pinned in container images) and why this matters for CDN cache keys.

3. "A creator claims their 4K video looks worse on Netflix than on YouTube for the same title. How would you diagnose this?" — Walk through VMAF scoring pipeline, codec selection, device capability negotiation, and CDN segment cache validation.

4. "How would you add support for real-time thumbnail generation that shows a current frame from a live Twitch stream?" — Not in the source files. Requires thinking about: extract one frame per N seconds from RTMP ingest, generate JPEG, upload to CDN with short TTL (10s), serve via CDN URL embedded in stream directory listing.

5. "Your recommendation model recommends a video that gets a creator temporarily banned for ToS violation. How do you remove it from all recommendation caches globally in < 1 minute?" — Requires cache invalidation design: Kafka `video-status-changed` topic, API servers subscribe and invalidate local caches, CDN purge API for manifest URLs, Redis `DEL reco:{user_id}` for all cached recommendation lists (requires a secondary index of which user caches contain this video_id — which is why recommendation caches are coarse-grained, not per-video).

### Common traps to avoid

- **Trap 1**: Proposing DynamoDB as the primary video metadata store. DynamoDB's query flexibility (no secondary indexes by default) and cost at YouTube's view-count-write QPS makes it a poor choice. MySQL/Vitess (for relational metadata) + Bigtable (for counters) + Cassandra (for time-series) is the correct polyglot answer.

- **Trap 2**: Forgetting that live and VOD require completely different transcoding models. Segment-parallel transcoding is wrong for live streaming (future segments don't exist). One-worker-per-stream is wrong for VOD (wasteful sequential processing). State this distinction explicitly.

- **Trap 3**: Using a single manifest TTL for all content. Live manifests: 2-second TTL (must show new segments promptly). VOD manifests: 5-10 minute TTL (stable content, high cache hit rate). Segment TTL: 1 year (immutable). Getting this wrong means either live streams are stale (missed segments) or CDN is hammered with unnecessary manifest refreshes for VOD content.

- **Trap 4**: Proposing Kafka for Twitch chat delivery. Kafka's partition-based consumers and write-to-disk architecture add too much latency for live chat (target < 100ms). Redis Pub/Sub is correct here because messages are ephemeral (missed messages for offline subscribers are acceptable), delivery is in-memory, and sub-millisecond fan-out is the requirement.

- **Trap 5**: Designing the recommendation system to do real-time ML inference per request. At 89,000 video start events per second, real-time inference is expensive and not necessary. Pre-compute recommendations for all active users in batch (nightly Spark job), cache in Redis/Cassandra, serve pre-computed results in < 5ms per request. Use real-time signals only for the current-session context (just-watched video, current search query) to do lightweight re-ranking of the pre-computed candidate set.

---

*End of Interview Guide — Pattern 4: Video Streaming*
