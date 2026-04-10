# System Design: Alerting and On-Call System

> **Relevance to role:** A cloud infrastructure platform engineer is responsible for the reliability of bare-metal hosts, Kubernetes clusters, OpenStack control planes, and critical platform services. The alerting and on-call system is the nerve center that detects failures, routes notifications to the right humans, and drives incident response. Deep understanding of Alertmanager routing, SLO-based alerting (multi-window multi-burn-rate), alert fatigue management, and escalation policies is essential for maintaining platform reliability.

---

## 1. Requirement Clarifications

### Functional Requirements
1. **Alert evaluation** from multiple sources: Prometheus alert rules (metric-based), ElastAlert (log-based), custom health checks, and synthetic monitoring.
2. **Alert routing** based on severity, service ownership, time of day, and on-call schedule to appropriate channels (PagerDuty, Slack, email).
3. **Alert grouping** to aggregate related alerts into a single notification (e.g., 50 hosts with disk full → one alert).
4. **Alert inhibition** to suppress downstream alerts when a root cause alert is already firing (e.g., if the network switch is down, suppress all host-unreachable alerts behind that switch).
5. **Alert silencing** for planned maintenance windows (e.g., silence all alerts for cluster-X during upgrade).
6. **On-call schedule management** with primary/secondary rotations, handoff procedures, and escalation policies.
7. **SLO-based alerting** using multi-window multi-burn-rate approach (Google SRE methodology).
8. **Runbook links** in every alert for guided incident response.
9. **Dead man's switch** to detect when the monitoring system itself fails.
10. **Alert lifecycle tracking**: firing → acknowledged → resolved, with timestamps and owner.

### Non-Functional Requirements
| Requirement | Target |
|---|---|
| Alert evaluation latency | < 30 seconds from metric anomaly to notification |
| Alert delivery reliability | 99.99% (missed alert = potential outage) |
| On-call notification latency | < 60 seconds from alert fire to phone call |
| False positive rate | < 5% of actionable alerts |
| Alert storm handling | Group 1000+ related alerts into < 10 notifications |
| Availability of alerting pipeline | 99.99% |

### Constraints & Assumptions
- Fleet: 50,000 bare-metal hosts, 200 Kubernetes clusters, 5,000 services.
- ~10,000 alert rules across all Prometheus instances.
- ~500 unique on-call rotations across teams.
- PagerDuty is the primary notification/escalation platform.
- Slack is the secondary channel for low-severity alerts and team awareness.
- Alertmanager is the alert routing and deduplication engine.

### Out of Scope
- Metric collection and storage (see metrics_and_monitoring_system.md).
- Log collection and search (see distributed_logging_system.md).
- Anomaly detection algorithms (see anomaly_detection_system.md).
- Incident management post-alert (war rooms, postmortems).

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Calculation | Value |
|---|---|---|
| Total alert rules | 200 clusters x 50 rules/cluster | 10,000 rules |
| Alert evaluation frequency | Every 15 seconds (aligned with scrape interval) | 15s |
| Alert evaluations per second | 10,000 / 15 | ~667 evaluations/sec |
| Active firing alerts (steady state) | ~200 (at any time, across the fleet) | 200 |
| Active firing alerts (during incident) | 500-5000 (alert storm) | Up to 5,000 |
| Notifications per day (normal) | ~200 alerts x 3 notifications avg (fire, remind, resolve) | ~600/day |
| Notifications per day (incident) | 5,000 alerts → grouped to ~50 notifications | ~150/incident |
| On-call rotations | 100 teams x 5 rotations avg (primary, secondary, management, etc.) | ~500 rotations |

### Latency Requirements

| Path | Target |
|---|---|
| Metric anomaly → Prometheus detects (scrape + rule eval) | < 30 seconds |
| Prometheus → Alertmanager | < 1 second |
| Alertmanager → PagerDuty API | < 5 seconds |
| PagerDuty → Phone call/push notification | < 30 seconds |
| **Total: anomaly → human notified** | **< 60 seconds** |

### Storage Estimates

| Component | Storage |
|---|---|
| Alert rules (YAML configs) | ~10 MB (version controlled in Git) |
| Alert history (1 year) | ~200 alerts/day x 365 x 10 KB = ~730 MB |
| On-call schedules | ~50 MB (managed in PagerDuty) |
| Silence history | ~10 MB/year |
| Alertmanager state (nflog, silences) | < 100 MB (in-memory + WAL) |

### Bandwidth Estimates

| Segment | Bandwidth |
|---|---|
| Prometheus → Alertmanager (alert payloads) | ~100 KB/s (small JSON payloads, not high bandwidth) |
| Alertmanager → PagerDuty/Slack | ~50 KB/s |
| Alertmanager cluster gossip | ~10 KB/s |

---

## 3. High Level Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        ALERT SOURCES                                        │
│                                                                             │
│  ┌──────────────────────┐ ┌──────────────────────┐ ┌────────────────────┐  │
│  │ Prometheus (per cluster)│ │ ElastAlert           │ │ Synthetic         │  │
│  │ - Metric-based rules   │ │ (log-based alerts)   │ │ Monitoring        │  │
│  │ - PromQL expressions   │ │ - Pattern match      │ │ (Blackbox Exporter│  │
│  │ - Threshold, rate,     │ │ - Frequency alerts   │ │  HTTP probes)     │  │
│  │   absence alerts       │ │ - Spike/flatline     │ │                   │  │
│  │                        │ │                      │ │                   │  │
│  │ Evaluates every 15s    │ │ Queries ES every 60s │ │ Probes every 30s  │  │
│  └──────────┬─────────────┘ └──────────┬───────────┘ └────────┬──────────┘  │
│             │                          │                      │             │
│             │ Prometheus alert API      │ Alertmanager API     │ AM API      │
│             │ (POST /api/v1/alerts)    │                      │             │
└─────────────┼──────────────────────────┼──────────────────────┼─────────────┘
              │                          │                      │
              ▼                          ▼                      ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                     ALERTMANAGER CLUSTER (HA)                                │
│                                                                             │
│  ┌───────────────────────────────────────────────────────────────────────┐  │
│  │  Instance 1 ◄──gossip──► Instance 2 ◄──gossip──► Instance 3          │  │
│  │                                                                       │  │
│  │  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐                │  │
│  │  │ Dispatcher   │  │ Inhibitor    │  │ Silencer     │                │  │
│  │  │ (dedup,      │  │ (suppress    │  │ (mute during │                │  │
│  │  │  group,      │  │  downstream  │  │  maintenance)│                │  │
│  │  │  route)      │  │  alerts)     │  │              │                │  │
│  │  └──────┬───────┘  └──────────────┘  └──────────────┘                │  │
│  │         │                                                             │  │
│  │         ▼                                                             │  │
│  │  ┌──────────────────────────────────────────────────────────┐         │  │
│  │  │                  ROUTING TREE                              │         │  │
│  │  │                                                            │         │  │
│  │  │  match: severity=critical                                  │         │  │
│  │  │  ├── match: team=platform                                  │         │  │
│  │  │  │   └── receiver: pagerduty-platform-oncall              │         │  │
│  │  │  ├── match: team=ml-training                               │         │  │
│  │  │  │   └── receiver: pagerduty-ml-oncall                    │         │  │
│  │  │  └── default: receiver: pagerduty-infra-oncall            │         │  │
│  │  │                                                            │         │  │
│  │  │  match: severity=warning                                   │         │  │
│  │  │  └── receiver: slack-team-channel                         │         │  │
│  │  │                                                            │         │  │
│  │  │  match: severity=info                                      │         │  │
│  │  │  └── receiver: slack-observability                        │         │  │
│  │  └──────────────────────────────────────────────────────────┘         │  │
│  └───────────────────────────────────────────────────────────────────────┘  │
│                                                                             │
└───────────────────┬─────────────────────┬──────────────────┬────────────────┘
                    │                     │                  │
                    ▼                     ▼                  ▼
┌──────────────────────┐ ┌───────────────────────┐ ┌──────────────────────────┐
│ PagerDuty            │ │ Slack                  │ │ Alert History Store      │
│                      │ │                        │ │                          │
│ - Phone call/SMS     │ │ - Channel notification │ │ - PostgreSQL or ES       │
│ - Push notification  │ │ - Thread per alert     │ │ - Full alert lifecycle   │
│ - Escalation policy  │ │   group                │ │ - Analytics dashboard    │
│ - On-call schedule   │ │ - Acknowledge button   │ │                          │
│ - Incident tracking  │ │ - Runbook link         │ │                          │
└──────────────────────┘ └───────────────────────┘ └──────────────────────────┘
```

### Component Roles

| Component | Role |
|---|---|
| **Prometheus** | Evaluates alert rules (PromQL expressions) every 15 seconds. When a rule condition is true for the configured `for` duration, it fires the alert to Alertmanager. |
| **ElastAlert** | Evaluates log-based alert rules against Elasticsearch. Sends alerts to Alertmanager via webhook. |
| **Blackbox Exporter** | Probes external endpoints (HTTP, TCP, ICMP, DNS). Prometheus scrapes probe results and evaluates alert rules. |
| **Alertmanager (cluster)** | Central alert routing engine. Receives alerts from all sources. Deduplicates, groups, inhibits, silences, and routes to receivers. Runs as a 3-node cluster with gossip protocol for HA. |
| **PagerDuty** | Notification delivery (phone, SMS, push), escalation policies, on-call schedule management. |
| **Slack** | Low-severity notifications, team awareness, interactive alert management (acknowledge, silence). |
| **Alert History Store** | Persists all alert events for analysis, trend reporting, and SLO tracking. |

### Data Flows

1. **Alert evaluation**: Prometheus evaluates `expr` → condition true for `for` duration → state transitions from `inactive` → `pending` → `firing` → sends to Alertmanager.
2. **Routing**: Alertmanager receives alert → checks inhibition rules → checks silence rules → matches routing tree → groups with similar alerts → waits `group_wait` → sends to receiver.
3. **Notification**: Alertmanager → PagerDuty API (creates incident) → PagerDuty escalation → phone call to on-call.
4. **Resolution**: Alert condition becomes false → Prometheus sends resolved notification → Alertmanager routes resolve → PagerDuty auto-resolves incident.

---

## 4. Data Model

### Core Entities & Schema

**Prometheus Alert Rule:**
```yaml
groups:
  - name: job-scheduler-alerts
    rules:
      - alert: JobSchedulerHighErrorRate
        expr: |
          sum(rate(http_server_requests_seconds_count{service="job-scheduler", status=~"5.."}[5m]))
          /
          sum(rate(http_server_requests_seconds_count{service="job-scheduler"}[5m]))
          > 0.05
        for: 5m
        labels:
          severity: critical
          team: platform
          service: job-scheduler
        annotations:
          summary: "Job Scheduler error rate is {{ $value | humanizePercentage }} (threshold: 5%)"
          description: "The job-scheduler service has an error rate exceeding 5% for the past 5 minutes."
          runbook_url: "https://runbooks.infra.internal/job-scheduler/high-error-rate"
          dashboard_url: "https://grafana.infra.internal/d/job-scheduler"
          impact: "Users cannot submit new jobs. Existing jobs are unaffected."
```

**Alertmanager Configuration:**
```yaml
global:
  resolve_timeout: 5m
  pagerduty_url: 'https://events.pagerduty.com/v2/enqueue'
  slack_api_url: 'https://hooks.slack.com/services/...'

inhibit_rules:
  # If cluster is down, suppress all service alerts in that cluster
  - source_matchers:
      - alertname = KubernetesClusterDown
    target_matchers:
      - severity =~ "warning|critical"
    equal: ['cluster']

  # If node is down, suppress all pod alerts on that node
  - source_matchers:
      - alertname = NodeDown
    target_matchers:
      - severity =~ "warning|critical"
    equal: ['node']

route:
  group_by: ['alertname', 'cluster', 'service']
  group_wait: 30s           # Wait before sending first notification for a new group
  group_interval: 5m         # Wait before sending updates for an existing group
  repeat_interval: 4h        # Resend if alert still firing after this
  receiver: default-slack

  routes:
    # Critical alerts → PagerDuty
    - matchers:
        - severity = critical
      receiver: pagerduty-platform
      continue: true          # Also send to Slack
      routes:
        - matchers:
            - team = ml-training
          receiver: pagerduty-ml
        - matchers:
            - team = data-platform
          receiver: pagerduty-data

    # Warning alerts → Slack only
    - matchers:
        - severity = warning
      receiver: slack-warnings
      group_wait: 60s
      repeat_interval: 12h

    # Dead man's switch → always firing
    - matchers:
        - alertname = DeadMansSwitch
      receiver: deadmansswitch-webhook
      repeat_interval: 1m

receivers:
  - name: pagerduty-platform
    pagerduty_configs:
      - service_key: '<platform-team-service-key>'
        severity: '{{ .GroupLabels.severity }}'
        description: '{{ .GroupLabels.alertname }}: {{ .CommonAnnotations.summary }}'
        details:
          firing: '{{ .Alerts.Firing | len }}'
          resolved: '{{ .Alerts.Resolved | len }}'
          runbook: '{{ (index .Alerts 0).Annotations.runbook_url }}'

  - name: slack-warnings
    slack_configs:
      - channel: '#infra-alerts'
        title: '{{ .GroupLabels.alertname }} [{{ .Status | toUpper }}]'
        text: >-
          {{ range .Alerts }}
          *{{ .Annotations.summary }}*
          Service: {{ .Labels.service }} | Cluster: {{ .Labels.cluster }}
          Runbook: {{ .Annotations.runbook_url }}
          {{ end }}
        send_resolved: true

  - name: deadmansswitch-webhook
    webhook_configs:
      - url: 'https://deadmansswitch.infra.internal/heartbeat'
```

**Alert Event (History Store):**
```json
{
  "alert_id": "a8f3d2e1-...",
  "alertname": "JobSchedulerHighErrorRate",
  "service": "job-scheduler",
  "cluster": "k8s-prod-us-east-1",
  "severity": "critical",
  "team": "platform",
  "state": "firing",
  "fired_at": "2026-04-09T14:20:00Z",
  "resolved_at": null,
  "acknowledged_at": "2026-04-09T14:22:15Z",
  "acknowledged_by": "engineer@company.com",
  "value": 0.087,
  "summary": "Job Scheduler error rate is 8.7% (threshold: 5%)",
  "runbook_url": "https://runbooks.infra.internal/job-scheduler/high-error-rate",
  "pagerduty_incident_id": "PD-12345",
  "notifications_sent": [
    {"channel": "pagerduty", "at": "2026-04-09T14:20:35Z", "to": "primary-oncall"},
    {"channel": "slack", "at": "2026-04-09T14:20:36Z", "to": "#infra-alerts"}
  ]
}
```

### Database Selection

| Storage | Technology | Rationale |
|---|---|---|
| **Alertmanager state** | In-memory + WAL (built-in) | Silences, notification log (nflog) for dedup. Gossip replicates to HA peers. |
| **Alert rules** | YAML in Git, deployed via CI/CD | Version-controlled, code review, rollback capability |
| **Alert history** | PostgreSQL or Elasticsearch | Queryable history for analytics, trend reporting. ES for full-text search on annotations. |
| **On-call schedules** | PagerDuty (SaaS) | Mature schedule management, mobile app, escalation engine. Not worth building in-house. |

### Indexing Strategy

Alert history in Elasticsearch:
- Index: `alerts-YYYY-MM` (monthly, low volume)
- Key fields: `alertname` (keyword), `service` (keyword), `severity` (keyword), `state` (keyword), `fired_at` (date), `resolved_at` (date), `team` (keyword)
- Useful queries: "all critical alerts for job-scheduler in the last 30 days", "MTTR by team", "most frequently firing alerts"

---

## 5. API Design

### Query APIs

**List Active Alerts**
```
GET /api/v2/alerts?filter=severity="critical"&filter=team="platform"&active=true

Response:
[
  {
    "labels": {"alertname": "JobSchedulerHighErrorRate", "severity": "critical", ...},
    "annotations": {"summary": "...", "runbook_url": "..."},
    "startsAt": "2026-04-09T14:20:00Z",
    "endsAt": "0001-01-01T00:00:00Z",
    "status": {"state": "active", "silencedBy": [], "inhibitedBy": []}
  }
]
```

**List Silences**
```
GET /api/v2/silences

Response:
[
  {
    "id": "silence-123",
    "matchers": [{"name": "cluster", "value": "k8s-staging-1", "isRegex": false}],
    "startsAt": "2026-04-09T10:00:00Z",
    "endsAt": "2026-04-09T14:00:00Z",
    "createdBy": "engineer@company.com",
    "comment": "Cluster upgrade maintenance window"
  }
]
```

**Alert History (Custom API)**
```
GET /api/v1/alert-history?alertname=JobSchedulerHighErrorRate&from=2026-04-01&to=2026-04-09

Response:
{
  "alerts": [...],
  "statistics": {
    "total_firings": 12,
    "avg_duration_minutes": 23,
    "mttr_minutes": 15,
    "false_positive_count": 2
  }
}
```

### Ingestion APIs

**Alertmanager Alert Receiver (from Prometheus)**
```
POST /api/v2/alerts
Content-Type: application/json

[
  {
    "labels": {
      "alertname": "JobSchedulerHighErrorRate",
      "severity": "critical",
      "service": "job-scheduler",
      "cluster": "k8s-prod-us-east-1"
    },
    "annotations": {
      "summary": "Error rate 8.7%",
      "runbook_url": "https://..."
    },
    "startsAt": "2026-04-09T14:20:00Z",
    "generatorURL": "https://prometheus.infra.internal/graph?..."
  }
]
```

### Admin APIs

```
# Create Silence
POST /api/v2/silences
{
  "matchers": [{"name": "cluster", "value": "k8s-staging-1"}],
  "startsAt": "2026-04-09T10:00:00Z",
  "endsAt": "2026-04-09T14:00:00Z",
  "createdBy": "engineer@company.com",
  "comment": "Cluster upgrade"
}

# Delete Silence
DELETE /api/v2/silence/{silenceId}

# Reload Configuration
POST /-/reload

# Cluster Status
GET /api/v2/status
Response: { "cluster": {"status": "ready", "peers": [...]} }
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: Multi-Window Multi-Burn-Rate SLO Alerting

**Why it's hard:**
Traditional threshold-based alerts ("error rate > 5%") are either too noisy (fire on brief spikes) or too slow (miss gradual degradation). SLO-based alerting with burn rates detects both fast and slow error budget burn, aligning alerts with business impact rather than arbitrary thresholds. The math is non-trivial and the configuration is complex.

**Approaches:**

| Approach | Pros | Cons |
|---|---|---|
| **Simple threshold** | Easy to understand and configure | Too noisy (brief spikes) or too slow (misses gradual burn) |
| **For-duration threshold** | `for: 5m` reduces noise | Still misses slow burns; 5-minute delay for fast burns |
| **Single-window burn rate** | Aligns with SLO budget | Single window misses either fast or slow burns |
| **Multi-window multi-burn-rate** | Catches both fast and slow burns, reduces false positives | Complex to configure, requires understanding of error budget math |

**Selected approach: Multi-window multi-burn-rate (Google SRE methodology)**

**Concept:**
Given an SLO of 99.9% availability over 30 days:
- Error budget = 0.1% x 30 days = 43.2 minutes of downtime.
- A "burn rate" of 1x means consuming the error budget exactly over 30 days.
- A burn rate of 14.4x means consuming the entire 30-day error budget in 2 days.
- A burn rate of 6x means consuming it in 5 days.

**Alert windows:**

| Burn Rate | Long Window | Short Window | Severity | Budget Consumed |
|---|---|---|---|---|
| 14.4x | 1 hour | 5 minutes | Critical (page) | 2% in 1 hour |
| 6x | 6 hours | 30 minutes | Critical (page) | 5% in 6 hours |
| 3x | 1 day | 2 hours | Warning (ticket) | 10% in 1 day |
| 1x | 3 days | 6 hours | Info (dashboard) | 10% in 3 days |

**Why two windows?**
- Long window (1 hour): catches sustained errors but might fire on errors that already resolved.
- Short window (5 minutes): confirms the error is still ongoing right now.
- Both must be true for the alert to fire. This dramatically reduces false positives.

**Implementation:**

```yaml
# Recording rules (pre-compute error ratios for different windows)
groups:
  - name: slo-recording-rules
    rules:
      # Error ratio over different windows
      - record: slo:http_error_ratio:rate5m
        expr: |
          sum(rate(http_server_requests_seconds_count{status=~"5.."}[5m])) by (service)
          /
          sum(rate(http_server_requests_seconds_count[5m])) by (service)

      - record: slo:http_error_ratio:rate30m
        expr: |
          sum(rate(http_server_requests_seconds_count{status=~"5.."}[30m])) by (service)
          /
          sum(rate(http_server_requests_seconds_count[30m])) by (service)

      - record: slo:http_error_ratio:rate1h
        expr: |
          sum(rate(http_server_requests_seconds_count{status=~"5.."}[1h])) by (service)
          /
          sum(rate(http_server_requests_seconds_count[1h])) by (service)

      - record: slo:http_error_ratio:rate6h
        expr: |
          sum(rate(http_server_requests_seconds_count{status=~"5.."}[6h])) by (service)
          /
          sum(rate(http_server_requests_seconds_count[6h])) by (service)

      - record: slo:http_error_ratio:rate1d
        expr: |
          sum(rate(http_server_requests_seconds_count{status=~"5.."}[1d])) by (service)
          /
          sum(rate(http_server_requests_seconds_count[1d])) by (service)

      - record: slo:http_error_ratio:rate3d
        expr: |
          sum(rate(http_server_requests_seconds_count{status=~"5.."}[3d])) by (service)
          /
          sum(rate(http_server_requests_seconds_count[3d])) by (service)

# Alert rules using multi-window multi-burn-rate
  - name: slo-alerts
    rules:
      # 14.4x burn rate: pages in 2% error budget consumed in 1 hour
      - alert: SLOHighBurnRate_Critical
        expr: |
          slo:http_error_ratio:rate1h{service="job-scheduler"} > (14.4 * 0.001)
          and
          slo:http_error_ratio:rate5m{service="job-scheduler"} > (14.4 * 0.001)
        for: 2m
        labels:
          severity: critical
          slo: "availability"
          burn_rate: "14.4x"
        annotations:
          summary: "{{ $labels.service }} is burning SLO budget at 14.4x rate"
          description: "At this rate, the 30-day error budget will be exhausted in ~2 days."
          runbook_url: "https://runbooks.infra.internal/slo/high-burn-rate"

      # 6x burn rate: pages in 5% error budget consumed in 6 hours
      - alert: SLOHighBurnRate_Warning
        expr: |
          slo:http_error_ratio:rate6h{service="job-scheduler"} > (6 * 0.001)
          and
          slo:http_error_ratio:rate30m{service="job-scheduler"} > (6 * 0.001)
        for: 5m
        labels:
          severity: critical
          slo: "availability"
          burn_rate: "6x"
        annotations:
          summary: "{{ $labels.service }} is burning SLO budget at 6x rate"

      # 3x burn rate: ticket
      - alert: SLOMediumBurnRate
        expr: |
          slo:http_error_ratio:rate1d{service="job-scheduler"} > (3 * 0.001)
          and
          slo:http_error_ratio:rate2h{service="job-scheduler"} > (3 * 0.001)
        for: 15m
        labels:
          severity: warning
          slo: "availability"
          burn_rate: "3x"

      # 1x burn rate: informational
      - alert: SLOSlowBurnRate
        expr: |
          slo:http_error_ratio:rate3d{service="job-scheduler"} > (1 * 0.001)
          and
          slo:http_error_ratio:rate6h{service="job-scheduler"} > (1 * 0.001)
        for: 30m
        labels:
          severity: info
          slo: "availability"
          burn_rate: "1x"
```

**Failure Modes:**
- **SLO definition drift**: The SLO (99.9%) is hardcoded in alert rules. If the team changes their SLO, all alert rules must be updated. Solution: use Sloth or similar SLO generators that produce Prometheus rules from SLO definitions.
- **Error budget exhaustion**: Once the 30-day error budget is consumed (43.2 minutes of downtime), any further errors immediately fire alerts. This is by design but can cause alert fatigue if the budget was consumed early in the period.
- **Metric label changes**: If the service renames or restructures its metrics, SLO rules break silently (return no data). Solution: unit tests for recording rules using `promtool test rules`.

**Interviewer Q&As:**

**Q1: Explain why you need both a long window and a short window.**
A: The long window (e.g., 1 hour) measures sustained error rate -- it catches real problems. But it has a failure mode: if errors spiked 50 minutes ago and have since resolved, the 1-hour window still shows a high error rate. The short window (5 minutes) confirms the error is happening right now. By requiring both: long window catches the trend, short window confirms it is current. This eliminates alerts for brief resolved spikes while still catching ongoing issues.

**Q2: How do you handle different SLOs for different services?**
A: Each service defines its SLO in a YAML file (e.g., `slo.yaml` in the service repo). A CI/CD pipeline (using Sloth or a custom generator) converts SLO definitions into Prometheus recording rules and alert rules. The generator parameterizes the burn rate thresholds by the SLO target. A 99.99% SLO generates stricter alerts (lower error tolerance) than a 99.9% SLO. All generated rules are version-controlled and deployed via Prometheus rule reload.

**Q3: What is the error budget and how do you track it?**
A: Error budget = `1 - SLO_target` expressed as allowed downtime over a period. For 99.9% over 30 days: `0.001 x 30 x 24 x 60 = 43.2 minutes`. We track remaining error budget on a Grafana dashboard: `error_budget_remaining = 1 - (sum(errors_over_30d) / (slo_target_ratio * total_requests_over_30d))`. When budget is < 20%, we freeze non-essential deploys. When budget is 0%, all deploys require VP approval.

**Q4: How do burn-rate alerts compare to simple threshold alerts?**
A: Comparison for a 99.9% SLO: Simple threshold `error_rate > 0.1%` fires on any brief spike, generating noise. `error_rate > 0.1% for 5m` misses slow accumulation. Burn rate 14.4x (error_rate > 1.44% over 1h AND 5m) catches fast burns quickly. Burn rate 1x (error_rate > 0.1% over 3d AND 6h) catches slow burns. The burn-rate approach is directly tied to business impact (SLO budget) rather than an arbitrary threshold.

**Q5: How do you handle SLO alerting for services with very low traffic?**
A: Low-traffic services (< 100 requests/min) have high variance in error rates. A single error out of 10 requests = 10% error rate. Solutions: (1) Use `for: 15m` or longer to require sustained errors. (2) Set minimum request count threshold: only evaluate SLO when `rate(requests[5m]) > 10/60`. (3) Use error count instead of error ratio for low-traffic services: "alert when > 5 errors in 1 hour" rather than "> 0.1% error rate."

**Q6: How do you test SLO alert rules before deploying to production?**
A: (1) `promtool test rules` with synthetic test cases that verify rules fire at expected thresholds. (2) Run rules in "dry-run" mode: add `severity: test` label that routes to a test Slack channel instead of PagerDuty. (3) Use Prometheus recording rules to compute the burn rate and display on a dashboard before enabling alerts. (4) Backtest: replay historical Prometheus data through the new rules to check if they would have fired on past incidents (and not fired on non-incidents).

---

### Deep Dive 2: Alert Fatigue Management

**Why it's hard:**
Alert fatigue is the #1 killer of on-call effectiveness. When on-call engineers receive too many alerts (especially false positives and non-actionable alerts), they start ignoring all alerts -- including critical ones. A single missed critical alert can cause a major outage. The challenge is balancing sensitivity (catching real problems) with specificity (not crying wolf).

**Approaches:**

| Approach | Mechanism | Impact |
|---|---|---|
| **Grouping** | Combine related alerts into single notification | Reduces notification volume during storms |
| **Inhibition** | Suppress symptoms when root cause is firing | Eliminates redundant alerts |
| **Silencing** | Mute alerts during known maintenance | Prevents expected noise |
| **For-duration** | Require condition to persist before firing | Eliminates transient spikes |
| **Deduplication** | Same alert only notifies once until resolved | Prevents repeat notifications |
| **Severity tiering** | Only page for critical; Slack for warning | Reduces interrupt-driven fatigue |
| **Alert quality metrics** | Track and improve alert signal-to-noise ratio | Systematic improvement over time |

**Selected approach: All of the above, systematically applied**

**Alert Quality Metrics (tracked monthly per team):**

| Metric | Target | Formula |
|---|---|---|
| **Actionability rate** | > 80% | Alerts that required human action / total alerts |
| **False positive rate** | < 5% | Alerts that auto-resolved within 5 min / total alerts |
| **MTTA (Mean Time to Acknowledge)** | < 5 min for critical | avg(acknowledged_at - fired_at) |
| **MTTR (Mean Time to Resolve)** | < 30 min for critical | avg(resolved_at - fired_at) |
| **Alerts per on-call shift** | < 10 | total alerts / number of on-call shifts |
| **Repeat offender rate** | < 20% | Alerts with same alertname firing > 5x/month |

**Implementation: Alert Lifecycle Dashboard**
```promql
# Alert quality metrics (computed from alert history store)

# Actionability rate
actionable_alerts / total_alerts

# Flapping alerts (fire and resolve within 5 minutes)
count(alert_history{state="resolved", duration_minutes < 5}) / count(alert_history)

# Repeat offenders (same alert fires > 5 times in 30 days)
count by (alertname) (alert_history{state="firing"}) > 5

# On-call burden (alerts per shift)
count(alert_history{state="firing", severity="critical"}) / count(on_call_shifts)
```

**Alertmanager Grouping Configuration:**
```yaml
# Group by alertname + cluster + service
# This means: if 50 pods of the same service in the same cluster
# trigger the same alert, the on-call gets ONE notification listing all 50.

route:
  group_by: ['alertname', 'cluster', 'service']
  group_wait: 30s      # Wait 30s after first alert in group before notifying
  group_interval: 5m    # Wait 5m before sending updates to the group
  repeat_interval: 4h   # Resend every 4h if still firing
```

**Inhibition Examples:**
```yaml
inhibit_rules:
  # Cluster down → suppress all alerts in that cluster
  - source_matchers: [alertname = KubernetesClusterDown]
    target_matchers: [severity =~ "warning|critical"]
    equal: [cluster]

  # Datacenter network down → suppress all host alerts in that DC
  - source_matchers: [alertname = DatacenterNetworkDown]
    target_matchers: [severity =~ "warning|critical"]
    equal: [datacenter]

  # Critical alert firing → suppress corresponding warning
  - source_matchers: [severity = critical]
    target_matchers: [severity = warning]
    equal: [alertname, service]
```

**Failure Modes:**
- **Over-inhibition**: A misconfigured inhibition rule suppresses legitimate alerts. Monitor `alertmanager_alerts_inhibited_total` and audit inhibition logs.
- **Grouping too aggressive**: Grouping by `alertname` only (not `cluster`) may combine alerts from different clusters into one notification, hiding the scope of the issue. Balance grouping granularity.
- **Silence forgotten**: An engineer creates a 24h silence for maintenance but forgets to remove it. Solution: all silences require a comment, max duration of 24h, and auto-expire. Slack bot sends daily summary of active silences.

**Interviewer Q&As:**

**Q1: An on-call engineer received 150 alerts last night. How do you fix this?**
A: (1) Analyze the 150 alerts: categorize by alertname, severity, actionability. (2) Identify top offenders: likely 3-5 alert rules generating 80% of the noise. (3) For each offending alert: Was it actionable? If not, delete the rule or convert to warning. Was it a symptom of a root cause? Add inhibition. Was it flapping? Increase `for` duration. Was it an alert storm? Improve grouping. (4) Set up a monthly alert quality review meeting. (5) Target: < 2 pages per on-call shift. (6) Common fixes: increase `for` duration from 0 to 5m (eliminates transient spikes), add `rate()` smoothing over 5m instead of instant value, add inhibition rules.

**Q2: How do you distinguish between a critical alert and a warning?**
A: Severity definitions tied to impact: **Critical (page)**: customer-facing impact, data loss risk, or security breach. Requires immediate action (within 15 minutes). Examples: service down, data pipeline stopped, SLO burn rate > 14x. **Warning (Slack/ticket)**: potential problem that could become critical if not addressed within hours. No immediate customer impact. Examples: disk 80% full, certificate expiring in 7 days, SLO slow burn. **Info (dashboard only)**: informational, no action needed. Examples: new deployment detected, traffic above normal.

**Q3: What is a dead man's switch and why is it critical?**
A: A dead man's switch is an alert that is always firing. The monitoring system sends a heartbeat (the alert) to an external service (e.g., Healthchecks.io, Cronitor, or our custom endpoint) every 1 minute. If the heartbeat stops (because Prometheus or Alertmanager is down), the external service alerts via a separate path (direct PagerDuty API call, not through Alertmanager). This answers: "Who monitors the monitoring?" Implementation: `alert: DeadMansSwitch; expr: vector(1)` -- this always evaluates to 1, always fires. Alertmanager sends it to the heartbeat webhook every minute.

**Q4: How do you handle alert storms during cascading failures?**
A: (1) **Grouping**: Alertmanager groups related alerts. 500 host alerts grouped by `alertname + datacenter` → 1 notification per DC. (2) **Inhibition**: Root cause alert (e.g., network switch down) inhibits all downstream alerts (host unreachable, service unhealthy). (3) **Automatic silence**: If > 100 alerts fire within 5 minutes for the same alertname, auto-create a silence and notify the on-call with "alert storm detected, auto-silenced, investigate root cause." (4) **War room escalation**: If > 50 critical alerts fire simultaneously, automatically create an incident and page the incident commander.

**Q5: How do you prevent silences from being used to hide real problems?**
A: (1) All silences require a comment explaining the reason. (2) Max silence duration is 24 hours; longer requires manager approval. (3) Daily Slack bot posts: "Active silences: [list]" to the team channel. (4) Monthly audit of silence history: if a team silences the same alert repeatedly, it indicates a chronic problem that needs fixing. (5) Silences are logged to the audit system with the creator's identity. (6) Dashboard shows "Silenced alerts" count -- non-zero means someone has intentionally muted monitoring.

**Q6: How do you handle alerts for services in different time zones?**
A: Alertmanager's routing can include time-based matching, but it is simpler to handle at the PagerDuty/OpsGenie layer. PagerDuty supports follow-the-sun on-call schedules: US hours → US team, APAC hours → APAC team, EU hours → EU team. The Alertmanager routing is by `team` label; PagerDuty handles the timezone-based escalation. If a team has no 24/7 coverage, critical alerts escalate to the global SRE team after hours.

---

### Deep Dive 3: Alertmanager High Availability and Gossip Protocol

**Why it's hard:**
Alertmanager is a critical system: if it goes down, no alerts are delivered, and outages go unnoticed. Running Alertmanager in HA mode requires multiple instances to coordinate on deduplication (prevent duplicate pages), silence state (a silence created on instance A must apply to instance B), and notification state (which alerts have already been notified).

**Alertmanager HA Architecture:**
```
┌──────────────────┐    ┌──────────────────┐    ┌──────────────────┐
│  Alertmanager 1  │◄──►│  Alertmanager 2  │◄──►│  Alertmanager 3  │
│  (active)        │    │  (active)        │    │  (active)        │
│                  │    │                  │    │                  │
│  nflog + silences│    │  nflog + silences│    │  nflog + silences│
│  (gossip sync)   │    │  (gossip sync)   │    │  (gossip sync)   │
└──────────────────┘    └──────────────────┘    └──────────────────┘
         ▲                       ▲                       ▲
         │                       │                       │
    All Prometheus instances send to ALL Alertmanager instances
```

**How it works:**
1. Each Prometheus instance sends alerts to all 3 Alertmanager instances (configured in `alertmanagers` list).
2. All 3 instances receive the same alerts.
3. **Deduplication via gossip**: Alertmanager uses a gossip protocol (Hashicorp Memberlist) to synchronize the notification log (nflog). When instance 1 sends a PagerDuty notification, it writes to its nflog and gossips this to instances 2 and 3. Instances 2 and 3 see the notification was already sent and skip it.
4. **Silences**: When an engineer creates a silence on instance 1, it is gossip-replicated to instances 2 and 3 within seconds.
5. **Consistency model**: Eventually consistent. There is a small window (milliseconds) where two instances might both send a notification before gossip propagates. This is acceptable: a duplicate PagerDuty page is better than a missed page.

**Failure Modes:**
- **Network partition between AM instances**: Each partition sends its own notification → duplicate pages. Acceptable trade-off (availability over consistency).
- **All AM instances down**: No alerts delivered. Detected by dead man's switch. Mitigated by running on separate hosts/racks.
- **Gossip storm**: With many silences or high alert volume, gossip traffic can increase. Monitor `alertmanager_cluster_messages_received_total`.
- **nflog corruption**: Notification log is in-memory with periodic WAL snapshots. On crash, some recent notifications may be resent. PagerDuty deduplicates by `dedup_key`.

**Interviewer Q&As:**

**Q1: Why does Alertmanager use gossip instead of a consensus protocol (Raft)?**
A: Alertmanager prioritizes availability over consistency. If one instance is partitioned, it should still send notifications (even if duplicate) rather than block. Raft requires a majority quorum; in a 3-node cluster, losing 2 nodes would stop all notifications. With gossip and independent operation, each instance can function alone. PagerDuty provides deduplication at the receiver side, making duplicate notifications a minor nuisance rather than a problem.

**Q2: How does Alertmanager dedup key work?**
A: The dedup key is derived from the alert's label set (all labels combined as a hash). When Alertmanager sends a notification for an alert group, it records the dedup key in the notification log (nflog) with a timestamp. On the next evaluation, if the same dedup key is in the nflog within the `repeat_interval`, the notification is skipped. Gossip replicates the nflog entry to other instances. PagerDuty also deduplicates using a `dedup_key` in the API payload (set to the alert group fingerprint).

**Q3: How do you upgrade Alertmanager without missing alerts?**
A: Rolling upgrade: (1) Remove one instance from the load balancer (Prometheus still sends to the other 2). (2) Upgrade and restart the instance. (3) Wait for gossip to sync (check `alertmanager_cluster_peers` metric). (4) Add back to load balancer. (5) Repeat for next instance. During the upgrade, at least 2 instances are always running. Configuration changes use hot-reload (`POST /-/reload`) which does not require restart.

**Q4: How do you test Alertmanager configuration changes?**
A: (1) `amtool check-config alertmanager.yml` validates syntax and routing tree. (2) `amtool config routes test --config.file=alertmanager.yml --tree` shows which route a test alert would match. (3) Deploy to staging Alertmanager (separate cluster) and send test alerts. (4) Use `amtool alert add` to send synthetic alerts and verify routing. (5) Routing tree visualization: `amtool config routes show` displays the full tree.

---

### Deep Dive 4: On-Call Schedule and Escalation Design

**Why it's hard:**
On-call is a sociotechnical problem. Poorly designed on-call leads to burnout, slow response, and attrition. The system must balance engineering load, ensure coverage 24/7, handle escalation when the primary does not respond, and integrate with the alerting pipeline.

**On-Call Schedule Design:**

```
┌─────────────────────────────────────────────────────────────────┐
│                    ON-CALL ROTATION                               │
│                                                                   │
│  Primary:    Engineer A → Engineer B → Engineer C → ...           │
│              (1-week rotation, Monday 09:00 handoff)              │
│                                                                   │
│  Secondary:  Engineer B → Engineer C → Engineer D → ...           │
│              (backup if primary does not ACK in 5 min)            │
│                                                                   │
│  Manager:    Team Lead → Engineering Manager                      │
│              (escalation if secondary does not ACK in 10 min)     │
│                                                                   │
│  Follow-the-sun (for global teams):                               │
│  US Hours (09:00-18:00 PT):  US-based engineers                  │
│  APAC Hours (09:00-18:00 JST): APAC-based engineers              │
│  EU Hours (09:00-18:00 CET): EU-based engineers                  │
│  After-hours: Global SRE team (24/7 coverage)                     │
└─────────────────────────────────────────────────────────────────┘
```

**Escalation Policy:**
```
Level 1 (T+0):    Page primary on-call via PagerDuty
                   ├── Phone call
                   ├── Push notification
                   └── SMS

Level 2 (T+5min):  If not acknowledged → page secondary on-call
                   ├── Phone call
                   └── Push notification

Level 3 (T+10min): If not acknowledged → page team manager + Slack #incident

Level 4 (T+15min): If not acknowledged → page VP Engineering + incident bridge
```

**On-Call Handoff Procedure:**
1. Outgoing on-call posts handoff notes in Slack: active issues, silences, upcoming maintenance.
2. Incoming on-call reviews notes and acknowledges readiness.
3. PagerDuty schedule automatically rotates at 09:00 Monday.
4. Bot sends: "On-call handoff: @engineer_a → @engineer_b for platform-team. Active alerts: 2."

**Failure Modes:**
- **On-call unreachable**: Phone off, battery dead, no signal. Escalation policy ensures secondary and manager are notified within 10 minutes.
- **All on-call unreachable**: After level 4 escalation, an automated incident is created in the incident management system and broadcast to the team Slack channel.
- **On-call burnout**: More than 10 pages per shift → systematic alert quality improvement. Track "on-call burden" metric weekly.

**Interviewer Q&As:**

**Q1: How do you reduce on-call burden?**
A: (1) Fix the root cause: every alert should have a follow-up action (runbook or ticket). If the same alert fires repeatedly, fix the underlying issue. (2) Improve alert quality: reduce false positives via better thresholds, sampling, and burn-rate alerting. (3) Automate responses: if a runbook has clear steps, automate them (self-healing). (4) Balance load: ensure on-call rotation is fair (no one engineer on-call every other week). (5) Track and review: monthly on-call report showing pages per shift, MTTA, MTTR. (6) Compensate: on-call stipend or time-off after heavy shifts.

**Q2: How do you design runbooks that are useful during an incident?**
A: Runbook structure: (1) **What this alert means** (1 sentence). (2) **Customer impact** (who is affected, how). (3) **Immediate actions** (step-by-step, with copy-pasteable commands). (4) **Investigation steps** (what to check, in what order, with dashboard links). (5) **Escalation criteria** (when to escalate vs. handle yourself). (6) **Related alerts** (what else to look at). (7) **Past incidents** (links to postmortems for similar issues). Anti-patterns: runbooks that say "investigate and fix" without specific steps, or runbooks that are 10 pages long. Keep it to 1-2 screens of actionable content.

**Q3: How do you handle the first 5 minutes of an incident?**
A: The "first 5 minutes" playbook: (1) **Acknowledge** the PagerDuty alert (stops escalation timer). (2) **Assess** severity: check the alert's impact annotation and dashboard link. (3) **Communicate**: post in #incidents Slack channel: "Investigating [alert name]. Impact: [summary]. Owner: @me." (4) **Follow the runbook**: execute the documented steps for this alert. (5) **Escalate if needed**: if the issue is beyond your expertise or requires broader coordination, escalate to the incident commander. The goal: within 5 minutes, the team knows someone is on it and what the initial impact is.

**Q4: How do you handle conflicting on-call schedules (holiday, sick leave)?**
A: PagerDuty supports schedule overrides: an engineer can transfer their on-call shift to a teammate via the UI or Slack bot. For holidays: teams submit override requests 2 weeks in advance. For sick leave: the secondary automatically becomes primary; the team lead arranges a replacement secondary. For company-wide holidays: skeleton crew rotation with extra compensation. PagerDuty's "on-call readiness" report shows upcoming gaps in coverage.

---

## 7. Scaling Strategy

**Interviewer Q&As:**

**Q1: How do you handle 10,000 alert rules across 200 Prometheus instances?**
A: Each Prometheus instance evaluates only the rules relevant to its scrape scope. A cluster-level Prometheus evaluates ~50 rules. Total evaluation load is distributed across 200 instances. Rule evaluation is CPU-intensive: Prometheus evaluates each rule every 15s. At 50 rules per instance, this takes ~200ms total (well within the 15s budget). Recording rules pre-compute expensive sub-expressions, reducing the cost of alert rule evaluation.

**Q2: How do you prevent Alertmanager from being overwhelmed during an alert storm?**
A: Alertmanager is lightweight and handles thousands of alerts efficiently. The main risk is notification receiver overload (PagerDuty API rate limiting). Mitigations: (1) Grouping: 5000 alerts → ~50 groups → 50 notifications. (2) Rate limiting on receivers: Alertmanager's `max_alerts` per notification limits the payload size. (3) PagerDuty's API handles batching. (4) For extreme storms, the automatic silence mechanism kicks in.

**Q3: How do you scale alert rule management for 100 teams?**
A: GitOps approach: each team owns their alert rules in their service repository's `monitoring/` directory. A CI/CD pipeline validates rules (`promtool check rules`), tests them (`promtool test rules`), and deploys them to the appropriate Prometheus instance via ConfigMap reload. The platform team owns common infrastructure rules (Node Exporter, kube-state-metrics) deployed centrally. Teams have autonomy over their service-specific alerts within guardrails (must have severity, team, runbook_url labels).

**Q4: How do you handle alert rules for dynamically created resources (e.g., new K8s namespaces)?**
A: Use PromQL label matchers with regex or templating. Example: `rate(http_requests_total{namespace=~".+"}[5m]) == 0 and ON(namespace) kube_namespace_status_phase{phase="Active"}` alerts for any namespace with zero traffic. Rules are written generically using label matching, not hardcoded resource names. When a new namespace is created, existing rules automatically apply to it.

**Q5: How do you handle alerting for multi-region deployments?**
A: Each region has its own Alertmanager cluster (HA triplet). This ensures alerts fire even during cross-region network partitions. For global alerts (e.g., "total cross-region error rate > threshold"), we use Thanos global recording rules evaluated on the global Thanos Ruler, which sends alerts to the global Alertmanager cluster.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure | Impact | Detection | Mitigation | RTO |
|---|---|---|---|---|
| **Single Alertmanager down** | No impact (gossip, HA) | Peer count metric, health check | 3-instance cluster, gossip sync | 0 (transparent) |
| **All Alertmanagers down** | No alert notifications | Dead man's switch heartbeat stops → external service pages | Dead man's switch, separate notification path | ~5 min |
| **Prometheus down (one instance)** | Alert rules not evaluated for that cluster | Meta-Prometheus detects HA peer unhealthy | HA pair per cluster; one instance continues | 0 (transparent) |
| **PagerDuty outage** | Pages not delivered despite Alertmanager firing | PagerDuty status page, delivery failure webhook | Fallback to Slack, email, OpsGenie secondary | Depends on PD |
| **Network partition (Prometheus ↔ AM)** | Alerts not delivered from affected Prometheus | `alertmanager_notifications_failed_total` | Prometheus sends to all AM instances; LB handles routing | Self-healing |
| **Alert rule error (bad PromQL)** | Rule fails to evaluate; alert never fires | `prometheus_rule_evaluation_failures_total` | Rule validation in CI/CD, `promtool check rules` | ~10 min (deploy fix) |
| **Silence misconfiguration** | Legitimate alerts suppressed | Silence audit dashboard, daily Slack report | Max 24h silences, required comments, weekly review | Manual fix |
| **Alert flood (10,000+ alerts)** | On-call overwhelmed, miss critical alerts | Alert volume metric spike | Grouping, inhibition, auto-silence mechanism | ~5 min (auto-mitigation) |
| **Clock skew between Prometheus and AM** | Alerts fire/resolve with wrong timestamps | NTP monitoring | All hosts sync to NTP; alerts are based on scrape time, not wall clock | Config fix |

---

## 9. Security

### Authentication & Authorization
- **Alertmanager API**: Behind OAuth2 proxy. Only Prometheus instances (service account) and authorized engineers can send alerts or create silences.
- **PagerDuty**: API keys stored in Kubernetes Secrets (encrypted at rest). Rotated quarterly.
- **Silence creation**: Logged with engineer identity. Requires at minimum `on-call` role.
- **Alert rule changes**: Via Git PR with required review from team lead and platform team.

### Audit Trail
- All silence create/delete operations logged to audit system.
- All alert rule changes tracked in Git (who, when, what changed).
- PagerDuty provides incident audit trail (who was paged, when they acknowledged, actions taken).
- Alert history store retains all alert lifecycle events for compliance.

### Sensitive Information in Alerts
- Alerts should never contain secrets, credentials, or PII in labels or annotations.
- Alert annotations may contain links to dashboards/runbooks (internal URLs only).
- PagerDuty API communication over TLS.

---

## 10. Incremental Rollout

### Rollout Phases

| Phase | Scope | Duration | Success Criteria |
|---|---|---|---|
| **Phase 0: Infra setup** | Deploy Alertmanager cluster, PagerDuty integration | 1 week | Dead man's switch working, test alert delivered |
| **Phase 1: Infrastructure alerts** | Node Exporter (disk, CPU, memory), K8s health | 4 weeks | All hosts monitored, < 5 false positives/week |
| **Phase 2: Service alerts** | RED alerts for critical path services | 4 weeks | SLO-based alerts for top 10 services |
| **Phase 3: Team self-service** | All teams write their own alert rules | 8 weeks | 80% of services have SLO-defined alerts |
| **Phase 4: Alert quality program** | Monthly reviews, automated quality metrics | Ongoing | < 2 pages per on-call shift |

### Rollout Q&As

**Q1: How do you migrate from an existing alerting system (e.g., Nagios) to Alertmanager?**
A: (1) Map existing Nagios checks to Prometheus alert rules. (2) Run both systems in parallel. (3) Alertmanager sends to a test Slack channel; Nagios continues paging. (4) Compare: every Nagios alert should have an equivalent Prometheus alert. (5) After 4 weeks of parity, switch PagerDuty integration from Nagios to Alertmanager. (6) Keep Nagios read-only for 30 days as fallback.

**Q2: How do you onboard a team to the alerting platform?**
A: (1) Team attends 1-hour "Alerting Best Practices" workshop. (2) Team defines their SLOs (availability, latency targets). (3) Platform team generates initial alert rules from SLO definitions using Sloth. (4) Team reviews and customizes rules in their Git repo. (5) CI/CD deploys rules. (6) Team creates runbooks for each alert. (7) PagerDuty rotation is set up. Total: ~1 day of effort.

**Q3: How do you handle the transition period where some teams have alerts and others do not?**
A: Platform-wide infrastructure alerts (Node Exporter, K8s health) cover all resources from day 1 -- these do not require per-team setup. Service-level alerts are opt-in during rollout. For non-onboarded teams, the platform team's on-call handles infrastructure alerts affecting their services. As each team onboards, alert routing shifts from platform on-call to team on-call.

**Q4: How do you validate that alerts are actually reaching on-call?**
A: Monthly fire drills: (1) Send a test critical alert to each team's PagerDuty rotation. (2) Measure time to acknowledge. (3) If any team fails to acknowledge within 15 minutes, investigate (wrong phone number, app not installed, schedule gap). PagerDuty also provides "on-call readiness" checks that verify contact methods work.

**Q5: How do you handle alert configuration drift across 200 clusters?**
A: All alert rules are stored in Git and deployed via CI/CD to Prometheus ConfigMaps. Prometheus watches the ConfigMap and hot-reloads on change. A daily reconciliation job compares deployed rules with Git and alerts on drift. ArgoCD or Flux can continuously reconcile. No manual edits to Prometheus are allowed (the ConfigMap is the source of truth).

---

## 11. Trade-offs & Decision Log

| Decision | Options Considered | Chosen | Rationale |
|---|---|---|---|
| **Alert routing engine** | Alertmanager, OpsGenie, Custom | Alertmanager | Open source, Prometheus-native, proven at scale, HA gossip |
| **Notification platform** | PagerDuty, OpsGenie, VictorOps | PagerDuty | Market leader, best mobile app, rich API, escalation engine |
| **SLO alerting approach** | Threshold, single burn rate, multi-window multi-burn-rate | Multi-window multi-burn-rate | Catches both fast and slow burns, aligned with Google SRE best practice |
| **On-call schedule management** | In-house, PagerDuty, OpsGenie | PagerDuty | Not worth building in-house; PD has mature UI, mobile app, API |
| **Alert rule authoring** | UI-based, code-based (Git) | Code-based (Git) | Version control, code review, CI/CD deployment, auditability |
| **Alert quality tracking** | Manual review, automated metrics | Both | Automated metrics for continuous measurement, monthly manual review for improvement |
| **HA strategy** | Active-passive, active-active gossip | Active-active gossip (Alertmanager native) | Built-in, no external coordination needed, availability-first |
| **Severity levels** | 2 (page/no-page), 3 (critical/warning/info), 5 (P0-P4) | 3 (critical/warning/info) | Simple enough to be consistently applied, clear action per level |

---

## 12. Agentic AI Integration

### AI-Powered Alert Management

**Use Case 1: Automated Alert Triage**
```
AI Agent receives PagerDuty incident:
"JobSchedulerHighErrorRate: Error rate 8.7%"

1. Queries Prometheus for current metrics:
   - Error rate: 8.7% (confirming)
   - Request rate: 5000 rps (normal)
   - Latency p99: 2.3s (elevated)

2. Queries Jaeger for recent error traces:
   - 90% of errors are ResourceExhaustedException from resource-manager

3. Queries Elasticsearch for recent error logs:
   - "GPU pool gpu-a100-us-east capacity 0/64"

4. Checks recent changes:
   - No recent deploys for job-scheduler
   - resource-manager deployed v2.3.1 at 14:00 (2 hours ago)
   - ml-training team submitted batch-7234 requesting 64 GPUs at 14:15

5. Generates triage report (posted to incident Slack channel):
   "TRIAGE: JobSchedulerHighErrorRate
    Root cause: GPU resource pool exhausted by job batch-7234 (ml-training team)
    Trigger: batch-7234 consumed 100% of gpu-a100-us-east pool at 14:15
    Impact: All new GPU job submissions failing (47 failures in last 15 min)
    Suggested actions:
    1. Contact ml-training team to release unused GPUs
    2. Temporarily increase GPU pool quota
    3. Add pool reservation limits to prevent single-user exhaustion
    Confidence: 92%"
```

**Use Case 2: Proactive Alert Tuning**
```
AI Agent weekly analysis:
1. Reviews all alert firings for the past 7 days
2. Identifies patterns:
   - "DiskSpaceWarning" fired 23 times for host bm-042, auto-resolved each time
   - "KafkaConsumerLag" fired during every deployment window (expected behavior)
   - "MySQLSlowQueries" threshold (100ms) too low for analytics queries

3. Recommendations:
   a. "DiskSpaceWarning for bm-042: log rotation is clearing space.
       Consider: increase threshold from 80% to 85%, or fix root cause (log growth)"
   b. "KafkaConsumerLag during deploys: add silence during deploy window,
       or add label 'during_deploy=true' and inhibit"
   c. "MySQLSlowQueries: 70% of firings are from analytics queries (SELECT *).
       Split alert: separate thresholds for OLTP (100ms) and analytics (5s)"
```

**Use Case 3: Intelligent Escalation**
```
AI Agent evaluates incident severity in real-time:
1. Alert fires: "HighLatency for api-gateway"
2. Agent checks blast radius:
   - api-gateway is in the critical path for ALL user requests
   - Current latency: 5s p99 (normal: 200ms)
   - Affected users: estimated 100,000 active users
3. Auto-escalation decision:
   - Impact score: 9.2/10 (critical path + high user count)
   - Auto-escalates to incident commander without waiting for 10-min timer
   - Creates incident bridge (Zoom/Slack)
   - Posts initial status page update
```

**Guardrails:**
- AI triage is advisory; humans make final decisions.
- AI cannot create silences or modify alert rules without human approval.
- Auto-escalation requires human confirmation within 2 minutes or reverts to standard escalation.
- All AI actions are logged to the audit system.

---

## 13. Complete Interviewer Q&A Bank

### Alert Design (Q1-Q5)

**Q1: What makes a good alert?**
A: A good alert has these properties: (1) **Actionable**: when it fires, someone needs to do something. If no action is needed, it is not an alert, it is a log entry or dashboard metric. (2) **Urgent**: the action needs to happen now (for pages) or soon (for warnings). (3) **Real**: it has a low false positive rate (< 5%). (4) **Understandable**: the alert message tells you what is wrong, what the impact is, and links to a runbook. (5) **Scoped**: it fires for a specific service/component, not "something is wrong somewhere." Every alert should answer: "What broke? Who is affected? What should I do?"

**Q2: How do you decide the threshold for an alert?**
A: (1) Start with SLO-based thresholds: if the SLO is 99.9%, alert when the error rate threatens the SLO (burn-rate approach). (2) For infrastructure metrics (disk, memory), use historical data: compute the p95 value over 30 days and set the threshold above it. (3) For latency, use percentile-based thresholds (p99 > 5x normal) rather than absolute values. (4) Start with a lenient threshold and tighten over time based on false positive rate. (5) Never set thresholds based on gut feeling; always use data.

**Q3: Explain the difference between `for: 0m` and `for: 5m` in a Prometheus alert rule.**
A: `for: 0m` (or no `for` clause): the alert fires immediately when the condition is true. It transitions directly from `inactive` to `firing`. This is appropriate for critical conditions that need immediate attention (e.g., `up == 0` for a critical service). `for: 5m`: the alert transitions from `inactive` to `pending` when the condition first becomes true, then from `pending` to `firing` only if the condition remains true for 5 continuous minutes. If the condition becomes false during the 5-minute window, the alert goes back to `inactive` without firing. This eliminates transient spikes. Use `for: 5m` for most alerts to reduce noise.

**Q4: How do you alert on "absence of data" (a metric that should be present but is not)?**
A: Use Prometheus `absent()` function: `absent(up{job="job-scheduler"})` returns 1 when no `up` metric exists. For metrics that should always be non-zero: `rate(http_requests_total{service="api-gateway"}[5m]) == 0` alerts when traffic drops to zero. For expected host counts: `count(up{job="node-exporter"}) < 45000` alerts when fewer than 45,000 hosts are reporting. Always pair absence alerts with a reasonable `for` duration to avoid transient false positives.

**Q5: How do you handle alerts during deployments?**
A: (1) Automated silence: the deployment pipeline creates an Alertmanager silence for the deploying service for 15 minutes (`POST /api/v2/silences`). (2) Canary alerts: deploy to 5% of pods first, monitor for 10 minutes, then proceed. Alerting is active during canary phase. (3) Post-deploy validation: after deployment, a synthetic test runs and verifies the service is healthy before removing the silence. (4) If the deployment causes alerts after the silence expires, auto-rollback triggers.

### Operations (Q6-Q10)

**Q6: Describe the lifecycle of an alert from detection to resolution.**
A: (1) **Inactive**: Rule condition is false. (2) **Pending**: Condition becomes true, `for` timer starts. (3) **Firing**: Condition true for `for` duration. Prometheus sends to Alertmanager. (4) **Notification**: Alertmanager routes to PagerDuty. On-call receives phone call. (5) **Acknowledged**: On-call acknowledges in PagerDuty (stops escalation). (6) **Investigating**: On-call follows runbook, posts in #incidents. (7) **Resolved**: Either the condition becomes false (Prometheus sends resolved) or on-call manually resolves in PagerDuty. (8) **Postmortem**: If the alert was a significant incident, a postmortem is written within 48 hours.

**Q7: How do you handle alert flapping (fires and resolves repeatedly)?**
A: (1) Increase the `for` duration (e.g., 2m → 10m). (2) Alertmanager's `group_interval` prevents rapid-fire notifications for the same group. (3) Use hysteresis: fire at threshold A, resolve at threshold B (where B < A). Example: fire when disk > 90%, resolve when disk < 85%. (4) Use `avg_over_time()` instead of instantaneous value to smooth the metric. (5) If flapping persists, investigate the root cause: the system might be oscillating around the threshold (auto-scaler, GC cycles).

**Q8: How do you implement alert-based auto-remediation?**
A: Selected alerts trigger automated remediation: (1) Alert fires → Alertmanager webhook receiver → remediation service. (2) Remediation service validates the alert, executes the runbook programmatically (e.g., restart pod, clear cache, scale up replicas). (3) If remediation succeeds, the alert auto-resolves. If it fails, the alert continues to the on-call human. Examples: "PodCrashLooping" → delete the pod (Kubernetes creates a new one). "DiskSpaceLow" → run log cleanup script. "HighCPU" → scale up the deployment by 2 replicas. Guardrails: auto-remediation runs at most 3 times per hour per alert; logs every action to audit trail.

**Q9: How do you handle alert routing when team ownership changes?**
A: The `team` label on each alert rule determines routing. When ownership changes: (1) Update the `team` label in the alert rule YAML (Git PR). (2) Update the Alertmanager routing tree to include the new team's PagerDuty service key. (3) Service ownership is tracked in a service registry (CMDB). A reconciliation bot validates that `team` labels in alert rules match the service registry.

**Q10: How do you handle alerts for shared infrastructure (databases, message brokers)?**
A: Shared infrastructure (MySQL, Kafka, Elasticsearch) has platform team as the on-call owner. Alert routing: severity=critical → pagerduty-platform-oncall. For alerts specific to one team's usage (e.g., "team-ml-training Kafka consumer lag"), the alert includes `team=ml-training` label and routes to that team. For ambiguous cases (MySQL slow queries affecting multiple teams), the platform team triages and coordinates with affected teams.

### Advanced (Q11-Q15)

**Q11: How do you implement synthetic monitoring (probes)?**
A: Blackbox Exporter probes external endpoints: (1) HTTP probes check response code, latency, and TLS certificate. (2) TCP probes check port connectivity. (3) ICMP probes check host reachability. (4) DNS probes check name resolution. Configuration: Prometheus scrapes Blackbox Exporter with target URL as parameter. Alert rules: `probe_success == 0` (endpoint down), `probe_http_duration_seconds > 5` (slow), `probe_ssl_earliest_cert_expiry - time() < 7*86400` (cert expiring in 7 days). Probes run from multiple locations (per-DC) for redundancy.

**Q12: How do you prevent alert configuration errors from causing outages?**
A: (1) `promtool check rules` validates syntax in CI/CD. (2) `promtool test rules` runs unit tests with expected outcomes. (3) OPA (Open Policy Agent) policies enforce: every alert must have `severity`, `team`, and `runbook_url` labels; `for` duration must be >= 1m for warning, >= 2m for info. (4) Staging environment where rules are tested before production deployment. (5) Git branch protection: alert rule PRs require review from on-call engineer + platform team member.

**Q13: How do you measure the effectiveness of your alerting system?**
A: Key metrics tracked monthly: (1) **Detection time**: average time from anomaly start to alert firing. (2) **MTTA**: mean time to acknowledge. (3) **MTTR**: mean time to resolve. (4) **Actionability rate**: % of alerts that required human action. (5) **False positive rate**: % of alerts that were not real problems. (6) **Coverage**: % of production incidents that were detected by alerts (vs. reported by users). Target: > 95% of incidents detected by alerts before user reports.

**Q14: How do you handle alerting for canary deployments and A/B tests?**
A: Canary-specific alerts: (1) Compare canary vs. baseline error rate: `(canary_error_rate - baseline_error_rate) / baseline_error_rate > 0.5` (50% relative increase). (2) Use Prometheus labels: `canary=true` on canary pods, `canary=false` on baseline. (3) Alert only if canary error rate is significantly worse than baseline (statistical test, not just threshold). (4) For A/B tests: alert if any variant's error rate exceeds the SLO, regardless of comparison.

**Q15: How do you implement "correlation alerts" (multiple conditions across different metrics)?**
A: Prometheus supports multi-metric alert expressions: `rate(errors[5m]) > 0.05 AND rate(requests[5m]) > 100` (high error rate only when traffic is significant). For cross-service correlation: use recording rules to compute intermediate metrics, then combine in alert expressions. Example: "alert when Kafka consumer lag is high AND the consuming service's CPU is at 100%" → indicates the service is CPU-bound and cannot keep up. For complex correlations, consider a dedicated alert correlation engine (like BigPanda or custom Flink job).

---

## 14. References

1. **Google SRE Book - Chapter 6: Monitoring Distributed Systems**: https://sre.google/sre-book/monitoring-distributed-systems/
2. **Google SRE Workbook - Alerting on SLOs**: https://sre.google/workbook/alerting-on-slos/
3. **Alertmanager Documentation**: https://prometheus.io/docs/alerting/latest/alertmanager/
4. **PagerDuty Operations Guide**: https://www.pagerduty.com/resources/learn/incident-response/
5. **Sloth SLO Generator**: https://github.com/slok/sloth
6. **Rob Ewaschuk - My Philosophy on Alerting**: https://docs.google.com/document/d/199PqyG3UsyXlwieHaqbGiWVa8eMWi8zzAn0YfcApr8Q
7. **Prometheus Best Practices for Alerting**: https://prometheus.io/docs/practices/alerting/
8. **Blackbox Exporter**: https://github.com/prometheus/blackbox_exporter
9. **Alert Fatigue in SRE** - Will Larson: https://lethain.com/alert-fatigue/
10. **Multi-Window Multi-Burn-Rate Alerts Explained**: https://sre.google/workbook/alerting-on-slos/#recommended_time_windows_and_burn_rates
