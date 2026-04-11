# Common Patterns — Video Streaming (04_video_streaming)

## Common Components

### CDN (Video Delivery at Edge)
- The dominant scalability tool in all four problems; segments cached at edge PoPs closest to the viewer
- In youtube: Google Global Cache (GGC) at thousands of ISP PoPs; 97%+ hit rate for popular content; segments cached 1 year (immutable), manifests 5 minutes
- In netflix: Open Connect Appliances (OCAs) — proprietary 1U/2U Linux servers with 16-36 TB NVMe SSD embedded inside ISPs; 95%+ of traffic served from OCA; nightly fill with top-N titles based on view statistics
- In twitch: Multi-CDN (Akamai + Fastly + custom PoPs); HLS segments 2-second TTL for live, 7 days for clips, 60 days for VODs
- In video_upload_pipeline: CDN for serving encrypted segments, manifests, and thumbnails after transcoding completes

### Object Storage (S3 / GCS / Equivalent)
- Used in all four for durable blob storage of all video segments, manifests, and thumbnails
- In youtube: Google Cloud Storage (GCS) for raw video and processed HLS/DASH segments
- In netflix: AWS S3 for media assets + AWS Glacier for master file archival (11-nine durability); cross-region replication to 2 secondary regions
- In twitch: Amazon S3 for live HLS segments (rolling 90s buffer, purged 2h after stream ends), VODs, and clips
- In video_upload_pipeline: S3 for raw chunks, assembled files, transcoded segments, encrypted output, thumbnails; S3 multipart upload API as underlying protocol

### HLS + DASH Adaptive Bitrate Streaming
- All four produce HLS and/or DASH manifests and serve fMP4 (or TS) segments
- Common encoding ladder: 144p–2160p (4K) across multiple bitrates per resolution
- 2–4 second segments for ABR adaptation speed
- Manifests served with short TTL (2–5 min); segments immutable with long TTL (days to 1 year)
- ABR algorithm in client: throughput-based with buffer health feedback; downgrade on buffer below minimum, upgrade after sustained headroom
- In youtube: CMAF (Common Media Application Format) allows one set of fMP4 segments referenced by both HLS and DASH manifests
- In twitch: additionally offers LHLS (Low-Latency HLS, 0.2s parts) and WebRTC for < 1s latency

### GPU-Accelerated Transcoding with FFmpeg
- All four use FFmpeg (or FFmpeg-based tools) and NVIDIA GPU hardware encoding (NVENC / h264_nvenc)
- In youtube: NVIDIA T4 GPUs running NVENC (H.264 at ~800 fps on a single T4)
- In netflix: EC2 encoding fleet (c5.18xlarge, 72 vCPU); per-scene analysis via FFmpeg signalstats; VMAF scoring
- In twitch: T4 GPU handles ~4 simultaneous streams at 6 quality levels via FFmpeg NVENC
- In video_upload_pipeline: AWS g4dn.xlarge (T4 GPU) running h264_nvenc; ffprobe for metadata extraction

### Segment-Parallel Transcoding Pipeline
- All four split video into segments (~2–30s), encode segments in parallel, then assemble manifests
- Enables linear horizontal scaling of transcoding workers and reduces total processing latency
- In youtube: 30-second GOP-aligned segments dispatched to Pub/Sub; workers assemble master manifest after all segments complete
- In netflix: 4-second DASH boundaries; per-scene complexity analysis before encoding; multiple codec/bitrate combinations per scene
- In twitch: real-time streaming → 2-second segments; no pre-split (live); one GPU worker per live stream
- In video_upload_pipeline: 30-second segments via FFmpeg split → parallel SQS jobs per quality tier → FFmpeg concat + Shaka Packager assembly

### Kafka (or Equivalent) Event Bus
- All four use Kafka (or Google Pub/Sub) for decoupling pipeline stages and analytics
- In youtube: Google Pub/Sub for transcoding jobs, video-published events, subscription notifications, analytics events; Dataflow for stream processing
- In netflix: Kafka for CDC pipeline from Cassandra to Elasticsearch; SQS for encoding job queue
- In twitch: Kafka for watch heartbeats, view events, chat events, drops, stream-starts, hype-chat events; Flink for aggregating heartbeats into 5-minute windows
- In video_upload_pipeline: Kafka `video-uploaded` topic (7-day retention, key = video_id) for event-driven pipeline; SQS for per-quality transcode jobs

### Redis
- Used in all four for hot-path ephemeral state: counters, caching, coordination
- In youtube: recommendation candidate cache (5-min TTL), trending sorted set per country (1-min TTL), view count aggregation (flush every 30s), session tokens, rate-limit counters
- In netflix: concurrent stream control (90-second TTL, checked against subscription tier max_streams)
- In twitch: directory sorted sets (`ZADD live:{game_id} {viewer_count} {channel_id}`), viewer count INCR/DECR, stream metadata cache (10s TTL), auth tokens, banned user sets, transcoding worker pool management
- In video_upload_pipeline: upload session state (24h TTL), chunk ETag bitmap, video status for polling (30s TTL), DRM key cache (1h TTL), license validation cache (5m TTL), job counter (DECR to zero triggers assembly)

### DRM (Digital Rights Management)
- Netflix and Video Upload Pipeline explicitly implement multi-DRM; YouTube implements content protection with signed URLs and Content ID; Twitch addresses DMCA for VOD audio
- Common DRM stack: Widevine (Google/Android/Chrome) + FairPlay (Apple iOS/macOS) + PlayReady (Microsoft Windows/Xbox)
- CENC (Common Encryption) for universal segment encryption with per-video content keys
- License server at playback time; license validity cached 24h–30 days depending on use case
- In netflix: Widevine + FairPlay + PlayReady; offline license bound to device TEE/TrustZone (30-day expiry)
- In video_upload_pipeline: Shaka Packager for CENC encryption; AWS KMS envelope encryption for key storage; PSSH boxes in manifests

### Pre-Signed URLs + Signed Manifests
- All four use time-limited signed URLs for upload sessions and/or for CDN access control
- In youtube: HMAC-SHA256 signed URLs for secure CDN access; upload session tokens valid 24h
- In netflix: presigned S3 URLs for offline downloads; OCA steering uses signed tokens
- In twitch: presigned upload URLs for VOD/clip storage
- In video_upload_pipeline: pre-signed S3 URLs for chunk uploads (24h expiry); HTTPS-signed manifest URLs for playback

### Content Moderation / Copyright Detection
- All four address copyright and/or harmful content detection during or after transcoding
- In youtube: Content ID fingerprinting (spectral audio + perceptual visual hash via Photon algorithm; LSH lookup); automated actions by rights holder (block/monetize/track)
- In netflix: DMCA for licensed content; regional availability table (title_availability) enforced at API layer
- In twitch: DMCA audio muting for VOD (post-stream); AutoMod for chat (BERT-based); CSAM detection implied
- In video_upload_pipeline: CSAM hash matching (PhotoDNA/NCMEC), ML violence/hate speech classifiers (Triton Inference Server), audio fingerprinting; moderation runs in parallel with transcoding

### Kubernetes for Transcoding Workers
- YouTube, Twitch, and Video Upload Pipeline all use Kubernetes to orchestrate GPU transcoding workers
- Workers auto-scaled by queue depth (Pub/Sub, SQS, Kafka consumer lag)
- Stateless workers: job claimed from queue, output written to S3, offset committed

## Common Databases

### MySQL (with Vitess for sharding)
- YouTube and Twitch both use MySQL sharded via Vitess for core relational entities
- In youtube: videos, users, subscriptions, comments tables sharded by video_id or user_id; 8 initial shards
- In twitch: users, streams, follows, subscriptions, VODs, clips sharded by user_id (16 shards initially)

### PostgreSQL
- Twitch (stream analytics dashboard) and Video Upload Pipeline (with Citus extension) use PostgreSQL
- In video_upload_pipeline: stores upload_sessions, videos, video_renditions, thumbnails, drm_keys, pipeline_events, moderation_results

### Elasticsearch
- YouTube and Netflix both use Elasticsearch for search
- Fed via CDC/Debezium (YouTube from MySQL, Netflix from Cassandra via Kafka)
- In youtube: 20 primary shards × 2 replicas; indexes title, description, tags, auto_captions; circuit breaker fallback to cached results

## Common Queues / Event Streams

### Kafka / Pub/Sub
- See Common Components — used for pipeline stage decoupling, analytics, and CDC

### SQS (Job Queue for Transcoding)
- Twitch and Video Upload Pipeline use Amazon SQS for individual transcode job queues
- Visibility timeout for worker failure detection; dead-letter queue after 3 retries
- In video_upload_pipeline: FIFO SQS with HIGH_PRIORITY and STANDARD queues per quality tier; deduplication ID = `{video_id}:{segment_index}:{quality_label}`

## Common Communication Patterns

### HLS / DASH ABR Protocols (see Common Components above)

### REST over HTTPS for API + CDN for Media
- All four separate the API plane (metadata, search, authentication via REST) from the media plane (segments via CDN edge)
- API servers are stateless and horizontally scaled; media delivery is entirely CDN-driven

### OAuth 2.0 Bearer JWT
- All four use OAuth 2.0 for authentication; JWT Bearer tokens for API requests

### Geo-Restriction Enforcement
- All four enforce regional licensing restrictions
- YouTube: `video_geo_restrictions` table + MaxMind DB; HTTP 451 for blocked content; CDN-level geo rules
- Netflix: `title_availability` table with country_code and date windows; OCA steering includes country check
- Twitch: implied for DMCA compliance by country
- Video Upload Pipeline: geo-restriction metadata stored per rendition; enforced at manifest generation

## Common Scalability Techniques

### Immutable Segment Caching at CDN Edge
- Segments are content-addressed and immutable once written; served with long `Cache-Control: max-age` (1 year for YouTube, days–months for others)
- CDN origin is only hit on cache miss; hit rate > 90% for popular content

### Segment-Parallel Transcoding (see Common Components above)

### Multi-Codec Encoding Ladder
- All four support multiple codecs: H.264 (widest compatibility) + at minimum one newer codec
- YouTube: H.264 + VP9 + AV1; Netflix: H.264 + HEVC + AV1; Twitch: H.264 (primary)
- AV1 provides ~30% better compression than VP9; HEVC provides ~40% better than H.264

### View Count / Viewer Count Eventual Consistency
- All four accept eventual consistency for view/viewer counts
- Pattern: buffer increments in Redis (INCR), flush to DB every 10–30 seconds
- In youtube: Bigtable counter sharding (64 shards per hot video); scatter-gather on reads
- In twitch: Redis INCR/DECR for concurrent viewer count; denormalized in `streams` table with 10s TTL

### Horizontal Auto-Scaling of Stateless Workers
- All API services and transcoding workers are stateless, auto-scaled by CPU/queue depth
- Transcoding GPU workers scale on queue depth; pre-scaled for known high-demand events

## Common Deep Dive Questions

### How do you transcode a 12-hour video quickly?
Answer: Split the video into ~30-second GOP-aligned segments using FFmpeg. Dispatch each segment as an independent job to a GPU worker pool (via Pub/Sub, SQS, or Kafka). Workers transcode in parallel (N segments × Q quality levels simultaneously). A coordinator (Redis counter, atomic DECR to 0) triggers manifest assembly and CDN push once all segments complete. A 2-hour movie can be transcoded in < 20 minutes with hundreds of parallel workers.
Present in: youtube, netflix (per-scene), twitch (real-time variant), video_upload_pipeline

### How do you implement adaptive bitrate streaming?
Answer: Encode at multiple quality levels (e.g., 144p to 4K) and store each as 2–4 second HLS/DASH segments in object storage. Generate a master manifest listing all quality variant playlists. Client downloads the master manifest, picks a quality based on current network bandwidth measurement + buffer health. Client switches quality per-segment with no rebuffering if using fMP4 (CMAF). CDN caches immutable segments with 1-year TTL.
Present in: youtube, netflix, twitch, video_upload_pipeline

### How do you handle the CDN cache miss when a popular video goes viral?
Answer: CDN request coalescing/collapsed forwarding: the CDN edge holds all requests for an uncached segment and issues a single origin request, then serves the segment to all waiting clients simultaneously. For predictable viral content, pre-warm CDN by proactively pushing popular segments to edge nodes before demand spikes.
Present in: youtube (request coalescing), netflix (nightly OCA fill), twitch (Akamai/Fastly), video_upload_pipeline

### How do you make video uploads resumable?
Answer: Client initiates an upload session (gets session_id and upload URL). Client uploads chunks with Content-Range headers. Server tracks received chunks in Redis and records each chunk's S3 ETag. On failure, client sends HEAD to get offset of last received chunk and resumes from there. S3 multipart upload handles parallel and sequential chunk assembly. Session valid for 24 hours.
Present in: youtube (GCS resumable protocol), video_upload_pipeline (TUS 1.0), twitch (implied for VOD), netflix (offline downloads)

### How do you handle hot videos without overwhelming the origin?
Answer: CDN with 97%+ cache hit rate handles the vast majority. For segments not yet cached: CDN collapsed forwarding coalesces simultaneous cache misses into a single origin request. Redis caches video metadata (5-min TTL) so the metadata API also avoids DB on cache hits. View counts aggregated in Redis and flushed every 30s to avoid per-view DB writes.
Present in: youtube, netflix, twitch, video_upload_pipeline

## Common NFRs

- **Video start time (TTFF)**: p99 < 2 seconds on broadband across all four problems
- **Availability**: 99.9–99.99% for playback; pipeline (transcoding) can tolerate brief outages with queued retry
- **Durability**: Video data never lost once acknowledged; 11-nine equivalent S3 durability for final segments
- **Adaptive quality**: Must degrade gracefully on poor network; no rebuffering allowed > threshold
- **Eventually consistent engagement metrics**: view counts, likes, ratings all eventually consistent; accuracy within 30–60 seconds acceptable
- **Scale**: CDN-first architecture so origin servers never see per-viewer load; origin load is O(cache misses), not O(viewers)
