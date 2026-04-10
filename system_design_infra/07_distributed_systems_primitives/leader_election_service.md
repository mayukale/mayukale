# System Design: Leader Election Service

> **Relevance to role:** Leader election is critical at every level of cloud infrastructure. The Kubernetes controller-manager and scheduler use lease-based leader election to run exactly one active instance. Kafka uses ZooKeeper-based election for partition leaders. OpenStack's DLM (Distributed Lock Manager) elects a leader for each resource group. The bare-metal job scheduler runs in active-passive HA mode with leader election. etcd and Consul themselves use Raft leader election internally. Understanding the nuances -- split-brain prevention, fencing, graceful failover, leader health monitoring -- is essential for building reliable infrastructure.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement | Detail |
|---|------------|--------|
| FR-1 | Elect a single leader | From N candidates, exactly one becomes leader at any time |
| FR-2 | Leader lease with TTL | Leadership has a time-limited lease; must be renewed |
| FR-3 | Automatic failover | If leader fails, a new leader is elected within bounded time |
| FR-4 | Leader identity discovery | All participants (and clients) can discover who the current leader is |
| FR-5 | Voluntary step-down | Leader can abdicate (e.g., for rolling upgrades) |
| FR-6 | Fencing | Old leader's in-flight operations are rejected after a new leader is elected |
| FR-7 | Health-based step-down | Leader monitors its own health; steps down if degraded |
| FR-8 | Campaign API | Candidate announces candidacy; blocks until elected or timeout |
| FR-9 | Observer API | Non-candidates watch the election and learn the current leader |

### Non-Functional Requirements
| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | Failover time | < 10 seconds (from leader death to new leader operational) |
| NFR-2 | Split-brain prevention | NEVER two leaders simultaneously (linearizable safety) |
| NFR-3 | Availability | 99.99% -- leadership always held (brief gap during failover OK) |
| NFR-4 | Leader discovery latency | < 100 ms for clients to learn the current leader |
| NFR-5 | Lease renewal latency | < 5 ms p99 |
| NFR-6 | Scalability | Support 1,000+ election groups (different resources with independent leaders) |

### Constraints & Assumptions
- Built on top of a consensus store (etcd or ZooKeeper).
- Candidates are Java/Python services running in k8s or bare-metal.
- Each election group has 2-5 candidates (typical HA deployment).
- Leader performs control-plane operations (scheduling, provisioning); not data-plane.
- Network partitions are expected; safety (no split-brain) over liveness.

### Out of Scope
- Multi-leader (active-active) architectures.
- Consensus protocol implementation (we use etcd/ZK, not build our own Raft).
- Data replication (handled by the application or separate system).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Value | Calculation |
|--------|-------|-------------|
| Election groups | 500 | Scheduler, controllers, per-service leaders |
| Candidates per group | 3 | Typical HA deployment |
| Total candidates | 1,500 | 500 x 3 |
| Lease renewals per second | 167 | 500 groups / 3s renewal interval |
| Leader discoveries per second | 5,000 | Clients querying current leader |
| Elections per day | 50 | Failures, upgrades, deployments |
| Watch subscriptions | 5,000 | Clients watching leader changes |

### Latency Requirements

| Operation | p50 | p99 | Notes |
|-----------|-----|-----|-------|
| Campaign (become leader, uncontested) | 5 ms | 20 ms | etcd txn + lease grant |
| Campaign (contested, wait for current leader to fail) | - | Leader TTL + election time | Depends on TTL (10-15s) |
| Lease renewal | 1 ms | 5 ms | Single etcd operation |
| Leader discovery | 0.5 ms | 2 ms | etcd read (cached) |
| Failover (leader death to new leader) | 5 s | 15 s | TTL expiry + election |
| Voluntary step-down | 10 ms | 50 ms | Delete lease + observer notification |

### Storage Estimates

| Item | Size | Total |
|------|------|-------|
| Election record (per group) | 512 B | 256 KB for 500 groups |
| Lease records | 128 B | 192 KB for 1,500 leases |
| Audit history (30 days) | 128 B per event x 50/day x 30 | 192 KB |
| **Total** | | **< 1 MB** |

### Bandwidth Estimates

| Flow | Calculation | Bandwidth |
|------|-------------|-----------|
| Lease renewals | 167/sec x 128 B | 21 KB/s |
| Leader discoveries | 5,000/sec x 128 B | 640 KB/s |
| Watch notifications | 50 elections/day x 5,000 watchers x 128 B | negligible |

---

## 3. High-Level Architecture

```
  ┌────────────────────────────────────────────────────────────┐
  │                    Election Participants                     │
  │                                                             │
  │  ┌────────────┐  ┌────────────┐  ┌────────────┐           │
  │  │ Candidate A │  │ Candidate B │  │ Candidate C │          │
  │  │ (Leader)    │  │ (Standby)   │  │ (Standby)   │          │
  │  │             │  │             │  │             │          │
  │  │ ┌────────┐  │  │ ┌────────┐  │  │ ┌────────┐  │          │
  │  │ │Election│  │  │ │Election│  │  │ │Election│  │          │
  │  │ │Client  │  │  │ │Client  │  │  │ │Client  │  │          │
  │  │ │SDK     │  │  │ │SDK     │  │  │ │SDK     │  │          │
  │  │ └────────┘  │  │ └────────┘  │  │ └────────┘  │          │
  │  └──────┬─────┘  └──────┬─────┘  └──────┬─────┘          │
  └─────────┼───────────────┼───────────────┼─────────────────┘
            │               │               │
            │  Campaign /   │  Observe      │  Observe
            │  Renew Lease  │               │
            ▼               ▼               ▼
  ┌────────────────────────────────────────────────────────────┐
  │                  Leader Election Service                     │
  │                                                             │
  │  ┌────────────────────────────────────────────────────┐    │
  │  │                   API Layer                         │    │
  │  │  Campaign / Resign / Discover / Watch               │    │
  │  └───────────────────────┬────────────────────────────┘    │
  │                          │                                  │
  │  ┌───────────────────────▼────────────────────────────┐    │
  │  │              Election Engine                        │    │
  │  │  ┌──────────┐  ┌──────────┐  ┌──────────────────┐ │    │
  │  │  │Lease     │  │Fencing   │  │Health Monitor    │ │    │
  │  │  │Manager   │  │Token Gen │  │(leader self-check│ │    │
  │  │  │          │  │          │  │ + observer check) │ │    │
  │  │  └──────────┘  └──────────┘  └──────────────────┘ │    │
  │  └───────────────────────┬────────────────────────────┘    │
  │                          │                                  │
  │  ┌───────────────────────▼────────────────────────────┐    │
  │  │        Consensus Store (etcd / ZooKeeper)           │    │
  │  │                                                     │    │
  │  │   Node 1 (Leader)  ◄──►  Node 2  ◄──►  Node 3     │    │
  │  │   AZ-1                   AZ-2          AZ-3        │    │
  │  └─────────────────────────────────────────────────────┘    │
  └────────────────────────────────────────────────────────────┘
            │
  ┌─────────▼─────────────────────────────────────────────────┐
  │                    Observer Clients                         │
  │  (Services that need to know the current leader)           │
  │  ┌──────────┐  ┌──────────┐  ┌──────────┐                │
  │  │Worker 1  │  │Worker 2  │  │Worker N  │                │
  │  │"Who is   │  │"Forward  │  │"Route to │                │
  │  │ leader?" │  │ to leader│  │ leader"  │                │
  │  └──────────┘  └──────────┘  └──────────┘                │
  └────────────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **Election Client SDK** | Library embedded in candidates; handles campaign, lease renewal, resign, observe |
| **API Layer** | gRPC/HTTP interface for campaign, discover, watch operations |
| **Lease Manager** | Creates and renews leases in etcd; detects lease expiry |
| **Fencing Token Generator** | Issues monotonically increasing tokens on each leadership change |
| **Health Monitor** | Leader self-checks (can I still do my job?); observers verify leader responsiveness |
| **Consensus Store** | etcd or ZooKeeper cluster; provides linearizable writes and watch API |
| **Observer Clients** | Non-candidates that discover the current leader for routing |

### Data Flows

**Campaign (become leader):**
1. Candidate calls `Campaign(election_name, candidate_id, lease_ttl)`.
2. SDK creates an etcd lease with TTL.
3. SDK attempts to create a key `/elections/{name}/leader` with value `candidate_id`, using a Create-If-Not-Exists transaction.
4. If key doesn't exist: create succeeds -> candidate is leader. Start lease renewal loop.
5. If key exists: another leader active. SDK watches the key; when it's deleted (leader lease expires), retry step 3.
6. On successful election, fencing token is incremented and returned.

**Lease Renewal (leader heartbeat):**
1. Leader SDK sends `LeaseKeepAlive(lease_id)` to etcd every TTL/3 seconds.
2. etcd extends the lease.
3. If keep-alive fails (network partition), leader has TTL seconds to reconnect.
4. If TTL expires, etcd deletes the key. All watchers are notified.

**Failover:**
1. Leader crashes or lease expires.
2. etcd deletes `/elections/{name}/leader` (lease-attached key).
3. Standby candidates watching the key are notified.
4. All standbys simultaneously attempt to create the key (compare-and-swap).
5. Exactly one succeeds (etcd transaction guarantees).
6. New leader starts operating; fencing token incremented.

---

## 4. Data Model

### Core Entities & Schema

```
┌─────────────────────────────────────────────────────────┐
│ ElectionGroup                                            │
├─────────────────────────────────────────────────────────┤
│ election_name    STRING      PK  -- e.g. "scheduler"     │
│ namespace        STRING          -- isolation domain      │
│ leader_key       STRING          -- etcd key path         │
│ lease_ttl        INT             -- seconds (default 15)  │
│ fencing_token    BIGINT          -- monotonic counter     │
│ created_at       TIMESTAMP                                │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ LeaderRecord (stored in etcd as a single key)            │
├─────────────────────────────────────────────────────────┤
│ Key:   /elections/{namespace}/{name}/leader               │
│ Value: JSON {                                            │
│          "candidate_id": "scheduler-host-1",             │
│          "fencing_token": 47,                            │
│          "elected_at": "2026-04-09T10:15:00Z",           │
│          "lease_id": 7587852391238,                      │
│          "metadata": {                                   │
│            "hostname": "host-1.dc1",                     │
│            "address": "10.0.1.5:8080",                   │
│            "version": "v2.3.1"                           │
│          }                                               │
│        }                                                 │
│ Lease: {id: 7587852391238, ttl: 15}                      │
└─────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────┐
│ CandidateRecord (stored in etcd)                         │
├─────────────────────────────────────────────────────────┤
│ Key:   /elections/{namespace}/{name}/candidates/          │
│        {candidate_id}                                    │
│ Value: JSON {                                            │
│          "candidate_id": "scheduler-host-2",             │
│          "joined_at": "2026-04-09T09:00:00Z",            │
│          "state": "STANDBY",  // STANDBY | LEADER |      │
│                                  RESIGNED                │
│          "address": "10.0.2.3:8080",                     │
│          "priority": 100                                 │
│        }                                                 │
│ Lease: {id: 7587852391240, ttl: 30}                      │
└─────────────────────────────────────────────────────────┘
```

### Database Selection

| Store | Use | Rationale |
|-------|-----|-----------|
| **etcd** | Leader record + lease management | Linearizable; lease with TTL; Watch API; k8s ecosystem |
| **ZooKeeper** | Alternative to etcd | Ephemeral sequential znodes; mature ecosystem (Kafka) |
| In-memory (on election participants) | Local state cache | Cache leader identity for fast reads |

### Indexing Strategy

- **Election lookup:** `/elections/{namespace}/{name}/leader` -- direct key access O(1).
- **Candidate list:** `/elections/{namespace}/{name}/candidates/` -- prefix range scan.
- **All elections:** `/elections/` -- prefix scan for admin tooling.

---

## 5. API Design

### gRPC Service

```protobuf
service LeaderElectionService {
  // Campaign to become leader (blocks until elected or timeout)
  rpc Campaign(CampaignRequest) returns (CampaignResponse);

  // Renew leadership lease
  rpc RenewLease(stream RenewLeaseRequest)
      returns (stream RenewLeaseResponse);

  // Voluntarily resign leadership
  rpc Resign(ResignRequest) returns (ResignResponse);

  // Discover current leader
  rpc GetLeader(GetLeaderRequest) returns (GetLeaderResponse);

  // Watch for leader changes (server streaming)
  rpc WatchLeader(WatchLeaderRequest) returns (stream LeaderEvent);

  // List all candidates for an election
  rpc ListCandidates(ListCandidatesRequest)
      returns (ListCandidatesResponse);

  // Health check for leader (observer-initiated)
  rpc CheckLeaderHealth(HealthCheckRequest) returns (HealthCheckResponse);
}

message CampaignRequest {
  string election_name = 1;
  string namespace = 2;
  string candidate_id = 3;
  int32 lease_ttl_seconds = 4;      // default 15
  int32 campaign_timeout_ms = 5;    // 0 = wait forever
  int32 priority = 6;               // higher = preferred
  map<string,string> metadata = 7;  // address, version, etc.
}

message CampaignResponse {
  bool elected = 1;
  int64 fencing_token = 2;
  int64 lease_id = 3;
  string error = 4;
}

message LeaderEvent {
  enum Type {
    ELECTED = 0;        // New leader elected
    RESIGNED = 1;       // Leader voluntarily resigned
    EXPIRED = 2;        // Leader lease expired (crash/partition)
    HEALTH_DEGRADED = 3; // Leader stepped down due to health
  }
  Type type = 1;
  string election_name = 2;
  string leader_id = 3;        // new leader (or empty if no leader)
  string previous_leader_id = 4;
  int64 fencing_token = 5;
  string timestamp = 6;
}
```

### REST Endpoints

| Method | Path | Description |
|--------|------|-------------|
| POST | `/v1/elections/{name}/campaign` | Start campaign |
| DELETE | `/v1/elections/{name}/leader` | Resign |
| GET | `/v1/elections/{name}/leader` | Get current leader |
| GET | `/v1/elections/{name}/candidates` | List candidates |
| GET | `/v1/elections/{name}/watch` | SSE stream of leader changes |
| GET | `/v1/elections` | List all election groups |
| POST | `/v1/elections/{name}/health` | Report leader health |

### CLI

```bash
# Start campaign
electctl campaign scheduler --candidate scheduler-host-1 --ttl 15s
# Output: Elected as leader. Fencing token: 47

# Check leader
electctl leader scheduler
# Output:
# Election:      scheduler
# Leader:        scheduler-host-1
# Fencing Token: 47
# Elected At:    2026-04-09T10:15:00Z
# Lease TTL:     15s (12s remaining)
# Address:       10.0.1.5:8080

# List candidates
electctl candidates scheduler
# Output:
# CANDIDATE          STATE     PRIORITY  JOINED
# scheduler-host-1   LEADER    100       2026-04-09T10:15:00Z
# scheduler-host-2   STANDBY   100       2026-04-09T10:14:00Z
# scheduler-host-3   STANDBY   100       2026-04-09T10:14:30Z

# Resign
electctl resign scheduler --candidate scheduler-host-1
# Output: Resigned. New leader will be elected.

# Watch
electctl watch scheduler
# [2026-04-09T10:15:00Z] ELECTED    scheduler-host-1  token=47
# [2026-04-09T14:30:00Z] RESIGNED   scheduler-host-1
# [2026-04-09T14:30:02Z] ELECTED    scheduler-host-2  token=48
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: etcd-Based Leader Election

**Why it's hard:**
Leader election must guarantee that at most one leader exists at any time (safety), while ensuring that a leader is eventually elected (liveness). The challenging scenarios are: (1) leader crash during operation (stale operations must be fenced), (2) network partition where the old leader is still alive but isolated, (3) election tie when multiple candidates campaign simultaneously, (4) lease renewal failure due to brief network hiccup (should not trigger unnecessary failover).

**Approaches:**

| Approach | Safety | Failover Time | Complexity | Used By |
|----------|--------|--------------|------------|---------|
| **etcd lease + CAS** | **Linearizable** | **TTL + election (~15s)** | **Medium** | **k8s, etcd clientv3** |
| ZooKeeper ephemeral sequential | Linearizable | Session timeout (~10-30s) | Medium | Kafka, HBase |
| Raft (built-in) | Linearizable | Election timeout (1-5s) | High (implement Raft) | etcd itself, CockroachDB |
| Database advisory lock | Serializable | Depends on lock timeout | Low | Simple HA systems |
| Consul session + KV | Linearizable | Session TTL (~15s) | Medium | Consul-based services |
| Redis SETNX | NOT linearizable | TTL | Low | NOT safe for leader election |

**Selected: etcd lease + Compare-And-Swap transaction**

```python
class EtcdLeaderElection:
    """
    Leader election using etcd v3 API.
    Implements the campaign/observe pattern from etcd's concurrency package.
    """

    def __init__(self, etcd_client, election_name, candidate_id,
                 lease_ttl=15):
        self.client = etcd_client
        self.election_name = election_name
        self.candidate_id = candidate_id
        self.lease_ttl = lease_ttl
        self.leader_key = f"/elections/{election_name}/leader"
        self.lease_id = None
        self.fencing_token = None
        self.is_leader = False
        self._cancel = threading.Event()

    def campaign(self, timeout_sec=None):
        """
        Campaign to become leader. Blocks until elected or timeout.

        Algorithm:
        1. Create a lease with TTL.
        2. Attempt to create the leader key (If-Not-Exists).
        3. If successful: we are the leader.
        4. If not: watch the key; when deleted, retry step 2.
        """
        # Step 1: Create lease
        self.lease_id = self.client.lease_grant(self.lease_ttl)

        # Start lease keep-alive in background
        self._start_keepalive()

        deadline = time.time() + timeout_sec if timeout_sec else float('inf')

        while time.time() < deadline and not self._cancel.is_set():
            # Step 2: Try to create leader key atomically
            # etcd transaction:
            #   IF key does NOT exist
            #   THEN create key with our candidate_id and attach our lease
            #   ELSE fail (someone else is leader)

            leader_value = json.dumps({
                "candidate_id": self.candidate_id,
                "elected_at": datetime.utcnow().isoformat(),
                "metadata": self._get_metadata()
            })

            success, responses = self.client.transaction(
                compare=[
                    # Key does not exist (create_revision == 0)
                    self.client.transactions.create(self.leader_key) == 0
                ],
                success=[
                    self.client.transactions.put(
                        self.leader_key,
                        leader_value,
                        lease=self.lease_id
                    )
                ],
                failure=[
                    self.client.transactions.get(self.leader_key)
                ]
            )

            if success:
                # We are the leader!
                self.is_leader = True
                self.fencing_token = self._increment_fencing_token()
                log.info(
                    f"Elected as leader for {self.election_name}. "
                    f"Fencing token: {self.fencing_token}"
                )
                return CampaignResult(
                    elected=True,
                    fencing_token=self.fencing_token
                )

            # Step 3: Someone else is leader. Watch for key deletion.
            current_leader = json.loads(responses[0][0].value)
            log.info(
                f"Leader is {current_leader['candidate_id']}. "
                f"Watching for vacancy..."
            )

            # Step 4: Watch and wait
            self._wait_for_vacancy(deadline)

        return CampaignResult(elected=False, error="TIMEOUT")

    def _wait_for_vacancy(self, deadline):
        """
        Watch the leader key. Return when it's deleted
        (leader died or resigned).
        """
        remaining = deadline - time.time()
        if remaining <= 0:
            return

        try:
            watcher = self.client.watch(
                self.leader_key,
                timeout=remaining
            )

            for event in watcher:
                if event.type == EventType.DELETE:
                    log.info("Leader key deleted. Campaigning...")
                    return  # Key deleted — try to become leader
                elif event.type == EventType.PUT:
                    # Leader key updated (different leader)
                    continue

        except WatchTimeout:
            pass  # Timeout — will check deadline in outer loop

    def resign(self):
        """Voluntarily give up leadership."""
        if not self.is_leader:
            return

        # Revoke lease — this deletes the leader key and all
        # keys attached to this lease
        self.client.lease_revoke(self.lease_id)
        self.is_leader = False
        log.info(f"Resigned from {self.election_name}")

    def _start_keepalive(self):
        """
        Background thread that renews the lease every TTL/3 seconds.
        If renewal fails, the leader must step down.
        """
        def keepalive_loop():
            interval = self.lease_ttl / 3.0
            consecutive_failures = 0
            max_failures = 2  # Step down after 2 consecutive failures

            while not self._cancel.is_set():
                try:
                    self.client.lease_keepalive(self.lease_id)
                    consecutive_failures = 0
                except Exception as e:
                    consecutive_failures += 1
                    log.warning(
                        f"Lease renewal failed ({consecutive_failures}/"
                        f"{max_failures}): {e}"
                    )

                    if consecutive_failures >= max_failures:
                        log.error(
                            "Lost lease. Stepping down as leader."
                        )
                        self.is_leader = False
                        self._on_leadership_lost()
                        return

                time.sleep(interval)

        thread = Thread(target=keepalive_loop, daemon=True)
        thread.start()

    def _on_leadership_lost(self):
        """
        Called when leadership is lost (lease expired or revoked).
        The application must stop all leader-only operations.
        """
        log.error(
            f"LEADERSHIP LOST for {self.election_name}. "
            f"Stopping all leader operations. "
            f"Fencing token {self.fencing_token} is now invalid."
        )
        # Application callback
        if self.on_lost_callback:
            self.on_lost_callback()

    def _increment_fencing_token(self):
        """
        Atomically increment the fencing token counter.
        Stored in a separate etcd key for persistence across leaders.
        """
        token_key = f"/elections/{self.election_name}/fencing_token"

        while True:
            current = self.client.get(token_key)
            current_value = int(current.value) if current else 0
            new_value = current_value + 1

            success, _ = self.client.transaction(
                compare=[
                    self.client.transactions.mod_revision(token_key) ==
                    (current.mod_revision if current else 0)
                ],
                success=[
                    self.client.transactions.put(
                        token_key, str(new_value))
                ],
                failure=[]
            )

            if success:
                return new_value
            # CAS failed — another leader incremented; retry
```

**ZooKeeper-Based Election (Alternative):**

```python
class ZooKeeperLeaderElection:
    """
    Leader election using ZooKeeper ephemeral sequential znodes.
    Same algorithm as the distributed lock, but for leadership.
    """

    def __init__(self, zk_client, election_path, candidate_id):
        self.zk = zk_client
        self.path = election_path  # e.g., "/elections/scheduler"
        self.candidate_id = candidate_id
        self.my_znode = None

    def campaign(self):
        """
        1. Create ephemeral sequential znode.
        2. Check if my znode has the lowest sequence number.
        3. If yes: I'm the leader.
        4. If no: watch the znode with sequence number just
           before mine. When it's deleted, re-check.
        """
        # Create: /elections/scheduler/candidate-00000000047
        self.my_znode = self.zk.create(
            path=f"{self.path}/candidate-",
            value=self.candidate_id.encode(),
            ephemeral=True,
            sequential=True
        )

        while True:
            children = sorted(self.zk.get_children(self.path))
            my_name = self.my_znode.split("/")[-1]

            if children[0] == my_name:
                # I have the lowest sequence number — I'm the leader!
                return ElectionResult(elected=True)

            # Find my predecessor
            my_index = children.index(my_name)
            predecessor = children[my_index - 1]

            # Watch predecessor (not all children — avoids herd)
            event = threading.Event()

            def on_delete(watch_event):
                if watch_event.type == EventType.DELETED:
                    event.set()

            stat = self.zk.exists(
                f"{self.path}/{predecessor}",
                watch=on_delete
            )

            if stat is None:
                continue  # Predecessor already gone; re-check

            event.wait()  # Wait for predecessor deletion
```

**Failure Modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| Leader crashes | Leadership vacant for TTL duration | TTL should be short (10-15s); standbys campaign immediately |
| Leader network-partitioned | Leader still alive but can't renew; standby elected | Fencing tokens prevent old leader's stale operations |
| etcd leader crashes | Election operations delayed until new etcd leader | etcd election completes in 1-3s; total impact < 5s |
| All candidates crash | No leader until at least one recovers | k8s restarts pods; supervisor restarts bare-metal processes |
| Split-brain (two leaders) | IMPOSSIBLE with etcd/ZK (linearizable CAS) | The CAS transaction ensures only one winner |
| Lease keepalive delayed (GC pause) | Lease may expire; leader loses leadership | Use short keepalive interval (TTL/3); set reasonable TTL |
| Rapid flip-flop (leader keeps being elected then failing) | Instability; downstream disrupted | Backoff: after resigning/losing, wait before re-campaigning |

**Interviewer Q&A:**

**Q1: Why can't you use Redis SETNX for leader election?**
A: Redis is not linearizable. In master-replica setup, if the master crashes after SETNX but before replicating, the promoted replica doesn't have the key. A second candidate can acquire it — two leaders. Even Redlock (multi-master) has the same clock-dependent safety issues as for distributed locks (Kleppmann critique). For leader election, you need a consensus-based store (etcd, ZooKeeper).

**Q2: What lease TTL would you recommend?**
A: 10-15 seconds. With keepalive every TTL/3 (3-5s), the leader has 2 retries before the lease expires. Too short (< 5s): risk of spurious failover during brief network hiccups. Too long (> 30s): slow failover when the leader actually crashes. For bare-metal schedulers (where failover latency matters), use 10s TTL.

**Q3: How does Kubernetes leader election work internally?**
A: k8s uses a Lease object (`coordination.k8s.io/v1`). The leader periodically updates `renewTime`. Other candidates check: if `renewTime + leaseDuration < now`, the lease is expired, and they attempt to acquire it (update with their identity). Under the hood, this uses etcd's optimistic concurrency (`resourceVersion`). The k8s client-go library provides `leaderelection.LeaderElector` with callbacks (`OnStartedLeading`, `OnStoppedLeading`).

**Q4: What is the difference between etcd `Campaign` and a simple `PUT`?**
A: etcd's `concurrency.Election.Campaign()` (Go client) does more than a simple PUT: (1) creates a lease, (2) creates a key under the election prefix with the lease, (3) waits for the key to have the lowest create-revision among all keys under the prefix (FIFO fairness), (4) returns when elected. A simple PUT doesn't provide fairness or wait-for-vacancy semantics.

**Q5: How do you prevent a leader from doing damage after losing its lease?**
A: (1) Fencing tokens: the leader's operations carry a token; downstream rejects stale tokens. (2) Self-awareness: the leader SDK detects lost lease and calls `OnStoppedLeading` callback; the application stops operations. (3) Short lease TTL: minimizes the window where the old leader might still be operating. (4) STONITH (bare-metal): physically power-fence the old leader node.

**Q6: What is STONITH and when is it used?**
A: STONITH = Shoot The Other Node In The Head. In bare-metal HA clusters (Pacemaker/Corosync), when a node is suspected to have failed but might still be running, the surviving node issues an IPMI power-off command to the failed node's BMC. This guarantees the old node is truly dead before the new leader takes over. It's a hardware-level fencing mechanism used when software-level fencing (tokens) is insufficient — e.g., if the failed node has direct disk access that can't be token-gated.

---

### Deep Dive 2: Split-Brain Prevention

**Why it's hard:**
Split-brain is the most dangerous failure mode: two nodes both believe they are the leader and issue conflicting commands. It can corrupt data, double-provision bare-metal servers, or cause billing errors. Preventing split-brain requires that the election mechanism is based on consensus, and that even in the presence of network partitions, at most one leader can operate.

**Why Two Nodes Cannot Safely Elect:**

```
Scenario: 2-node cluster, each with 50% voting weight.

          Network Partition
          ─────────────
Node A          │          Node B
(thinks B dead) │  (thinks A dead)
"I'm leader"   │  "I'm leader"
                │
        SPLIT BRAIN!

With 2 nodes, neither can form a majority (need > 50% = 2 nodes).
Both nodes are stuck OR both claim leadership.

Minimum safe cluster size: 3 nodes (quorum = 2).
Tolerate 1 failure. During partition: one side has 2 nodes (majority),
the other has 1 (minority). Only the majority side can elect.
```

**Quorum Requirement:**

```
N nodes → quorum = floor(N/2) + 1

N=1: quorum=1 → no fault tolerance
N=2: quorum=2 → no fault tolerance (both must agree)
N=3: quorum=2 → tolerate 1 failure
N=4: quorum=3 → tolerate 1 failure (same as N=3!)
N=5: quorum=3 → tolerate 2 failures
N=6: quorum=4 → tolerate 2 failures (same as N=5!)
N=7: quorum=4 → tolerate 3 failures

Rule: Always use odd numbers (3, 5, 7).
Even numbers waste a node (N=4 and N=3 have same fault tolerance).
```

**Implementation of Split-Brain Protection:**

```python
class SplitBrainProtection:
    """
    Ensures the leader can only operate when it has confirmed
    connectivity to the consensus store (etcd).
    """

    def __init__(self, election, health_checker):
        self.election = election
        self.health = health_checker
        self.operating = False

    def leader_loop(self, on_become_leader, on_lose_leader,
                     work_function):
        """
        Main loop for a leader candidate.
        Continuously verifies leadership before performing work.
        """
        while True:
            # Campaign (blocks until elected)
            result = self.election.campaign(timeout_sec=60)
            if not result.elected:
                continue

            self.operating = True
            on_become_leader(result.fencing_token)

            try:
                while self.election.is_leader:
                    # Verify we can still reach consensus store
                    if not self._verify_leadership():
                        log.error(
                            "Cannot verify leadership. Stepping down."
                        )
                        break

                    # Verify own health
                    if not self.health.is_healthy():
                        log.error(
                            "Health degraded. Stepping down."
                        )
                        self.election.resign()
                        break

                    # Do leader work
                    work_function(result.fencing_token)

            finally:
                self.operating = False
                on_lose_leader()

            # Backoff before re-campaigning
            time.sleep(random.uniform(1, 5))

    def _verify_leadership(self):
        """
        Active verification: read the leader key from etcd
        and confirm it still has our candidate_id.

        This catches the case where our lease expired (network
        partition) but we haven't detected it yet locally.
        """
        try:
            leader_record = self.election.client.get(
                self.election.leader_key)
            if leader_record is None:
                return False  # Key deleted — we're not leader

            leader_data = json.loads(leader_record.value)
            return (
                leader_data["candidate_id"] ==
                self.election.candidate_id
            )
        except Exception:
            return False  # Can't reach etcd — assume not leader
```

**Leader Lease with Time-Limited Operations:**

```python
class LeaderLease:
    """
    Time-limited leadership token that auto-expires.
    Provides a stronger guarantee than checking is_leader:
    operations are bounded by the lease duration.
    """

    def __init__(self, leader_id, fencing_token, ttl_seconds,
                 granted_at):
        self.leader_id = leader_id
        self.fencing_token = fencing_token
        self.ttl = ttl_seconds
        self.granted_at = granted_at
        self.renewed_at = granted_at
        # Subtract safety margin for clock drift
        self.clock_safety_margin = 2  # seconds

    def is_valid(self):
        """
        Check if the lease is still valid locally.
        Uses monotonic clock to avoid wall clock issues.
        """
        elapsed = time.monotonic() - self.renewed_at
        return elapsed < (self.ttl - self.clock_safety_margin)

    def execute_if_valid(self, operation):
        """
        Execute an operation only if the lease is still valid.
        This is the core safety mechanism.
        """
        if not self.is_valid():
            raise LeaseExpiredError(
                f"Lease expired. Elapsed: {self._elapsed()}s, "
                f"TTL: {self.ttl}s"
            )

        # Check if operation can complete within remaining lease
        remaining = self.ttl - self.clock_safety_margin - self._elapsed()
        if remaining < operation.estimated_duration:
            raise InsufficientLeaseTimeError(
                f"Operation needs {operation.estimated_duration}s "
                f"but only {remaining}s remaining"
            )

        return operation.execute(fencing_token=self.fencing_token)
```

**Interviewer Q&A:**

**Q1: Can you have a split-brain with etcd?**
A: No, if etcd is configured correctly. etcd uses Raft, which requires a majority quorum. In a 3-node cluster, a partition creates a majority (2) and minority (1). Only the majority side can commit writes. The minority side rejects all writes. Since leader election is a write (creating/updating the leader key), only one side can elect a leader. The old leader on the minority side cannot renew its lease (write requires quorum) and steps down.

**Q2: What if the network partition is asymmetric (A can reach etcd but not B, and B can reach etcd but not A)?**
A: This is fine. Both A and B can talk to etcd. etcd's linearizable guarantees ensure that if A is the leader (holds the key), B's campaign fails (key exists). There's no split-brain because etcd is the single source of truth, and both sides agree on its state.

**Q3: What is the "zombie leader" problem?**
A: A leader that has lost its lease (due to network partition or GC pause) but continues operating because it hasn't detected the loss yet. During this window, both the zombie and the new leader may operate. Fencing tokens solve this: the zombie's operations carry an old token; downstream services reject them. The detection delay is bounded by the lease check interval.

**Q4: How do you handle a leader that is alive but slow (partially degraded)?**
A: Health-based step-down: the leader periodically checks its own health (can it reach the database? is CPU < 90%? is memory OK?). If health degrades, the leader voluntarily resigns, and a healthier candidate takes over. This is proactive — don't wait for the lease to expire.

**Q5: What is the "pre-vote" optimization in Raft?**
A: In Raft, if a follower's election timer expires, it becomes a candidate and increments the term. But if the leader is actually fine (the follower was temporarily partitioned), this disrupts the cluster. Pre-vote adds a step: the follower first asks peers "Would you vote for me?" without incrementing the term. If a majority says no (they're receiving heartbeats from the current leader), the follower doesn't campaign. This prevents unnecessary leader changes due to transient network issues.

**Q6: How do you implement priority-based leader election (prefer certain candidates)?**
A: Three approaches: (1) Priority key ordering: candidates create keys with their priority embedded; the lowest-sequence key among the highest-priority candidates wins. (2) Weighted lease duration: higher-priority candidates get longer leases, making them harder to preempt. (3) Step-down protocol: if a higher-priority candidate joins, the current (lower-priority) leader voluntarily resigns. We prefer (3) because it's explicit and doesn't interfere with the core election mechanism.

---

### Deep Dive 3: Client-Side Leader Discovery

**Why it's hard:**
Clients need to know who the current leader is to send requests. But the leader can change at any time. Stale leader information causes requests to fail. The discovery mechanism must be fast, accurate, and handle leader transitions gracefully.

**Approaches:**

| Approach | Latency | Freshness | Complexity |
|----------|---------|-----------|------------|
| Direct etcd query | 1-5 ms | Real-time | Low |
| **etcd Watch (streaming)** | **< 100 ms** | **Real-time** | **Medium** |
| DNS (SRV record for leader) | TTL-dependent (5-30s) | Stale by TTL | Low |
| VIP (Virtual IP failover) | Transparent | 1-5s failover | Medium (network config) |
| Leader endpoint on all candidates | 1 ms | Immediate | Low |
| Gossip-based | 1-5s convergence | Eventually consistent | High |

**Selected: etcd Watch for real-time + VIP for legacy clients**

```python
class LeaderDiscoveryClient:
    """
    Client-side SDK for discovering the current leader.
    Maintains a local cache updated by etcd Watch.
    """

    def __init__(self, etcd_client, election_name):
        self.client = etcd_client
        self.election_name = election_name
        self.leader_key = f"/elections/{election_name}/leader"
        self.current_leader = None
        self.leader_address = None
        self.fencing_token = None
        self._watchers = []
        self._start_watch()

    def get_leader(self):
        """
        Return the current leader. Uses cached value.
        If cache is empty, fetch from etcd.
        """
        if self.current_leader is None:
            self._refresh()
        return LeaderInfo(
            candidate_id=self.current_leader,
            address=self.leader_address,
            fencing_token=self.fencing_token
        )

    def wait_for_leader(self, timeout_sec=30):
        """
        Block until a leader is available.
        Used during startup when leader might not be elected yet.
        """
        deadline = time.time() + timeout_sec
        while time.time() < deadline:
            leader = self.get_leader()
            if leader.candidate_id:
                return leader
            time.sleep(0.5)
        raise NoLeaderError(f"No leader for {self.election_name} "
                            f"after {timeout_sec}s")

    def _start_watch(self):
        """Watch for leader changes and update local cache."""
        def watch_loop():
            while True:
                try:
                    watcher = self.client.watch(self.leader_key)
                    for event in watcher:
                        if event.type == EventType.PUT:
                            data = json.loads(event.value)
                            self.current_leader = data["candidate_id"]
                            self.leader_address = data["metadata"].get(
                                "address")
                            self.fencing_token = data.get("fencing_token")
                            log.info(
                                f"New leader for {self.election_name}: "
                                f"{self.current_leader}"
                            )
                            self._notify_watchers("NEW_LEADER")

                        elif event.type == EventType.DELETE:
                            old_leader = self.current_leader
                            self.current_leader = None
                            self.leader_address = None
                            log.warning(
                                f"Leader lost for {self.election_name}. "
                                f"Previous: {old_leader}"
                            )
                            self._notify_watchers("LEADER_LOST")

                except Exception as e:
                    log.error(f"Watch error: {e}. Reconnecting...")
                    time.sleep(1)
                    self._refresh()  # Full refresh on reconnect

        Thread(target=watch_loop, daemon=True).start()

    def on_leader_change(self, callback):
        """Register a callback for leader changes."""
        self._watchers.append(callback)
```

**VIP-Based Discovery (for bare-metal HA):**

```python
class VIPManager:
    """
    Manages a Virtual IP that follows the leader.
    Used for bare-metal services where clients connect to a fixed IP.
    """

    def __init__(self, vip_address, interface, election):
        self.vip = vip_address     # e.g., "10.0.1.100"
        self.interface = interface  # e.g., "eth0"
        self.election = election

    def on_become_leader(self, fencing_token):
        """Assign VIP to this node when elected."""
        # Add VIP to network interface
        self._run_cmd(
            f"ip addr add {self.vip}/32 dev {self.interface}")
        # Send gratuitous ARP to update switches
        self._run_cmd(
            f"arping -A -c 3 -I {self.interface} {self.vip}")
        log.info(f"VIP {self.vip} assigned to {self.interface}")

    def on_lose_leader(self):
        """Remove VIP from this node."""
        self._run_cmd(
            f"ip addr del {self.vip}/32 dev {self.interface}")
        log.info(f"VIP {self.vip} removed from {self.interface}")
```

**Interviewer Q&A:**

**Q1: How do Kubernetes clients discover the leader controller-manager?**
A: They don't — Kubernetes clients talk to the API server, not the controller-manager. The API server routes to any healthy API server (via a load balancer). The controller-manager's leader election is internal (it determines which instance runs the control loops). The elected controller-manager reads from and writes to the API server. Non-leader instances are idle.

**Q2: What is the trade-off between VIP and DNS-based leader discovery?**
A: VIP: instant failover (gratuitous ARP updates switch tables in milliseconds); no client-side changes. But: requires L2 network adjacency, single subnet, only works for bare-metal/VM. DNS: works across networks, easy to implement. But: subject to DNS caching; failover delay = DNS TTL (5-30s). For k8s services (overlay network), use a Service pointing to the leader pod (label selector updated on election).

**Q3: How do you handle the gap between leader loss and new leader election?**
A: Clients must handle "no leader" gracefully. Strategies: (1) Queue requests and replay when a new leader appears. (2) Return 503 to callers (service unavailable). (3) Route to any candidate (for read-only operations, any replica may serve stale data). For the job scheduler, we queue job submissions for up to 30 seconds during failover.

**Q4: How do you test leader failover?**
A: (1) Kill the leader process; verify a new leader is elected within TTL + election time. (2) Network-partition the leader; verify failover. (3) Cause a GC pause on the leader (pause the JVM with `kill -STOP`); verify lease expires and failover occurs. (4) Rapid kill/restart cycles; verify no split-brain. (5) Measure failover time end-to-end (last request served by old leader to first request served by new leader).

**Q5: How does Kafka determine partition leaders?**
A: Kafka uses ZooKeeper (< 3.0) or KRaft (>= 3.0) for controller election. The controller is a special broker that manages partition leadership. When a broker fails, the controller reassigns partition leaders from the ISR (In-Sync Replicas) list. Each partition has a leader and N replicas. The controller writes the new leader to ZooKeeper/KRaft, and brokers learn via watches.

**Q6: What is a "leader lease" and how does it differ from a "leader lock"?**
A: A lock is a binary resource (held or not). A lease is a time-bounded lock that auto-expires. For leader election, leases are preferred because they handle the crash case automatically: if the leader crashes and can't release the lock, the lease expires after TTL. With a non-leased lock, the lock is stuck forever (or until an admin intervenes).

---

## 7. Scheduling & Resource Management

### Leader Election for Job Scheduler

```python
class SchedulerHA:
    """
    High-availability job scheduler using leader election.
    Only the leader processes the job queue.
    """

    def __init__(self, election_service, job_queue, resource_manager):
        self.election = election_service
        self.queue = job_queue
        self.resources = resource_manager
        self.fencing_token = None

    def run(self):
        """
        Main loop: campaign, process jobs if leader, observe if not.
        """
        while True:
            result = self.election.campaign(
                election_name="scheduler",
                candidate_id=self.node_id,
                lease_ttl=15
            )

            if result.elected:
                self.fencing_token = result.fencing_token
                log.info(f"Scheduler leader elected. "
                         f"Token: {self.fencing_token}")

                try:
                    self._leader_loop()
                except LeadershipLostError:
                    log.warning("Lost scheduler leadership")
                    continue
            else:
                log.info("Campaign timeout. Retrying...")

    def _leader_loop(self):
        """Process jobs while we are the leader."""
        while self.election.is_leader:
            # All job assignments carry the fencing token
            # Workers verify the token before executing
            jobs = self.queue.dequeue(batch_size=100)
            for job in jobs:
                self.resources.assign_job(
                    job,
                    fencing_token=self.fencing_token
                )
```

**Bare-Metal STONITH Integration:**

```python
class BareMetalLeaderFencing:
    """
    For bare-metal scheduler HA, use IPMI STONITH to fence
    the old leader node.
    """

    def on_become_leader(self, fencing_token, old_leader_bmc):
        """
        After winning election, fence the old leader.
        """
        if old_leader_bmc:
            # STONITH: power off the old leader
            ipmi_cmd = (
                f"ipmitool -H {old_leader_bmc.address} "
                f"-U {old_leader_bmc.user} "
                f"-P {old_leader_bmc.password} "
                f"power off"
            )
            result = subprocess.run(ipmi_cmd, shell=True,
                                     capture_output=True)
            if result.returncode == 0:
                log.info(f"STONITH: Powered off old leader at "
                         f"{old_leader_bmc.address}")
            else:
                log.error(f"STONITH FAILED: {result.stderr}")
                # If STONITH fails, we cannot safely proceed
                # without risk of split-brain
                raise STONITHFailureError(
                    "Cannot fence old leader. Aborting takeover.")
```

---

## 8. Scaling Strategy

| Dimension | Strategy | Detail |
|-----------|----------|--------|
| Election groups | Thousands supported | Each election is one etcd key; etcd handles millions of keys |
| Candidates per group | 2-7 typical | More candidates = more watchers; etcd handles easily |
| Leader discovery clients | 10K+ | etcd Watch multiplexing; client-side caching |
| Cross-datacenter | Per-DC election with DC-level failover | Each DC has independent leader; global coordinator for DC failover |
| etcd load from elections | < 1% of etcd capacity | Elections generate few writes (renewals: ~167/sec for 500 groups) |

**Interviewer Q&A:**

**Q1: Can leader election be a bottleneck?**
A: Rarely. Leader election generates very few writes (lease renewals). The main concern is etcd's overall load from all uses (k8s, config, locks, elections). For 500 election groups with 3-second lease renewal intervals, that's ~167 writes/sec to etcd — trivial.

**Q2: How do you handle leader election across datacenters?**
A: Don't use a single etcd cluster spanning DCs (latency kills Raft performance). Instead: (1) Each DC has its own leader election. (2) A "super-election" at the DC level determines which DC is primary. (3) Or: one DC's leader is the global leader, with cross-DC heartbeat. For our scheduler: each DC has a local scheduler leader; a global coordinator (with its own election) handles cross-DC job placement.

**Q3: What happens if etcd is temporarily slow (high latency)?**
A: Lease renewals may fail. If the lease expires, the leader steps down. To avoid false failover from transient etcd slowness: (1) Increase lease TTL (15s instead of 10s gives more buffer). (2) Use a local lease timer: the leader considers itself leader until `last_successful_renewal + TTL - safety_margin`, not based on the keepalive response time.

**Q4: How many candidates should an election group have?**
A: 2-3 for most services. Benefits of more candidates: faster failover (more candidates ready to take over). Costs: each candidate maintains a watch and is ready to take over; resource overhead. 3 candidates is the sweet spot: one leader, two standbys, survives 2 simultaneous failures.

**Q5: Can you run leader election without etcd or ZooKeeper?**
A: Yes, but carefully. (1) Raft-based (implement your own): correct but complex. (2) Database advisory locks: PostgreSQL's `pg_advisory_lock` — not as robust. (3) Cloud-native: AWS DynamoDB conditional writes (ConditionExpression: attribute_not_exists). (4) Kubernetes Lease objects: if running in k8s, this is the simplest path. For bare-metal infrastructure, etcd is the standard choice.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure | Detection | Impact | Recovery | RTO |
|---|---------|-----------|--------|----------|-----|
| 1 | Leader crashes | Lease expires (TTL) | Standby takes over | Standby campaigns; new leader in TTL + election time | 10-15s |
| 2 | Leader network-partitioned | Lease renewal fails | Leader steps down; standby elected | Old leader detects partition, stops operations | 10-15s |
| 3 | etcd node crash | Raft heartbeat timeout | etcd remains available (quorum) | etcd elects new internal leader; elections continue | 1-3s |
| 4 | All etcd nodes down | Elections impossible | No leader changes; current leaders continue if their leases are still valid locally | Restart etcd; elections resume | 5-15 min |
| 5 | All candidates crash | No leader | Service outage | k8s/supervisor restarts candidates; election runs | 30s-2 min |
| 6 | Rapid leader flapping | Leader elected, fails, elected, fails... | Instability; downstream confused | Exponential backoff on campaign; alert after N failovers | Self-healing + alert |
| 7 | Clock skew | Lease expiry mismatch | Leader thinks lease is valid but etcd expired it | Use monotonic clock; add safety margin | N/A (preventable) |
| 8 | GC pause on leader (JVM) | Lease expires during pause | Old leader wakes up thinking it's leader | Fencing token rejects old leader's ops; lease check on wake | Fencing: 0; Lease: TTL |

### Consensus & Coordination

**Raft Leader Election (inside etcd):**

```
etcd's own leader election uses Raft:

1. Each etcd node starts as Follower.
2. If a Follower doesn't receive heartbeat within election_timeout
   (randomized 1000-1500ms), it becomes Candidate.
3. Candidate increments term, votes for itself, requests votes from peers.
4. If Candidate receives majority votes: becomes Leader.
5. Leader sends heartbeats every 100ms to prevent new elections.

This is the election INSIDE etcd. Our application-level election
runs ON TOP of etcd using leases and transactions.

The relationship:
  etcd Raft election: determines which etcd node processes writes.
  Our application election: determines which application instance
  is the leader for a given resource group.
```

---

## 10. Observability

### Key Metrics

| # | Metric | Type | Alert Threshold | Why |
|---|--------|------|-----------------|-----|
| 1 | `election.leader.is_leader` | Boolean gauge per group | false for > 30s | No leader; service degraded |
| 2 | `election.failover.duration` | Histogram | > 15s | Slow failover |
| 3 | `election.failover.count` | Counter per group | > 3/hour | Flapping |
| 4 | `election.lease.renewal.latency` | Histogram | > 5s | Close to TTL; risk of false failover |
| 5 | `election.lease.renewal.failures` | Counter | > 2 consecutive | Leader about to lose lease |
| 6 | `election.candidates.count` | Gauge per group | < 2 | Reduced HA; one more failure = no leader |
| 7 | `election.fencing_token.current` | Gauge per group | N/A | Track election count |
| 8 | `election.fencing_token.rejections` | Counter | > 0 | Stale leader attempted operation |
| 9 | `election.split_brain.detected` | Counter | > 0 | CRITICAL: two leaders detected |
| 10 | `election.voluntary_resign.count` | Counter | spike | Rolling upgrade or health issue |
| 11 | `election.etcd.unreachable` | Counter | > 0 | Elections impaired |
| 12 | `election.campaign.wait_time` | Histogram | > 30s | Long wait for leadership |

---

## 11. Security

| Threat | Mitigation |
|--------|------------|
| Unauthorized candidate (rogue service campaigns) | mTLS: only authorized services can campaign; etcd RBAC per election group |
| Leader impersonation (client sends to fake leader) | Fencing token verification; mTLS for client-leader communication |
| Denial-of-leadership (flood etcd with campaign requests) | Rate limiting on campaign API; per-client campaign limits |
| STONITH abuse (power off arbitrary nodes) | IPMI credentials restricted to HA manager; audit all STONITH operations |
| Leader exfiltrates data before stepping down | Leader has minimum required permissions; audit all leader operations |
| etcd credential compromise | etcd certificates with short TTL; Vault-managed; network isolation |

---

## 12. Incremental Rollout Strategy

### Phase 1: Library Development (Week 1-2)
- Implement election SDK in Java and Python.
- Unit tests: campaign, resign, failover, fencing.
- Integration tests against etcd cluster.

### Phase 2: Non-Critical Services (Week 3-4)
- Apply leader election to log aggregator (currently single-instance).
- Validate failover behavior in staging.
- Measure failover time.

### Phase 3: Scheduler HA (Week 5-8)
- Apply to job scheduler.
- Test bare-metal provisioning during failover.
- Validate fencing tokens prevent stale operations.
- Chaos testing: random leader kill.

### Phase 4: All Control-Plane Services (Week 9-12)
- Apply to all singleton controllers.
- Standardize HA patterns across the platform.

**Rollout Q&A:**

**Q1: How do you test that fencing tokens actually work?**
A: Integration test: (1) Leader A acquires leadership with token T1. (2) Simulate network partition (pause A's lease renewal). (3) Leader B is elected with token T2. (4) Resume A — it still thinks it's leader. (5) A attempts an operation with token T1. (6) Verify the operation is rejected (T1 < T2). If accepted, the fencing implementation is broken.

**Q2: How do you handle in-flight operations during failover?**
A: In-flight operations on the old leader may complete or fail. If they complete, the fencing token ensures the downstream resource accepts them only if no newer leader has written. If they fail (connection dropped), the new leader re-processes them. Operations must be idempotent (duplicate execution = same result).

**Q3: What is the risk of leader election during a deployment?**
A: During a rolling deployment, each instance is restarted sequentially. If the leader is restarted, it loses leadership. The standby takes over. When the old leader restarts (with new version), it re-campaigns. If it wins, leadership transfers back. This is a planned failover. Mitigation: configure the deployment to restart the leader last; or use voluntary resign before restart.

**Q4: How do you validate that split-brain is impossible?**
A: (1) Formal verification: TLA+ model of the election protocol; verify the invariant "at most one leader per group." (2) Jepsen testing: inject partitions, clock skew, process pauses; verify no two candidates simultaneously hold the leader key. (3) Runtime monitoring: each leader periodically reads the leader key and verifies it matches its own identity. Alert on mismatch.

**Q5: How do you handle the case where the fencing token store (in the protected resource) is lost?**
A: If the protected resource loses its `last_seen_token` (e.g., database restart), it must bootstrap from the election service. On startup, query the current fencing token and set `last_seen_token` to that value. This prevents stale tokens from being accepted. The startup sequence must complete before the resource accepts writes.

---

## 13. Trade-offs & Decision Log

| # | Decision | Options | Selected | Rationale |
|---|----------|---------|----------|-----------|
| 1 | Consensus store | etcd vs ZooKeeper vs custom Raft | etcd | k8s ecosystem; better Watch API; Go-native; simpler operations |
| 2 | Election mechanism | Lease-based CAS vs ephemeral znodes vs Raft | Lease-based CAS (etcd) | Most straightforward; well-supported by etcd client libraries |
| 3 | Failover time target | 5s vs 10s vs 30s | 10-15s | Balance between false failover (too short) and actual failover delay (too long) |
| 4 | Fencing mechanism | Tokens vs STONITH vs both | Tokens (software) + STONITH (bare-metal) | Software fencing for all cases; hardware fencing as additional safety for bare-metal |
| 5 | Client discovery | Watch vs DNS vs VIP | Watch (primary) + VIP (bare-metal) | Watch for real-time in k8s; VIP for bare-metal legacy clients |
| 6 | Leader health check | Self-check vs external vs both | Both | Self-check catches degradation; external catches zombie leaders |
| 7 | Lease TTL | 5s vs 10s vs 15s vs 30s | 15s | 3 keep-alive opportunities (every 5s); tolerates 2 missed renewals |
| 8 | Campaign fairness | FIFO vs random vs priority | Priority-based with FIFO tiebreak | Preferred candidates (higher capacity, newer version) win; FIFO among equal priority |

---

## 14. Agentic AI Integration

**1. Predictive Failover:**
```
Agent monitors:
  - Leader health metrics (CPU, memory, disk, network)
  - Lease renewal latency trend
  - Leader error rate

Predictions:
  - "Scheduler leader's renewal latency is trending up
    (5ms -> 8ms -> 12ms over last 10 minutes).
    Likely to hit 15s TTL in ~30 minutes.
    Recommend proactive resign and failover."

Actions:
  - Trigger voluntary resign before lease expires.
  - Pre-warm standby with latest state.
  - Zero-surprise failover (planned > unplanned).
```

**2. Failover Quality Analysis:**
```
After each failover, agent analyzes:
  - Time from leader loss to new leader operational
  - Number of requests that failed during failover
  - Duration of split-brain window (should be 0)
  - Data consistency after failover

Report:
  - "Failover #47: 12.3s total (TTL expiry: 10s, election: 0.5s,
    warm-up: 1.8s). 47 requests failed (0.02% of traffic).
    No split-brain detected. New leader caught up with 3 pending
    jobs in 0.5s."
```

**3. Optimal Candidate Selection:**
```python
class CandidateRankingAgent:
    """Ranks candidates by health, capacity, and location."""

    def rank_candidates(self, election_name):
        candidates = self.election.list_candidates(election_name)

        scores = []
        for c in candidates:
            health = self.metrics.get_health_score(c.candidate_id)
            cpu = self.metrics.get_cpu_available(c.candidate_id)
            zone_diversity = self._zone_score(c, candidates)

            score = (health * 0.4 + cpu * 0.3 + zone_diversity * 0.3)
            scores.append((c, score))

        scores.sort(key=lambda x: -x[1])
        return [
            {"candidate": c.candidate_id,
             "score": f"{s:.2f}",
             "recommendation": "PREFERRED" if s > 0.8 else "ACCEPTABLE"}
            for c, s in scores
        ]
```

---

## 15. Complete Interviewer Q&A Bank

**Q1: What is the fundamental difference between a distributed lock and leader election?**
A: Semantically, they're almost identical — both ensure at most one holder. The difference is intent: a lock is held briefly (for a critical section), while leadership is held long-term (for the duration of a process's lifetime). Technically: leader election adds the concepts of "campaign" (persistent desire to become leader), "observe" (learn who the leader is), and health-based failover. A lock doesn't have observers or health checks.

**Q2: Why does Kubernetes use Lease objects instead of direct etcd locks?**
A: Abstraction and portability. Kubernetes Lease objects are standard k8s API resources, managed by the API server. They work with any etcd backend and don't require direct etcd client access. The API server handles authentication, authorization, and audit logging. Direct etcd locks would bypass these controls and couple controllers tightly to etcd.

**Q3: How does Raft leader election differ from application-level leader election?**
A: Raft leader election is for the consensus protocol itself (which node replicates logs). Application-level election is for the application's HA (which scheduler instance processes jobs). Raft election happens within the consensus cluster (3-5 nodes); application election happens between application instances (2-3 instances) using the consensus cluster as the arbiter. They're orthogonal.

**Q4: What is the "election storm" problem?**
A: When the current leader fails and multiple standbys campaign simultaneously, they may all create their lease and attempt the CAS transaction in a tight loop. If all fail (rare with etcd's linearizable CAS, but possible with timing), they retry. With N candidates retrying simultaneously, etcd sees a burst of failed transactions. Mitigation: random backoff before campaign retry (like Raft's randomized election timeout).

**Q5: Can you have leader election with exactly-once semantics?**
A: Leader election guarantees at-most-one leader at any time. But during failover, there's a brief period with no leader. And the old leader might complete in-flight operations after a new leader is elected. Combined with fencing tokens and idempotent operations, you get effectively-once semantics. True exactly-once requires the leader's state to be perfectly replicated, which is the domain of state machine replication (Raft).

**Q6: How do you implement graceful leader handoff (no downtime)?**
A: (1) Old leader completes in-flight operations. (2) Old leader stops accepting new operations. (3) Old leader transfers state to designated successor (if stateful). (4) Old leader resigns. (5) Successor immediately wins election (pre-arranged). (6) Successor starts accepting operations. The gap is minimized because the successor is pre-selected and pre-warmed.

**Q7: What is "leader stickiness" and why does it matter?**
A: Leader stickiness means the current leader should remain leader as long as it's healthy (not lose leadership due to a momentary hiccup). Frequent leader changes cause disruption (state transfer, connection drain, client rediscovery). Implemented via: lease renewal (leader continuously extends its lease), priority (current leader has higher priority), and backoff (candidates wait before challenging the current leader).

**Q8: How does Google's Chubby handle leader election?**
A: Chubby provides a lock service; leader election is a lock acquisition. The leader holds a Chubby lock with a sequencer (fencing token). Chubby uses Paxos for consensus. The lock is session-based (session timeout = lease expiry). Chubby also provides a "master election" API that encapsulates the lock-based election pattern. Google's Bigtable, GFS, and MapReduce all use Chubby for master election.

**Q9: What is the CAP theorem implication for leader election?**
A: Leader election is a CP operation. During a network partition, only the majority partition can elect a leader. The minority partition has no leader. This is correct: the alternative (AP leader election) would allow two leaders during a partition, violating safety. The trade-off: during partition, some clients (on the minority side) cannot reach the leader.

**Q10: How do you handle a "zombie leader" that doesn't know it's been replaced?**
A: Three defenses: (1) Fencing tokens: the zombie's operations are rejected by downstream services. (2) Self-check: the zombie's SDK detects lease loss and calls `OnStoppedLeading`. (3) STONITH (bare-metal): the new leader physically powers off the zombie's node. Defense (1) is the most reliable because it doesn't depend on the zombie's cooperation.

**Q11: How does Apache Kafka's KRaft (Kafka Raft) leader election work?**
A: KRaft (Kafka 3.0+) replaces ZooKeeper with a built-in Raft implementation. The Kafka controller quorum (typically 3 brokers) runs Raft. The Raft leader is the active controller. Partition leadership is determined by the controller (not by Raft directly) — the controller assigns partition leaders from the ISR list and writes the assignment to the Raft log. This removes the ZooKeeper dependency.

**Q12: What is the difference between active-passive and active-active HA?**
A: Active-passive: one leader processes all requests; standbys are idle. Leader election determines who is active. Simple; no conflict resolution. Active-active: all instances process requests simultaneously. No leader election needed. But: requires conflict resolution (CRDTs, last-writer-wins, or application-level merge). Active-passive is simpler and safer for control-plane operations (scheduling, provisioning).

**Q13: How do you implement leader election for a service that runs on bare-metal (not in k8s)?**
A: (1) Deploy etcd cluster on bare-metal (3-5 nodes across racks). (2) Use the etcd client library (Go, Java, Python) in the service for campaign/renew/resign. (3) For fencing, use STONITH via IPMI. (4) For leader discovery, use VIP failover (keepalived + VRRP). This is the traditional approach used by OpenStack HA, Pacemaker/Corosync clusters.

**Q14: What is the "dueling leaders" scenario and how do you prevent it?**
A: Dueling leaders: two candidates continuously campaign, win briefly, then lose to the other — rapid flip-flop. Caused by: both candidates have the same priority and similar timing. Prevention: (1) Random backoff before re-campaigning. (2) Leader stickiness: current leader's lease renewal succeeds first (it doesn't need to do a CAS, just a keep-alive). (3) Priority: assign different priorities to candidates.

**Q15: How would you design leader election for a globally distributed system?**
A: (1) Per-region leaders: each region has its own leader for region-local operations. (2) Global leader: one region's leader is the "global leader" for cross-region operations. (3) The global leader election uses a cross-region consensus group (high latency: 100-200ms per round-trip). (4) Keep the global leader's responsibilities minimal (rare cross-region operations). (5) If the global leader's region goes down, another region's leader takes over via the cross-region election.

---

## 16. References

1. Ongaro, D. & Ousterhout, J. (2014). *In Search of an Understandable Consensus Algorithm* (Raft). USENIX ATC.
2. Burrows, M. (2006). *The Chubby Lock Service for Loosely-Coupled Distributed Systems*. OSDI.
3. etcd documentation. *Leader election*. https://etcd.io/docs/latest/dev-guide/api_concurrency_reference_v3/
4. Kubernetes documentation. *Leader Election*. https://kubernetes.io/docs/concepts/architecture/leases/
5. Apache Kafka. *KRaft: Apache Kafka Without ZooKeeper*. https://cwiki.apache.org/confluence/display/KAFKA/KIP-500
6. Hunt, P. et al. (2010). *ZooKeeper: Wait-free Coordination*. USENIX ATC.
7. Lamport, L. (2001). *Paxos Made Simple*. ACM SIGACT News.
8. Kingsbury, K. (Jepsen). *Analyses of distributed systems*. https://jepsen.io/analyses
9. Kleppmann, M. (2017). *Designing Data-Intensive Applications*. O'Reilly. Chapter 8.
10. Pacemaker/Corosync. *High Availability Cluster Stack*. https://clusterlabs.org/
