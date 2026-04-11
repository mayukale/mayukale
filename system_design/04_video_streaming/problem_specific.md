# Problem-Specific Design — Video Streaming (04_video_streaming)

## YouTube

### Unique Functional Requirements
- Supports video uploads up to 12 hours / 256 GB raw source
- Search across title, description, tags, and auto-generated transcripts (captions in 125+ languages)
- Recommendation feed on homepage and "Up Next" sidebar
- Creator analytics dashboard with ad revenue reporting
- Playlists, Watch Later, and cross-device "Continue Watching"
- Trending page: velocity-based trending algorithm per country
- Ad insertion: Client-Side (CSAI via IMA SDK) and Server-Side (SSAI) pre-roll and mid-roll ads
- Subscription notifications to all subscribers when a channel uploads

### Unique Components / Services
- **Google Bigtable**: view counts (row key = `video_id#YYYY-MM-DD`, 64 counter shards), watch history, and comment storage (row key = `video_id#reverse_timestamp`)
- **BigQuery**: offline analytics, ad reporting, ML feature engineering; partitioned by `(video_id, date)`
- **Google Dataflow**: stream processing for windowed view aggregations (1-min, 15-min, 1-hr, 1-day windows); feeds BigQuery and Redis trending sorted sets
- **Google Pub/Sub**: transcoding job queue, video-published events, subscription notification fan-out (batch of 10,000 subscribers at a time)
- **ScaNN (Scalable Nearest Neighbors)**: ANN index served in-memory on recommendation fleet; 800M video embeddings, top-500 candidates in < 10ms
- **Google Cloud Speech-to-Text API**: auto-caption generation for 125+ languages post-transcoding
- **Cloud Translation API**: caption translation to additional languages
- **Maglev (L4) + GFE (L7)**: Google's custom load balancers with Anycast IP + GeoDNS routing
- **VTOrc**: MySQL failover orchestration (heartbeat-based detection, auto-promotion)
- **Content ID (Photon algorithm)**: spectral audio + perceptual visual hash for copyright fingerprinting; LSH lookup against rights-holder database; automated actions (block/monetize/track) configured per rights holder

### Unique Data Models
- **Bigtable view counts**: row key = `{video_id}#shard#{rand(0,63)}` (64 shards for hot-video writes); scatter-gather read then SUM
- **Bigtable comments**: row key = `{video_id}#reverse_timestamp`; wide-column for threaded replies via parent_id
- **videos** (MySQL): `codec_version` field enables background re-encoding; `chapters JSONB` for timestamp chapters parsed from description; `thumbnail_url`, `status`, `visibility`
- **video_renditions** (MySQL): resolution, bitrate, codec, container, manifest_key per quality level
- **Watch events** (BigQuery / Bigtable): user_id, video_id, watched_at, watch_duration_sec, completion_pct, device_type — partitioned monthly
- **Trending sorted set** (Redis): `ZADD trending:{country} {score} {video_id}` — velocity = view rate growth over 24h vs. channel baseline, weighted by geographic spread and engagement rate; refreshed every minute by Dataflow job
- **Search index** (Elasticsearch): 20 primary shards × 2 replicas; fields: title (analyzed), description, tags, hashtags (keyword), auto_captions (analyzed), like_count, view_count; Debezium CDC from MySQL

### Unique Scalability / Design Decisions
- **Google-proprietary infrastructure**: GCS, Bigtable, BigQuery, Pub/Sub, Dataflow, Maglev — all Google internal services tightly integrated; no vendor-neutral equivalents used
- **Counter sharding in Bigtable**: 64 shards per hot video_id prevent Bigtable hotspot on single row; writes distributed, reads scatter-gather then sum
- **CMAF for single-encode multi-protocol**: one set of fMP4 segments referenced by both HLS master.m3u8 and DASH MPD; avoids encoding the same quality twice
- **AV1 codec for 30% bandwidth saving**: encodes older high-view videos during off-peak; incremental re-encode threshold: > 1M views
- **Two-stage recommendation (Two-Tower + Wide & Deep)**: Two-Tower generates 500 candidates via ANN in < 10ms; Wide & Deep ranks them using 30-day watch history and real-time signals; ANN index rebuilt every 6 hours
- **Geo-restriction at Stream API Gateway**: MaxMind DB lookup on IP → country code → check `video_geo_restrictions` table; return HTTP 451 for blocked content; CDN geo rules as defense-in-depth
- **Subscription notification fan-out**: async batch via Pub/Sub; 10,000 subscribers per batch; rate-limited per video_id; delivered via FCM (mobile), WebSocket/SSE (browser), email (max 1/day per channel)

### Key Differentiator
YouTube's defining architectural challenge is **scale of search and recommendations across 800M+ videos**: the two-tower recommendation model, ScaNN ANN index, auto-caption indexing in Elasticsearch (requiring Speech-to-Text pipeline per video), and Bigtable counter sharding for billions of daily view events — combined with the Content ID copyright fingerprinting system — make it the most complex analytics-and-ML-heavy problem in this folder.

---

## Netflix

### Unique Functional Requirements
- Catalog browsing with metadata (title, genre, actors, ratings) — no user-uploaded content
- Multiple user profiles per account (up to 5 profiles per subscription)
- Concurrent stream limit enforced per subscription tier (Basic: 1, Standard: 2, Premium: 4)
- Offline downloads: encrypted segments downloaded to device, licenses bound to device TEE/TrustZone (30-day expiry)
- Regional content licensing: 190+ countries with per-title date-windowed availability
- Personalized artwork selection per user using multi-armed bandit (Thompson Sampling on click + play conversion)
- Netflix Originals distributed globally simultaneously (no staggered rollout)

### Unique Components / Services
- **Open Connect Appliances (OCAs)**: Netflix's proprietary CDN; 1U/2U Linux servers with 16-36 TB NVMe SSD + 2×100GbE NICs embedded inside ISPs worldwide; nginx-based HTTP server with custom modules; 1,000+ locations; filled nightly via BGP anycast over dedicated 10/100G circuits; sibling OCA support and hierarchical fallback
- **EVCache**: Netflix's distributed Memcached-based caching layer; reduces DB load by ~99% for catalog reads; stale-while-revalidate pattern; probabilistic early expiry (±20% TTL jitter)
- **Hystrix**: Netflix's open-source fault tolerance library (circuit breaker, thread pool isolation, timeouts, fallbacks); now in maintenance (succeeded by Resilience4j)
- **Titus**: Netflix's internal container management platform (on top of AWS EC2)
- **Netflix Atlas**: in-house time-series monitoring (1.3 billion metrics/minute)
- **Chaos Engineering Suite**: Chaos Monkey (kills random EC2 instances), Latency Monkey (injects artificial latency), Conformity Monkey (compliance checks), Chaos Kong (simulates full region failure), ChAP (Chaos Automation Platform)
- **Zuul**: Netflix's open-source API Gateway with dynamic routing
- **Zipkin**: distributed tracing with X-B3-TraceId headers
- **Per-scene dynamic bitrate encoding**: FFmpeg signalstats computes spatial info (SI) and temporal info (TI) per scene; convex hull optimization finds VMAF/bitrate sweet spot; target VMAF=93 for 1080p; saves ~30% bandwidth vs. fixed ladder

### Unique Data Models
- **active_streams** (Redis, 90s TTL): stream_id, account_id, profile_id, title_id, device_fingerprint, last_heartbeat_at, expires_at — checked against subscription tier `max_streams`; heartbeat every 60s
- **title_availability** (Cassandra/MySQL): (title_id, country_code) → available_from, available_until — O(1) lookup at playback start
- **watch_events** (Cassandra): partition key = profile_id, clustering key = title_id; resume_position_sec updated every 60s heartbeat
- **renditions** (Cassandra): resolution, codec (h264/hevc/av1/vp9), bitrate_kbps, hdr_format (Dolby Vision / HDR10), dash_manifest_key
- **A/B experiment assignment**: `hash(profile_id + experiment_id) mod 100` → variant bucket; stored in EVCache (< 1ms lookup); config in Zookeeper with < 5s propagation
- **OCA fill metadata**: nightly analysis of view stats → top-N titles per OCA cluster; pushed over BGP anycast

### Unique Scalability / Design Decisions
- **Open Connect instead of commercial CDN**: Netflix operates its own CDN embedded in ISPs, reducing external transit cost and lowering latency; top 10% of titles (1,700) account for 80% of streams; OCA selection based on geo + ISP + RTT probes + health checks + title cached check
- **Per-scene encoding (not per-title or fixed ladder)**: each scene analyzed for complexity; easy scenes (animation) encoded at lower bitrates than action scenes at the same target quality (VMAF 93); saves ~30% bandwidth without quality loss
- **Multi-DRM offline downloads**: Widevine/FairPlay/PlayReady licenses bound to device hardware (TEE/TrustZone); 30-day offline expiry; license renewal requires connectivity
- **1,000+ concurrent A/B experiments**: hash-based deterministic assignment cached in EVCache; orthogonal experiment layers (UI, algorithm, artwork); CUPED variance reduction; 2-sample t-test with Benjamini-Hochberg correction; automated guardrail rollback via PagerDuty
- **Chaos Monkey / Chaos Kong**: production traffic continuously killed and region failures simulated to ensure resilience; not a separate testing environment — runs against live production systems
- **EVCache stale-while-revalidate**: serve stale data immediately, refresh asynchronously; probabilistic early expiry prevents cache stampede (±20% TTL jitter)
- **Last-Write-Wins (LWW) conflict resolution**: Cassandra multi-DC active-active uses client-provided timestamps for LWW; acceptable for social data (watch history, ratings)
- **EC2 Spot Instances for encoding farm**: 70% cheaper; encoding jobs are interruptible and re-queueable via SQS dead-letter queue

### Key Differentiator
Netflix's defining architectural choices are the **Open Connect proprietary CDN** (embedded inside ISPs, filled nightly, serving 95%+ of global traffic without external CDN vendors) and **per-scene bitrate optimization** (VMAF-targeted encoding that treats each scene individually rather than applying a fixed bitrate ladder) — combined with the most mature chaos engineering and A/B experimentation infrastructure in the industry.

---

## Twitch

### Unique Functional Requirements
- Live streaming as the primary use case: broadcaster sends RTMP from OBS/encoder to ingest servers; viewers watch live with < 5–8s latency
- Live chat: IRC-based protocol over WebSocket; channels with 100,000+ simultaneous chatters
- Stream directory: real-time sorted list of live streams by viewer count per game/category
- Clips: 30-second highlights extracted from a live stream's 90-second rolling buffer in S3
- VOD recording: live stream segments assembled into permanent VOD after stream ends
- Channel subscriptions (tiers: $4.99/$9.99/$24.99), Bits virtual currency for cheers
- Channel Points: watch-time-based loyalty points; predictions and polls
- Drops: reward system for watching games (watch-time accumulation tracked per campaign)
- Low-Latency HLS (LHLS): 0.2-second parts for < 3–4s latency; WebRTC for < 1s ultra-low latency

### Unique Components / Services
- **Ingest Edge Servers** (~150 global PoPs): receive RTMP streams (TCP port 1935); authenticate stream keys; buffer and relay to transcoding cluster; Anycast routing for nearest PoP
- **Transcoding Workers**: one GPU worker per live stream (fault isolation); T4 GPU handles ~4 simultaneous streams at 6 quality levels (1080p60, 720p60, 720p30, 480p, 360p, 160p); Redis LPUSH/RPOPLPUSH for atomic worker pool management
- **Manifest Service**: maintains rolling HLS playlist (last N segments); atomic S3 PUT on each update; viewers poll manifest every 2 seconds
- **Chat Ingress Servers (IRCd-based)**: parse IRC protocol (PRIVMSG, PASS oauth, NICK, JOIN); apply rate limits; publish to Redis Pub/Sub per channel
- **Chat Distribution Servers**: subscribe to Redis Pub/Sub per channel; fan-out to viewer WebSocket connections; IP hash for session affinity
- **Directory Service**: Redis ZSET `live:{game_id}` scored by viewer_count; `ZADD` on viewer joins/leaves; sub-millisecond reads for directory pages
- **Clip Service**: receives clip request → selects S3 segments from manifest → SQS job → FFmpeg concat + trim + multi-quality encode → permanent S3 key + CDN URL
- **AutoMod**: BERT-based ML classifier for harassment, hate speech, sexual content, spam; runs on chat messages
- **Flink**: streaming job for aggregating watch heartbeats into 5-minute windows for Channel Points accumulation
- **ClickHouse**: high-throughput columnar storage for view events and chat events (real-time analytics)

### Unique Data Models
- **streams** (MySQL): stream_id, user_id, stream_key_hash, title, game_id, language, tags, started_at, ended_at, status (live/ended/error), peak_viewer_count, total_view_minutes, ingest_server, ingest_bitrate_kbps
- **chat_messages** (Cassandra): partition key = (channel_id, date); clustering key = sent_at DESC; body, message_type (chat/cheer/sub/resub/raid), bits_used, color, badges (array); TWCS compaction
- **vods** (MySQL): vod_id, stream_id, manifest_key (S3), muted_segments (JSONB for DMCA audio muting), expires_at (60 days for all; unlimited for Partners)
- **clips** (MySQL): clip_id (VARCHAR 20 slug), broadcaster_id, creator_id, vod_offset_sec, duration_sec (5–60)
- **channel_points_ledger** (event sourcing): (txn_id UUID, viewer_id, broadcaster_id, delta INT, reason, created_at) — immutable append-only
- **Redis structures**: `live:{stream_id}` (Hash: title, game, thumbnail); `channel_modes:{channel_id}` (Hash: sub_only, slow_mode, emote_only); `banned:{channel_id}` (Set of user_ids); `available_workers` (Redis List for atomic RPOPLPUSH pool)
- **drops_progress** (Redis): `drop_progress:{viewer_id}:{campaign_id}` for real-time watch-time accumulation
- **Live segment buffer**: rolling 90-second window of HLS segments in S3 with lifecycle rule purging segments older than 2 hours

### Unique Scalability / Design Decisions
- **One GPU worker per live stream**: fault isolation — a crash affects only one channel; no multiplexing; Redis RPOPLPUSH for atomic "claim a worker" without double-assignment
- **LHLS (Low-Latency HLS)**: segments broken into 0.2-second parts with EXT-X-PART tags; HTTP/2 multiplexing for efficient manifest + part requests; reduces latency from 8s to 3–4s; WebRTC SFU available for < 1s for special event modes
- **Redis Pub/Sub for chat fan-out** (not Kafka): sub-millisecond delivery; messages not persisted — if subscriber is offline, messages are lost (acceptable for live chat); fan-out math: 8.3M WebSocket operations/sec for a 100K-viewer channel requires Redis Pub/Sub + dedicated distribution server pools
- **Channel sharding for chat**: consistent hash of channel_id → dedicated server cluster; high-traffic channels (e.g., 1M viewers) get dedicated pools
- **Stream directory Redis ZSET**: `ZADD live:{game_id} {viewer_count} {channel_id}` updated on every viewer join/leave; ZREVRANGE for paginated directory; sub-millisecond reads; directory is eventually consistent (viewer count updated every ~10s)
- **Clip 90-second S3 buffer**: live segments stored to S3 continuously; clip request selects the most recent 30 seconds from manifest; async SQS job trims + re-encodes; user polls for completion status
- **DMCA audio muting for VODs**: audio fingerprinting runs post-stream on VOD; muted_segments JSONB field marks time ranges; HLS manifest generation skips or replaces audio tracks in muted segments; live audio not muted (processing cost too high)

### Key Differentiator
Twitch is the only problem in this folder where the video is generated in real time (RTMP ingest → live transcoding → 2-second segment delivery) rather than from a pre-uploaded file, making the ingest edge server fleet, one-worker-per-stream transcoding model, IRC-based live chat fan-out (Redis Pub/Sub rather than Kafka), and LHLS latency optimization the defining architectural challenges.

---

## Video Upload Pipeline

### Unique Functional Requirements
- Resumable multi-part uploads: files up to 50 GB / 12 hours; resume from last byte on failure
- Chunks (5 MB each) uploaded in parallel or sequentially; checksum verified per chunk (SHA-256)
- Full DRM pipeline: CENC encryption + PSSH box generation + license server at playback time
- Moderation: CSAM hash matching (PhotoDNA/NCMEC — P0 alert on detection), ML violence/hate speech classifiers (confidence scores stored)
- Pipeline status tracked per video (uploading → assembling → transcoding → published/failed) with real-time SSE/WebSocket updates to creator
- Thumbnail candidates auto-generated at 10%/25%/50% of video duration; quality-filtered (blurriness, darkness, motion artifacts); creator can upload custom thumbnail
- Sprite sheets (VTT + JPEG contact sheet) for video scrubber hover preview

### Unique Components / Services
- **TUS 1.0 Protocol**: open standard for resumable uploads; `HEAD` to get upload offset on resume; `PATCH` for chunk upload; compatible with S3 multipart upload API as backend
- **PostgreSQL with Citus extension**: distributed PostgreSQL for videos, upload_sessions, renditions, thumbnails, drm_keys, pipeline_events, moderation_results; range-partitioned by video_id hash across 10 nodes
- **Shaka Packager**: Google's open-source DASH/HLS packager used for CENC segment encryption; generates PSSH boxes for Widevine/FairPlay/PlayReady
- **AWS KMS (envelope encryption)**: KMS master key encrypts per-video content key; encrypted key stored in DB; KMS not called at playback time (only at key generation and rotation)
- **2D Parallel Transcoding Coordinator**: Redis counter per video_id tracks pending segment-quality combinations; `DECR` to zero atomically triggers assembly step; avoids polling
- **DRM/License Server**: stateless K8s pods (50 pods × 500 RPS/pod = 25,000 license requests/sec capacity); validates user entitlement, decrypts content key from DB, issues license; entitlement cached in Redis (5-min TTL)
- **Assembly Worker**: FFmpeg concat of all segment outputs per quality → Shaka Packager for DRM → S3 write with `If-None-Match: *` (conditional PUT for idempotency)
- **NVIDIA Triton Inference Server** (implied): serves ML moderation models (violence/hate speech classifiers)

### Unique Data Models
- **upload_sessions**: session_id (UUID), video_id, user_id, total_size_bytes, received_bytes, chunk_size_bytes, total_chunks, received_chunks (bitmap in Redis), raw_storage_prefix, status (in_progress/assembling/complete/failed/expired), checksum_sha256, expires_at
- **upload_chunks**: session_id, chunk_number, size_bytes, checksum_sha256, s3_etag, status (received/verified/failed), received_at
- **videos**: full lifecycle status field (uploading/assembling/extracting_metadata/moderating/transcoding/post_processing/published/failed_moderation/failed_transcode/private/deleted); moderation_status, moderation_flags (JSONB), drm_key_id
- **video_renditions**: rendition_id, video_id, quality_label, width, height, bitrate_kbps, codec, hdr_format (hdr10/dolby_vision), container (fmp4), hls_manifest_key, dash_manifest_key, is_drm_encrypted, status (pending/processing/ready/failed), started_at, completed_at
- **drm_keys**: key_id, video_id, key_system (widevine/fairplay/playready/cenc), key_id_hex, encrypted_key (KMS envelope), pssh_data, created_at, rotated_at
- **pipeline_events** (audit): event_id, video_id, stage, status (started/completed/failed), worker_id, detail (JSONB), duration_ms — append-only audit trail
- **moderation_results**: result_id, video_id, check_type (csam_hash/audio_fingerprint/ml_violence/ml_hate_speech), verdict (pass/fail/review/error), confidence (0.0–1.0), checker_version
- **thumbnails**: thumbnail_id, video_id, s3_key, cdn_url, width (1280), height (720), timestamp_sec, is_auto_generated, is_selected

### Unique Scalability / Design Decisions
- **TUS 1.0 + S3 Multipart backend**: TUS for client-facing resumable protocol (open standard with ecosystem support); S3 multipart upload as backend (S3 handles durability and assembly); Upload API tracks per-chunk ETags in Redis for S3 `CompleteMultipartUpload` call
- **2D parallelism (N segments × Q qualities)**: e.g., 30-minute video = 60 segments × 7 qualities = 420 parallel transcode jobs; reduces wall-clock time from hours to < 20 minutes; Redis DECR coordinator avoids polling and distributed locking for assembly trigger
- **CENC + multi-DRM (Widevine + FairPlay + PlayReady)**: CENC encrypts once with a common key; each DRM system wraps the key differently (PSSH boxes in manifests); avoids encoding separate encrypted streams per DRM vendor
- **KMS envelope encryption for DRM keys**: master KMS key encrypts per-video content key; encrypted key stored in DB; KMS called only at key generation time, not at license serving time; key rotatable without re-encrypting video segments
- **S3 conditional PUT (`If-None-Match: *`)**: makes transcoding output writes idempotent; if a worker retries and the segment already exists in S3, the write fails silently — the segment is already correct
- **Moderation parallel to transcoding**: content moderation pipeline starts from the raw uploaded file in parallel with transcoding; if moderation fails, in-progress transcode jobs are cancelled — saves GPU cost on violating content; moderation verdict typically available in < 5 minutes
- **Sprite sheet VTT + JPEG contact sheet**: ffprobe extracts video duration; frames sampled at 1-per-10s; assembled into a sprite JPEG + VTT timing file; client scrubber requests the sprite sheet URL and renders hover previews from CSS background-position

### Key Differentiator
The Video Upload Pipeline is the only problem in this folder that treats the **upload and processing infrastructure itself as the product** (not the playback): it focuses entirely on resumable chunked uploads (TUS 1.0), 2D parallel transcoding coordination (Redis DECR), DRM key lifecycle management (CENC + KMS envelope encryption), and pipeline audit trails — none of which appear as deep dives in the other three problems.
