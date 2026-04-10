# System Design: Elasticsearch

---

## 1. Requirement Clarifications

### Functional Requirements

1. **Document Indexing**: The system must accept structured and semi-structured JSON documents and make them searchable within 1 second (near-real-time).
2. **Full-Text Search**: Free-text queries must return documents ranked by relevance using TF-IDF / BM25 scoring.
3. **Structured Filtering**: Users must be able to combine full-text queries with structured filters (date ranges, keyword matches, numeric comparisons) using boolean logic (AND, OR, NOT, must, should, filter).
4. **Aggregations**: The system must support analytical aggregations over the result set: terms aggregation (faceted navigation), date histograms, min/max/avg/sum/percentile metrics, and nested aggregations.
5. **Near-Real-Time Indexing**: Documents indexed must be visible to search queries within 1 second.
6. **Cluster Management**: The system must support a distributed cluster of nodes with automatic shard allocation, rebalancing, and failure recovery.
7. **Replication**: Each shard must have configurable replicas for fault tolerance and read throughput.
8. **Index Lifecycle Management**: Old indices must be automatically rolled over and retired based on age or size policies.

### Non-Functional Requirements

1. **Throughput**: Support 10,000 indexing documents/second and 1,000 search QPS per cluster.
2. **Latency**: P99 search latency < 200 ms for typical queries; aggregation queries < 500 ms.
3. **Scale**: Handle indices with up to 1 trillion documents and 1 PB of data per cluster.
4. **Availability**: 99.9% uptime (8.7 hours downtime/year); no data loss on single-node failure.
5. **Consistency**: Near-real-time (NRT) — documents visible within 1 second of indexing.
6. **Durability**: Data persisted to disk with translog; no acknowledged write is ever lost.

### Out of Scope

- ACID transactional semantics (Elasticsearch is not a relational database).
- Joins across indices (Elasticsearch does not support cross-index joins at query time).
- OLAP-style analytical queries over PBs of data (use Spark or BigQuery for that).
- Graph traversal queries.
- Real-time streaming aggregation (use Flink/Kafka Streams).

---

## 2. Users & Scale

### User Types

| User Type             | Description                                                                      |
|-----------------------|----------------------------------------------------------------------------------|
| Application Developer | Issues search queries via REST API from application backends.                    |
| Data Ingestion Service| Writes documents to Elasticsearch via bulk indexing APIs.                        |
| DevOps / SRE          | Manages cluster: node allocation, index settings, ILM policies, snapshot/restore.|
| Kibana / Dashboard User| Issues aggregation queries for dashboards and analytics.                       |

### Traffic Estimates

Assumptions:
- A medium-to-large production Elasticsearch cluster (e.g., serving a major e-commerce platform's product search and log analytics).
- 500 million documents indexed in the primary product search index.
- 5 billion log documents across time-series indices.
- Search traffic: 1,000 QPS average, 3,000 QPS peak.
- Indexing traffic: 5,000 docs/second average, 15,000 docs/second peak.

| Metric                          | Calculation                                              | Result            |
|---------------------------------|----------------------------------------------------------|-------------------|
| Total documents                 | 500M product + 5B log                                    | ~5.5 B docs       |
| Search QPS (average)            | Given assumption                                         | 1,000 QPS         |
| Search QPS (peak)               | 1,000 × 3                                                | 3,000 QPS         |
| Indexing throughput (average)   | 5,000 docs/s                                             | 5K docs/s         |
| Indexing throughput (peak)      | 15,000 docs/s                                            | 15K docs/s        |
| Bulk indexing batches/s         | 15,000 docs/s ÷ 500 docs/batch                           | 30 batches/s      |
| Index refresh rate              | Every 1 second (NRT)                                     | 1 Hz              |

### Latency Requirements

| Operation                   | P50 Target | P99 Target | Notes                                          |
|-----------------------------|------------|------------|------------------------------------------------|
| Single-document get         | 5 ms       | 20 ms      | Direct doc ID lookup; no scoring               |
| Simple keyword search       | 20 ms      | 100 ms     | 5-shard scatter/gather + BM25                  |
| Complex boolean + agg       | 80 ms      | 500 ms     | Multi-shard agg; depends on cardinality        |
| Bulk index acknowledgement  | 50 ms      | 200 ms     | After translog fsync; before segment flush     |
| Segment refresh (NRT)       | —          | 1,000 ms   | Documents visible within 1 s of indexing       |

### Storage Estimates

Assumptions:
- Average product document (JSON): 2 KB raw.
- After Lucene compression: 0.7 KB stored.
- Inverted index overhead: ~50% of raw document size.
- Log document: 500 bytes raw; ~200 bytes stored.
- Replication factor: 1 primary + 1 replica (2x total storage).

| Component                         | Calculation                                               | Size         |
|-----------------------------------|-----------------------------------------------------------|--------------|
| Product index raw storage         | 500M × 2 KB                                               | 1 TB         |
| Product index compressed          | 500M × 0.7 KB                                             | 350 GB       |
| Product inverted index overhead   | 350 GB × 1.5                                              | 525 GB       |
| Product index with replica        | 525 GB × 2                                                | ~1.05 TB     |
| Log indices raw                   | 5B × 500 bytes                                            | 2.5 TB       |
| Log indices compressed            | 5B × 200 bytes × 2 (replica)                              | ~2 TB        |
| **Total cluster storage**         |                                                           | **~3.1 TB**  |

Note: A real large deployment (ELK stack for security/observability) can reach petabytes; these estimates represent a typical mid-scale deployment.

### Bandwidth Estimates

| Flow                              | Calculation                                          | Bandwidth     |
|-----------------------------------|------------------------------------------------------|---------------|
| Indexing ingest (peak)            | 15,000 docs/s × 2 KB                                 | ~30 MB/s      |
| Search responses (peak)           | 3,000 QPS × 50 KB avg response (10 hits + agg)       | ~150 MB/s     |
| Intra-cluster replication         | 30 MB/s indexing × 1 replica                         | ~30 MB/s      |

---

## 3. High-Level Architecture

```
 ┌──────────────────────────────────────────────────────────────────────────┐
 │                          CLIENT LAYER                                     │
 │   (Application servers, Logstash/Fluentd, Kibana, REST clients)          │
 └─────────────────┬───────────────────────────────┬────────────────────────┘
                   │ Search/Index Requests          │ Bulk Index Requests
                   ▼                               ▼
 ┌─────────────────────────────────────────────────────────────────────────┐
 │                    COORDINATING NODES (stateless)                        │
 │  (Parse requests, route to correct shards, aggregate partial results)    │
 │  (No data storage; pure request coordination; scale independently)       │
 └─────────┬────────────────────────────┬────────────────────────────┬─────┘
           │                            │                            │
     ┌─────▼──────┐             ┌───────▼──────┐             ┌───────▼──────┐
     │  DATA NODE │             │  DATA NODE   │             │  DATA NODE   │
     │  node-1    │             │  node-2      │             │  node-3      │
     │            │             │              │             │              │
     │ Shard P0   │             │ Shard P1     │             │ Shard P2     │
     │ Shard R1   │             │ Shard R2     │             │ Shard R0     │
     │ Shard R2   │             │ Shard R0     │             │ Shard R1     │
     │            │             │              │             │              │
     │ [Lucene    │             │ [Lucene      │             │ [Lucene      │
     │  Index     │             │  Index       │             │  Index       │
     │  Segments] │             │  Segments]   │             │  Segments]   │
     └────────────┘             └──────────────┘             └──────────────┘
           │                            │                            │
           └────────────────────────────┴────────────────────────────┘
                                        │
                              ┌─────────▼──────────┐
                              │  MASTER NODES (3)   │
                              │  (Cluster state,    │
                              │   shard allocation, │
                              │   node membership;  │
                              │   elected via Raft) │
                              └────────────────────┘

 ┌──────────────────────────────────────────────────────────────────────────┐
 │                        STORAGE LAYER (per data node)                     │
 │                                                                           │
 │  ┌─────────────────────┐   ┌────────────────────┐   ┌─────────────────┐  │
 │  │  In-Memory Buffer   │   │  Translog (WAL)    │   │  Segment Files  │  │
 │  │  (IndexWriter       │   │  (fsync'd every    │   │  (Immutable     │  │
 │  │   in-memory index)  │   │   5s or per req)   │   │   Lucene segs)  │  │
 │  └──────────┬──────────┘   └────────────────────┘   └────────┬────────┘  │
 │             │ refresh (1s)                                    │ merge     │
 │             └─────────────────────────────────────────────────┘          │
 └──────────────────────────────────────────────────────────────────────────┘
```

**Component Roles:**

| Component             | Role                                                                                         |
|-----------------------|----------------------------------------------------------------------------------------------|
| Coordinating Node     | Parses query DSL, routes sub-queries to primary/replica shards, merges partial results, returns response. |
| Data Node             | Stores shard data (Lucene index + segments); executes local search and indexing.             |
| Master Node           | Maintains cluster state (routing table, index metadata, shard allocations); runs Raft-based election. |
| Shard (Primary)       | Owns a partition of the index; accepts writes; replicates to replica shards.                 |
| Shard (Replica)       | Read-only copy of a primary shard; serves search queries; promoted to primary on failure.    |
| Translog (WAL)        | Write-ahead log for durability; ensures acknowledged writes survive process crashes.         |
| Lucene Segment        | Immutable on-disk inverted index file; multiple segments per shard, periodically merged.     |
| In-Memory Buffer      | Accumulates indexed documents; flushed to a new segment on refresh (every 1 second).        |

**Primary Use-Case Data Flow (index a document, then search):**

**Indexing:**
1. Client sends `POST /products/_doc/123` with JSON document.
2. Coordinating node hashes doc ID → determines primary shard (e.g., shard 1 on node-2).
3. Coordinating node forwards to node-2's primary shard 1.
4. Node-2 writes to in-memory buffer AND translog (WAL), fsync translog.
5. Node-2 replicates to replica shard 1 (node-1) — async.
6. Once replica acknowledges, node-2 ACKs to coordinating node → client receives 201.
7. After 1 second (default `refresh_interval`), the in-memory buffer is flushed to a new Lucene segment — document becomes searchable (NRT).

**Search:**
1. Client sends `GET /products/_search` with query DSL.
2. Coordinating node broadcasts query to one copy (primary or replica) of each shard — parallel fan-out.
3. Each shard executes query locally: inverted index lookup, BM25 scoring, return local top-K (e.g., top-100) doc IDs + scores.
4. Coordinating node merges all partial top-K lists → global top-K.
5. Coordinating node fetches full document fields for top-K from shards (second round trip — "fetch phase").
6. Returns merged, paginated result set to client.

---

## 4. Data Model

### Entities & Schema

```json
// ─────────────────────────────────────────
// Elasticsearch Index Mapping (products index)
// ─────────────────────────────────────────
PUT /products
{
  "settings": {
    "number_of_shards": 5,
    "number_of_replicas": 1,
    "refresh_interval": "1s",
    "index": {
      "codec": "best_compression",
      "similarity": {
        "custom_bm25": {
          "type": "BM25",
          "k1": 1.2,
          "b": 0.75
        }
      }
    },
    "analysis": {
      "analyzer": {
        "product_analyzer": {
          "type": "custom",
          "tokenizer": "standard",
          "filter": ["lowercase", "asciifolding", "en_stop", "en_stemmer", "synonym_filter"]
        }
      },
      "filter": {
        "en_stop": { "type": "stop", "stopwords": "_english_" },
        "en_stemmer": { "type": "stemmer", "language": "english" },
        "synonym_filter": {
          "type": "synonym",
          "synonyms_path": "analysis/synonyms.txt"
        }
      }
    }
  },
  "mappings": {
    "properties": {
      "product_id":    { "type": "keyword" },
      "title":         {
                         "type": "text",
                         "analyzer": "product_analyzer",
                         "similarity": "custom_bm25",
                         "fields": {
                           "keyword": { "type": "keyword", "ignore_above": 256 }
                         }
                       },
      "description":   { "type": "text", "analyzer": "product_analyzer" },
      "brand":         { "type": "keyword" },
      "category":      {
                         "type": "text",
                         "fields": {
                           "keyword": { "type": "keyword" }
                         }
                       },
      "price":         { "type": "scaled_float", "scaling_factor": 100 },
      "rating":        { "type": "half_float" },
      "review_count":  { "type": "integer" },
      "in_stock":      { "type": "boolean" },
      "attributes":    {
                         "type": "nested",
                         "properties": {
                           "name":  { "type": "keyword" },
                           "value": { "type": "keyword" }
                         }
                       },
      "created_at":    { "type": "date", "format": "strict_date_optional_time" },
      "updated_at":    { "type": "date", "format": "strict_date_optional_time" },
      "suggest":       {
                         "type": "completion",
                         "analyzer": "product_analyzer"
                       }
    }
  }
}

// ─────────────────────────────────────────
// Time-Series Log Index Mapping (ILM-managed)
// ─────────────────────────────────────────
PUT /_index_template/logs_template
{
  "index_patterns": ["logs-*"],
  "data_stream": {},
  "template": {
    "settings": {
      "number_of_shards": 3,
      "number_of_replicas": 1,
      "refresh_interval": "5s",
      "index.lifecycle.name": "logs_policy",
      "index.lifecycle.rollover_alias": "logs"
    },
    "mappings": {
      "properties": {
        "@timestamp":  { "type": "date" },
        "level":       { "type": "keyword" },
        "service":     { "type": "keyword" },
        "trace_id":    { "type": "keyword" },
        "message":     { "type": "text", "analyzer": "standard" },
        "host":        { "type": "keyword" },
        "latency_ms":  { "type": "integer" },
        "status_code": { "type": "short" },
        "request_path":{ "type": "keyword" }
      }
    }
  }
}

// ─────────────────────────────────────────
// ILM Policy (Index Lifecycle Management)
// ─────────────────────────────────────────
PUT /_ilm/policy/logs_policy
{
  "policy": {
    "phases": {
      "hot": {
        "min_age": "0ms",
        "actions": {
          "rollover": {
            "max_primary_shard_size": "50gb",
            "max_age": "1d"
          },
          "forcemerge": { "max_num_segments": 1 }
        }
      },
      "warm": {
        "min_age": "7d",
        "actions": {
          "allocate": { "number_of_replicas": 0 },
          "readonly": {}
        }
      },
      "cold": {
        "min_age": "30d",
        "actions": {
          "allocate": { "require": { "box_type": "cold" } }
        }
      },
      "delete": {
        "min_age": "90d",
        "actions": { "delete": {} }
      }
    }
  }
}
```

### Database Choice

Elasticsearch is the system being designed here, so this section discusses the internal storage choices made within Elasticsearch's architecture:

| Internal Storage Component   | Option                      | Pros                                              | Cons                                              | Selected |
|------------------------------|-----------------------------|---------------------------------------------------|---------------------------------------------------|----------|
| Inverted index format        | Lucene segments             | Battle-tested; excellent compression; SIMD ready  | JVM GC overhead; compaction pauses                | YES      |
| Inverted index format        | Custom binary               | Ultimate performance                              | Maintenance burden                                | No       |
| Document store               | Lucene stored fields        | Co-located with index; good compression           | Cannot update individual fields without reindex   | YES      |
| Document store               | Separate column store       | Better analytics performance                     | Added complexity                                  | No (Fielddata/Doc Values used) |
| Translog (WAL)               | Sequential write + fsync    | Durable; sequential I/O fast on spinning disk    | fsync latency affects indexing throughput         | YES      |
| Node communication           | Custom TCP binary protocol  | Low overhead; connection pooling                  | Not HTTP-based (Elasticsearch adds REST layer on top) | YES |
| Cluster state storage        | In-memory + Raft log        | Fast reads; strong consistency for metadata       | Entire state must fit in memory                   | YES      |

**External database choice for complementary systems:**

When Elasticsearch is used as part of a broader system:
- **Primary data store**: PostgreSQL or DynamoDB is the system of record; Elasticsearch is the search index. Documents are written to primary DB first, then synced to ES via CDC (Change Data Capture) pipeline.
- **Justification**: Elasticsearch's lack of ACID transactions makes it unsuitable as the only data store for transactional data.

---

## 5. API Design

Elasticsearch exposes a REST API. All endpoints use JSON over HTTP/HTTPS. Authentication via API keys or TLS client certificates.

### Index a Document

```
PUT /products/_doc/123
Authorization: ApiKey <base64_encoded_key>
Content-Type: application/json

{
  "product_id": "SKU-123",
  "title": "Wireless Noise Cancelling Headphones",
  "brand": "SoundMax",
  "price": 299.99,
  "rating": 4.5,
  "in_stock": true,
  "created_at": "2024-11-15T12:00:00Z"
}

Response: 201 Created
{
  "_index": "products",
  "_id": "123",
  "_version": 1,
  "result": "created",
  "_shards": { "total": 2, "successful": 2, "failed": 0 },
  "_seq_no": 0,
  "_primary_term": 1
}
```

### Bulk Index

```
POST /_bulk
Authorization: ApiKey <key>
Content-Type: application/x-ndjson

{"index": {"_index": "products", "_id": "124"}}
{"title": "Sony WH-1000XM5", "price": 349.99, "brand": "Sony", "in_stock": true}
{"index": {"_index": "products", "_id": "125"}}
{"title": "Bose QuietComfort 45", "price": 279.00, "brand": "Bose", "in_stock": false}

Response: 200 OK
{
  "took": 30,
  "errors": false,
  "items": [
    { "index": { "_index": "products", "_id": "124", "status": 201, "result": "created" } },
    { "index": { "_index": "products", "_id": "125", "status": 201, "result": "created" } }
  ]
}
```

### Search with Query DSL

```
GET /products/_search
Authorization: ApiKey <key>
Content-Type: application/json

{
  "from": 0,
  "size": 10,
  "track_total_hits": true,
  "query": {
    "bool": {
      "must": [
        {
          "multi_match": {
            "query": "noise cancelling headphones",
            "fields": ["title^3", "description", "brand^2"],
            "type": "best_fields",
            "tie_breaker": 0.3
          }
        }
      ],
      "filter": [
        { "term": { "in_stock": true } },
        { "range": { "price": { "gte": 100, "lte": 500 } } },
        { "term": { "brand": "Sony" } }
      ],
      "should": [
        { "term": { "brand": "Sony" } }
      ],
      "minimum_should_match": 0
    }
  },
  "sort": [
    { "_score": "desc" },
    { "rating": "desc" },
    { "review_count": "desc" }
  ],
  "aggs": {
    "brands": {
      "terms": { "field": "brand", "size": 10 }
    },
    "price_histogram": {
      "histogram": { "field": "price", "interval": 50 }
    },
    "avg_rating": {
      "avg": { "field": "rating" }
    }
  },
  "highlight": {
    "fields": {
      "title": { "number_of_fragments": 1, "fragment_size": 150 },
      "description": { "number_of_fragments": 2 }
    }
  }
}

Response: 200 OK
{
  "took": 45,
  "timed_out": false,
  "_shards": { "total": 5, "successful": 5, "failed": 0 },
  "hits": {
    "total": { "value": 234, "relation": "eq" },
    "max_score": 12.43,
    "hits": [
      {
        "_index": "products",
        "_id": "125",
        "_score": 12.43,
        "_source": { "title": "Sony WH-1000XM5", "price": 349.99, "brand": "Sony" },
        "highlight": {
          "title": ["Sony <em>WH-1000XM5 Noise Cancelling</em> Headphones"]
        }
      }
    ]
  },
  "aggregations": {
    "brands": {
      "buckets": [
        { "key": "Sony", "doc_count": 45 },
        { "key": "Bose", "doc_count": 32 }
      ]
    },
    "price_histogram": {
      "buckets": [
        { "key": 250.0, "doc_count": 78 },
        { "key": 300.0, "doc_count": 54 }
      ]
    },
    "avg_rating": { "value": 4.32 }
  }
}
```

---

## 6. Deep Dive: Core Components

### 6.1 Inverted Index Internals & BM25 Scoring

**Problem it solves:**
Given a query "noise cancelling headphones" and 500 million product documents, the system must identify and rank the most relevant documents in < 100 ms. A brute-force scan of all documents is infeasible; the inverted index pre-computes term-to-document mappings.

**Lucene Segment Internals:**

A Lucene index (one per Elasticsearch shard) consists of multiple segment files. Each segment is immutable once written:

```
Lucene Segment File Components:
─────────────────────────────────────────────────────────────────────

1. Term Dictionary (.tim / .tip files)
   Purpose: Maps terms → byte offset in posting lists
   Structure: Block-based B-tree
   Example entry: "headphones" → offset 0x1A2B3C in .doc file

2. Posting Lists (.doc file)
   Purpose: Stores the list of document IDs containing each term
   Format: Delta-encoded, compressed with FOR (Frame Of Reference)
   Example: term "headphones" → [1, 3, 7, 12, 25, 31, ...] (doc IDs)
   After delta-encoding: [1, 2, 4, 5, 13, 6, ...] (gaps between doc IDs)

3. Term Frequencies (.doc file, interleaved)
   Purpose: How many times each term appears in each document
   Format: Stored alongside doc IDs in posting lists
   Example: "headphones" in doc 3 → tf=2

4. Position Lists (.pos file)
   Purpose: Position of each term occurrence (for phrase queries)
   Format: Delta-encoded per-doc positions
   Example: "headphones" in doc 3 → positions [15, 47] (word indices)

5. Stored Fields (.fdt / .fdx files)
   Purpose: Original field values (for _source, highlight, return)
   Format: Block-compressed (LZ4 or DEFLATE) stored field data

6. DocValues (.dvd / .dvm files)
   Purpose: Column-oriented storage for sorting, aggregations, scripting
   Format: Sorted or numeric column store; NumericDocValues for numbers
   Key insight: opposite layout to inverted index (doc → value vs term → docs)

7. Norms (.nvd / .nvm files)
   Purpose: Pre-computed length normalisation factors per document field
   Format: Single byte per document (quantised float)

8. Bloom Filter (.tip file, per segment)
   Purpose: Fast negative term membership test
   Use: Check if a term exists in a segment before doing full dict lookup

9. Segment Info (.si file)
   Purpose: Segment metadata: num_docs, codec version, field info, del count

10. Live Docs (.liv file)
    Purpose: Bitset of non-deleted doc IDs within segment
    Deletion is lazy: doc is marked deleted; removed on next merge
```

**BM25 Scoring Implementation:**

```python
# BM25 parameters (Elasticsearch defaults)
K1 = 1.2    # term frequency saturation: higher K1 = more weight to TF
B  = 0.75   # length normalisation: 0 = no normalisation, 1 = full normalisation

def bm25_score(query_terms, doc_id, index):
    """
    Compute BM25 relevance score for a document given a query.
    This runs per-shard in Lucene during the query phase.
    """
    score = 0.0
    doc_length = index.get_doc_length(doc_id)           # number of tokens in doc
    avg_doc_length = index.get_avg_doc_length()         # corpus average

    for term in query_terms:
        tf = index.get_term_frequency(term, doc_id)     # times term appears in doc
        df = index.get_doc_frequency(term)              # number of docs containing term
        N  = index.total_doc_count()                    # total docs in shard

        # IDF component: rare terms are more discriminative
        # +1 smoothing prevents log(0) for terms in all documents
        idf = math.log(1 + (N - df + 0.5) / (df + 0.5))

        # TF component with saturation (K1) and length normalisation (B)
        # As tf → ∞, TF_component → (K1 + 1) [saturation]
        # Documents shorter than average get a score boost (B effect)
        length_norm = 1 - B + B * (doc_length / avg_doc_length)
        tf_component = (tf * (K1 + 1)) / (tf + K1 * length_norm)

        score += idf * tf_component

    return score

# Field boosting (applied before BM25 at query time):
# A match in "title" field (boost=3) contributes 3× more to score than "description"
# Implemented by multiplying the field-level BM25 score by the boost factor
```

**Segment Merge Process:**

```
Write path:
Document added → in-memory IndexWriter buffer
                      │ refresh (every 1s by default)
                      ▼
               New immutable segment (small, in memory first)
                      │ flush to disk (every 512 MB or on commit)
                      ▼
               Segment files on disk
                      │ merge (background thread, tiered merge policy)
                      ▼
               Larger merged segment (small segments eliminated)
```

Merge policy (TieredMergePolicy):
- Segments are grouped into "tiers"; when a tier has ≥ N segments (default 10), they are merged.
- Merging 10 × 10 MB segments → 1 × 100 MB segment reduces segment count, improving query performance (fewer segments to search).
- Force merge to 1 segment per shard on read-only indices (time-series warm tier) for maximum query performance.

**Interviewer Q&A:**

1. **Q: What is the NRT (near-real-time) mechanism? How does a document become searchable within 1 second?**
   A: The key is Lucene's `IndexWriter.commit()` vs `IndexWriter.refresh()`. A `commit()` makes data durable to disk (slow — requires fsync). A `refresh()` creates a new in-memory segment from the current IndexWriter buffer and makes it searchable without fsync. Elasticsearch calls `refresh()` every 1 second (configurable via `refresh_interval`). After refresh, the new segment is visible to all subsequent search operations on that shard. The document is NOT yet durably on disk after refresh — that happens on the next `flush()` (which fsyncs the translog and writes a new Lucene commit point). This separation is why NRT = 1s but durability is guaranteed by the translog at write-acknowledgement time.

2. **Q: Why does Elasticsearch use a separate translog (WAL) if Lucene already has durability?**
   A: Lucene's `commit()` is expensive (fsync of all segment files). Without a translog, every indexed document would require a full Lucene commit to be durable — this would limit indexing throughput severely. The translog is a simple sequential append-only file that can be fsynced cheaply per document or per batch. If the node crashes between Lucene commits, the translog is replayed to recover the missing documents. Lucene commit (checkpoint) happens every 30 minutes or on explicit flush; translog keeps a record of all operations since the last checkpoint.

3. **Q: Explain how field boosting works with BM25 at the Lucene level.**
   A: When a query has `"title^3"` (boost=3), Elasticsearch computes the BM25 score for the "title" field separately from the "description" field, then multiplies the title BM25 score by 3 before summing. This is implemented via Lucene's `BoostQuery` wrapper. The `multi_match` query with `tie_breaker` parameter further refines this: the highest-scoring field's score counts fully (×1.0), while other matching fields contribute their score × tie_breaker (e.g., 0.3). This prevents artificially inflating scores for documents that match a term across many fields.

4. **Q: What happens to deleted documents? How does deletion work in an immutable segment model?**
   A: Deletion is two-phase: (1) Immediately, the deleted doc_id is added to a `.liv` (live docs) bitset file associated with the segment. This bitset is checked during every search; "deleted" docs are skipped during scoring. (2) Eventually, when the segment is merged with others, deleted docs are physically removed from the new merged segment. This means: deletes are always fast (O(1) bitset write); storage is reclaimed only on merge. A high delete rate (e.g., frequent updates) causes segment bloat; `forcemerge` can reclaim space immediately.

5. **Q: How does Elasticsearch handle updates? Are they in-place?**
   A: No in-place updates. Elasticsearch deletes the old document (marks it deleted in the live-docs bitset) and indexes a new version. The `_version` field increments. For partial updates (`POST /_update`), Elasticsearch fetches the existing `_source`, merges the updated fields, and re-indexes the merged document — again as a delete + insert. This is why high-update-rate workloads benefit from `forcemerge` to remove tombstoned documents and reduce storage overhead. Optimistic concurrency control uses `_seq_no` and `_primary_term` to prevent conflicting concurrent updates.

---

### 6.2 Sharding Strategy & Cluster Management

**Problem it solves:**
A single Lucene index cannot serve 500M documents with < 100ms query latency and 5,000 docs/s indexing throughput on a single node. Data must be partitioned across multiple nodes. Decisions about how many shards to create and how to size them are critical for performance.

**Approaches Comparison:**

| Sharding Strategy             | Query Parallelism | Hotspot Risk    | Shard Count Control | Notes                                           |
|-------------------------------|-------------------|-----------------|---------------------|-------------------------------------------------|
| Single shard                  | None              | Extreme         | None                | Node becomes bottleneck; no horizontal scale   |
| Shard by document ID hash     | High              | Low (uniform)   | Fixed at creation   | Default Elasticsearch behavior; good general choice |
| Shard by time (ILM)           | Medium            | Low for time queries | Grows over time | Best for time-series; newer data is hot         |
| Shard by tenant/category      | Medium            | High (unequal)  | Controllable        | Useful for multi-tenant isolation               |
| Custom routing                | Customisable      | Variable        | Fixed               | Co-locate related docs on same shard for locality|

**Selected: Hash-based sharding for product index; Time-based (ILM) for log index**

**Shard Count Decision Formula:**

```
Recommended shard size: 10 GB to 50 GB per primary shard
(too small → overhead of many small segments; too large → slow rebalancing)

For product index:
  Total data size = 525 GB (with inverted index)
  Target shard size = 50 GB
  Number of shards = ceil(525 GB / 50 GB) = ceil(10.5) = 11 → round up to 5 or 10 (powers of 2 align with routing)
  Selected: 5 shards (expected shard size: 525/5 = 105 GB — slightly large but manageable)
  With 1 replica: 10 shards total → spread across data nodes

For log index (time-series, ILM-managed):
  Rollover at: 50 GB per primary shard OR 1 day
  With 3 primaries per daily index: 3 shards × 50 GB = 150 GB/day
  30 days hot tier: 30 × 150 GB = 4.5 TB
  Then warm → cold → delete per ILM policy
```

**Cluster State Management (Master nodes):**

```
Master node responsibilities:
1. Maintain cluster state: { index metadata, routing table, node membership }
2. Run shard allocation: decide which data nodes host which shards
3. Detect node failures: ping each node every 1s; declare dead after 3 missed pings
4. Trigger shard recovery on failure: copy missing primary from replica
5. Index creation / deletion: validate settings, allocate shards

Cluster state update flow:
  Master node proposes update → Raft consensus (majority vote of master-eligible nodes)
  → Committed update published to all data nodes
  → Data nodes apply routing table changes (start/stop shards as assigned)

Master election (Raft-based since ES 7.0):
  All master-eligible nodes participate in Raft.
  Minimum master-eligible nodes: ceil((N_masters + 1) / 2) where N_masters = 3
  → minimum 2 of 3 must agree (prevents split-brain)
```

**Shard Allocation and Rebalancing:**

```python
# Elasticsearch's ShardsAllocator logic (conceptual)

class ShardAllocator:
    def allocate(self, cluster_state, unassigned_shards):
        for shard in unassigned_shards:
            best_node = self.find_best_node(shard, cluster_state)
            assign(shard, best_node)

    def find_best_node(self, shard, cluster_state):
        candidates = cluster_state.data_nodes
        # Filter out nodes that already have this shard's replica
        candidates = [n for n in candidates
                      if not has_copy_of_same_index(n, shard)]
        # Filter by disk threshold (default: don't fill above 85% disk)
        candidates = [n for n in candidates
                      if n.disk_usage_pct < 85]
        # Balance: prefer nodes with fewer shards
        return min(candidates, key=lambda n: n.shard_count)

    def rebalance(self, cluster_state):
        # Move shards from over-loaded to under-loaded nodes
        avg_shards = total_shards / len(data_nodes)
        for node in data_nodes:
            if node.shard_count > avg_shards + threshold:
                shard_to_move = pick_relocatable_shard(node)
                target = find_best_node(shard_to_move, cluster_state)
                move(shard_to_move, node, target)
```

**Interviewer Q&A:**

1. **Q: Can you change the number of shards after an index is created?**
   A: Not directly — the shard count is fixed at index creation because the routing formula `shard = hash(doc_id) % num_shards` would break if num_shards changed (existing documents would be "lost" — on the wrong shard). Options: (1) `_split` API: double the shard count (5 → 10) by splitting each shard into 2 — documents are rehashed; only works if the new count is a multiple of the old. (2) `_shrink` API: reduce shard count (10 → 5) — only works if all primaries of the shrunk shards are on one node. (3) Full reindex: create a new index with the desired shard count, reindex all documents (`POST /_reindex`), then swap aliases.

2. **Q: How many master-eligible nodes should you run and why?**
   A: Always an odd number of master-eligible nodes: 3 is standard for most clusters (larger clusters can use 5). Rationale: Raft requires a majority (quorum) to commit cluster state changes. With 3 nodes, quorum = 2; you can tolerate 1 node failure. With 2 nodes, quorum = 2; any single failure loses consensus — worse than 1 node (which doesn't need consensus). 5 nodes tolerate 2 failures but increase overhead. In practice, dedicated master nodes (no data) are recommended for clusters > 5 data nodes — this prevents master tasks from being disrupted by heavy search/index load on data nodes.

3. **Q: What is "split brain" in Elasticsearch and how is it prevented?**
   A: Split brain occurs when a network partition causes two separate groups of nodes to each elect their own master and diverge in cluster state. Both sides accept writes independently; when the partition heals, they have conflicting data. Prevention: Raft consensus (since ES 7.0) requires a majority of master-eligible nodes to agree before any cluster state change is committed. With 3 master-eligible nodes and quorum=2, if the cluster splits into a group of 2 and a group of 1, only the group of 2 can form quorum and elect a master. The group of 1 becomes read-only (and rejects writes). When the partition heals, the group-of-1 node resynchronises from the majority's cluster state. The `cluster.initial_master_nodes` setting prevents false elections during initial cluster bootstrap.

4. **Q: What is "hot-warm-cold" architecture and when do you use it?**
   A: A tiered storage architecture for time-series data: Hot tier: fast NVMe SSD nodes; active indexing and recent queries (e.g., last 7 days of logs). Warm tier: cheaper HDD nodes; older data that's still queried but not written to; replicas reduced to 0 to save storage. Cold tier: cheapest storage (object store via searchable snapshots); infrequent queries; data mounted from S3/GCS snapshots without copying to local disk. Delete tier: ILM policy deletes indices after retention period. This architecture reduces cost by 5-10x for long-retention log data vs. keeping everything on SSD.

5. **Q: How does Elasticsearch handle a data node failure during an active indexing operation?**
   A: When a data node fails (heartbeat timeout), the master node promotes a replica shard to primary (if a replica exists) within seconds. In-flight index operations to the failed primary: if the coordinating node hasn't received an ACK, it will retry the operation (Elasticsearch uses sequence numbers to detect duplicates — the retry is idempotent). Operations acknowledged before the failure are safe: they were written to the translog on the replica before ACK was sent to the client. The new primary's translog is replayed to ensure it's in sync with the old primary's last committed state. The overall process takes ~30-60 seconds for a large shard to complete recovery.

---

### 6.3 Aggregations & Relevance Scoring

**Problem it solves:**
E-commerce faceted search requires both ranked results AND aggregated counts per facet (brand: Sony=45, Bose=32, price range: $100-200: 78 products). These aggregations must run over the same filtered document set returned by the query, within the overall latency budget.

**Aggregation Execution Model:**

```
Aggregation types and their execution:

1. Bucket Aggregations (group documents):
   - terms: group by field value → top-N buckets by doc count
   - date_histogram: group by time interval (hour, day, month)
   - range: group by numeric range buckets
   - nested: aggregate inside nested objects

2. Metric Aggregations (compute statistics per bucket):
   - min/max/avg/sum: single-pass computation
   - percentiles: TDigest approximation algorithm
   - cardinality: HyperLogLog++ approximation

3. Pipeline Aggregations (aggregate aggregations):
   - derivative: change rate between buckets
   - cumulative_sum: running total
   - bucket_sort: sort buckets by a metric value

Execution flow for terms aggregation:

// On each shard (parallel):
function shard_terms_agg(query_results, field, size):
    // Step 1: Apply query filter → get doc ID set
    matching_docs = execute_query(query)

    // Step 2: For each matching doc, look up field value in DocValues
    //  DocValues is a column store: fast iteration by doc ID
    //  Unlike inverted index (term → docs), DocValues is doc → value
    bucket_counts = {}
    for doc_id in matching_docs:
        value = docvalues.get(field, doc_id)  // O(1) column lookup
        bucket_counts[value] = bucket_counts.get(value, 0) + 1

    // Step 3: Return top (size × shard_size_factor) buckets to coordinating node
    //  We return more than `size` because other shards may have different top buckets
    //  shard_size = max(size * 1.5 + 10, size) is the default
    return top_k(bucket_counts, k=shard_size)

// On coordinating node:
function merge_terms_agg(partial_results_from_each_shard, size):
    global_counts = {}
    for partial in partial_results_from_each_shard:
        for key, count in partial.buckets:
            global_counts[key] = global_counts.get(key, 0) + count
    return top_k(global_counts, k=size)
```

**DocValues for Aggregations:**

```
Why DocValues instead of inverted index for aggregations?

Inverted index layout:       DocValues layout:
term  → [doc1, doc2, ...]   doc_id → value
"Sony" → [1, 3, 7, 12]     1 → "Sony"
"Bose" → [2, 5, 9]         2 → "Bose"
                             3 → "Sony"

For aggregation ("count docs per brand"):
  Inverted index approach: iterate all terms, count posting list lengths
    → must scan all terms in dictionary, then check each doc
    → O(V + N) where V = vocabulary size, N = matching docs
  DocValues approach: iterate all matching doc IDs, look up their brand value
    → O(N) where N = matching docs
    → Better for high-cardinality fields and when N << total docs
    → Column-oriented storage → excellent cache locality

DocValues are stored as:
  - SortedDocValues (keyword fields): sorted list of unique values + per-doc ordinal index
  - NumericDocValues (numeric fields): fixed-width per-doc numeric array
  - Compressed at the block level (128 docs per block)
```

**Fielddata vs DocValues:**

```
field_type  | Storage    | Use Case                  | Memory  | When to Use
------------|------------|---------------------------|---------|---------------------------
keyword     | DocValues  | Aggregations, sorting      | Disk    | Always use for categorical
text        | Fielddata  | Aggregations on text       | Heap    | Avoid! Use .keyword sub-field
            |            | (on-the-fly inverted index |         |
            |            | inversion → heap OOM risk) |         |
```

**Relevance Scoring — Functional Score Query:**

```json
// Example: boost products with higher ratings + recent dates + in_stock
GET /products/_search
{
  "query": {
    "function_score": {
      "query": { "match": { "title": "headphones" } },
      "functions": [
        {
          "filter": { "term": { "in_stock": true } },
          "weight": 1.5
        },
        {
          "field_value_factor": {
            "field": "rating",
            "factor": 0.5,
            "modifier": "log1p",
            "missing": 3.0
          }
        },
        {
          "gauss": {
            "created_at": {
              "origin": "now",
              "scale": "30d",
              "offset": "7d",
              "decay": 0.5
            }
          }
        }
      ],
      "score_mode": "sum",
      "boost_mode": "multiply"
    }
  }
}
// Final score = BM25_score × (in_stock_boost + rating_boost + recency_decay)
// Gaussian decay: products older than 37 days get 50% score reduction
```

**Interviewer Q&A:**

1. **Q: Why does terms aggregation return inaccurate results for distributed indices? How do you fix it?**
   A: The problem is that with N shards, the coordinating node asks each shard for its top-K buckets. If "Sony" has 100 documents on shard 1 but is not in the top-10 of shard 2 (which returns "Bose" with 90 docs instead), the coordinating node never sees Sony's shard-2 count. The final count for "Sony" is understated. Solutions: (1) Increase `shard_size` parameter — request more buckets per shard so that less-frequent-on-individual-shards terms are still included. Default is `size × 1.5 + 10`. (2) Set `execution_hint: "map"` to enumerate ALL values on each shard (exact but potentially very slow for high-cardinality fields). (3) Ensure the aggregation field has low cardinality or use `global_ordinals` optimization. The error rate decreases as data distribution becomes more uniform across shards.

2. **Q: What is the "fielddata" problem and how does it cause OutOfMemoryError?**
   A: Aggregations or sorting on `text` fields require "fielddata" — an on-the-fly reverse of the inverted index structure loaded into the JVM heap. For a high-cardinality text field (e.g., "title"), this means loading the entire term-to-doc mapping into heap memory. On a 50M document index, this can consume 10s of GBs of heap. The JVM GC then runs frequently to reclaim this space, causing "stop the world" pauses and query latency spikes. Eventually, `OutOfMemoryError` crashes the node. Fix: NEVER aggregate on text fields. Always add a `.keyword` sub-field (which uses DocValues stored efficiently on disk) and aggregate on `title.keyword` instead.

3. **Q: How does the HyperLogLog cardinality aggregation work?**
   A: `cardinality` aggregation estimates the number of distinct values for a field without storing all unique values (which would require memory proportional to cardinality). HyperLogLog++ (HLL++) hashes each field value to a bit string, then observes the maximum number of leading zeros across all hashes. Statistically, the probability of seeing K leading zeros requires ≈ 2^K unique values. HLL++ uses multiple "registers" (sub-hash buckets) and harmonic mean to reduce variance. Accuracy is ±5% by default; configurable up to ±0.001% by increasing `precision_threshold` (at higher memory cost). Memory: O(log(log(N))) — ~80 KB for 1 billion distinct values.

4. **Q: How does Elasticsearch score documents when multiple clauses match (bool query with must + should)?**
   A: The `must` clauses are AND requirements — all must match. Their individual scores are summed and divided by the number of clauses (configurable via `boost` and `tie_breaker`). The `should` clauses are optional but increase the score when they match. The `filter` clauses do not contribute to score (they run in filter context — binary match/no-match, results cached in bitsets). The final score formula for a bool query: `score = sum_of_matching_clause_scores / num_matching_clauses`. A document matching all `should` clauses scores higher than one matching only `must`. The `minimum_should_match` parameter controls how many `should` clauses must match for a document to be included.

5. **Q: How does Elasticsearch handle a query that times out on some shards but succeeds on others?**
   A: When `timeout` is set in the request, each shard enforces the deadline locally. If a shard times out, it returns whatever results it has computed so far (partial results). The coordinating node includes these partial results in the response with `"timed_out": true` and a shard-level `_shards.failed` count. The client receives a degraded but valid response: results may be incomplete (missing documents from timed-out shards), but the query doesn't error. The application must handle partial results: either retry with a higher timeout, or accept the degraded quality with a warning banner in the UI.

---

## 7. Scaling

### Horizontal Scaling

**Scaling Reads:**
- Add replica shards (increase `number_of_replicas` on existing index; Elasticsearch auto-allocates them).
- Add data nodes: Elasticsearch auto-rebalances shards across new nodes.
- Rule of thumb: Each data node should handle at most 20 shards per GB of heap (e.g., 32 GB heap → ≤ 640 shards per node).

**Scaling Writes:**
- Add more primary shards (requires reindex or index splitting).
- Scale horizontally: more data nodes handle more primary shards.
- Increase refresh interval (e.g., `refresh_interval: "30s"`) during bulk indexing — fewer refresh cycles = higher throughput.
- Use bulk API (batch 500-1,000 docs per request) to amortise network overhead.

**Scaling Aggregations:**
- Scale data nodes vertically (more heap for aggregation state).
- Denormalise (pre-aggregate at index time using ingest pipelines) to reduce query-time aggregation complexity.

### DB Sharding

| Index Type    | Sharding Approach         | Number of Shards | Rationale                                            |
|---------------|---------------------------|------------------|------------------------------------------------------|
| Products      | Hash(doc_id) % 5          | 5 primary        | Uniform distribution; 105 GB per shard               |
| Logs (daily)  | Time-based (ILM rollover) | 3 per day        | Temporal locality; hot/warm/cold tiering             |
| User profiles | Hash(user_id) % 10        | 10 primary       | Even distribution; 50 GB per shard at 1B users       |

### Replication

- Standard: 1 primary + 1 replica (2 copies).
- Increase to 2 replicas for indices requiring higher read throughput or 3-node fault tolerance.
- Replica shards serve reads independently, doubling read throughput per shard.
- `wait_for_active_shards` setting controls write durability: `1` = write acknowledged after primary only, `all` = write acknowledged after all replicas confirm.

### Caching

| Cache              | Technology              | What's Cached                                     | Size / Policy          |
|--------------------|-------------------------|---------------------------------------------------|------------------------|
| Request cache      | In-process (JVM heap)   | Complete query responses for size=0 agg queries   | 1% of heap; LRU       |
| Query cache        | In-process (JVM heap)   | Segment-level filter bitsets (frequently reused)  | 10% of heap; LRU      |
| Field data cache   | In-process (JVM heap)   | Fielddata (text field aggregations — avoid!)      | 40% of heap (caution) |
| OS page cache      | OS-level memory         | Lucene segment files (most important!)            | All available free RAM|
| External cache     | Redis / Varnish         | Complete search responses (by application layer)  | TTL-based per use case|

Key insight: The OS page cache is the most important cache in Elasticsearch. Lucene reads are memory-mapped (mmap); frequently accessed segment files stay in OS page cache. This is why Elasticsearch nodes should have RAM ≥ 2× the size of the hot data tier — half for JVM heap, half for OS page cache.

### CDN

Not directly applicable to Elasticsearch (an internal service). However:
- If search results are served via a public-facing API, the application layer may cache certain popular search responses (e.g., "trending products" page) at an edge CDN.
- The Elasticsearch cluster itself is never exposed to public internet; all access is through application services.

**Interviewer Q&A:**

1. **Q: What is the "split brain" mitigation in Elasticsearch 7.0+ compared to the old `minimum_master_nodes` setting?**
   A: Pre-7.0, `minimum_master_nodes` (equivalent to quorum size) was a manually configured parameter. Administrators could misconfigure it (e.g., setting it to 1 in a 3-node cluster, allowing split brain). Post-7.0, Elasticsearch uses a proper Raft-based consensus algorithm where the quorum is automatically managed. The `cluster.initial_master_nodes` setting bootstraps the cluster once; thereafter, Raft handles all elections with automatic quorum computation. No administrator manual configuration of quorum is needed. The Raft log also ensures cluster state changes are linearisable — no two masters can simultaneously commit conflicting changes.

2. **Q: How do you scale an Elasticsearch cluster that has grown beyond its initial shard count?**
   A: Three paths: (1) Vertical scaling of existing data nodes (more RAM, faster NVMe) — cheapest short-term. (2) `_split` API (ES 6.1+) — split existing primary shards by factor of 2 without reindexing. Requires enough disk space on nodes to hold the split copies. (3) Full reindex — create a new index with more shards and use `_reindex` API or Logstash to copy documents. This is zero-downtime with index aliases: `alias: products_read` points to old index during reindex, then atomically swaps to point to new index. Option 3 is preferred when other changes (field mapping improvements, codec upgrade) are also needed.

3. **Q: How do you handle a "hot shard" problem where one shard receives 10x more traffic than others?**
   A: A hot shard usually means one "category" or "customer" generates disproportionate traffic. Solutions: (1) Custom routing with `routing.partition_count` — spread documents with the same routing key across multiple shards (e.g., `partition_count=3` sends each tenant's docs to 3 random shards instead of 1). (2) Denormalise: pre-compute and cache results for hot tenants/categories. (3) Dedicated index for hot tenants with more shards. (4) Read replicas: increase `number_of_replicas` specifically for the hot index so the coordinating node load-balances reads across more replicas.

4. **Q: How does an Elasticsearch cluster handle a prolonged network partition (more than the shard recovery timeout)?**
   A: If a data node is partitioned from the master for longer than `cluster.routing.allocation.node_left.delayed_timeout` (default: 1 minute), the master starts recovering shards from other nodes. When the partitioned node rejoins, if its shards are still at the same version as the copies on other nodes, they can be re-used (the node just re-joins with its existing data). If other nodes have received new writes and moved ahead in `_seq_no`, the rejoining node's shards become stale replicas and are brought up to date via shard recovery (copy changed segments from primary). This uses sequence numbers to send only the delta, minimising recovery time.

5. **Q: How do you choose between 3 shards × 1 replica vs 6 shards × 0 replicas for the same data size?**
   A: 3 shards × 1 replica = 6 total shard copies = same storage cost, but with fault tolerance. 6 shards × 0 replicas = no redundancy. Choosing 3P+1R over 6P+0R: You get fault tolerance (if one node fails, the replica is promoted to primary — no data loss, no unavailability). The 6-shard version fails if any data node goes down — the shard it hosted is unassigned until recovered from another node. Unless disk space is extremely constrained, always run at least 1 replica. The replica also handles read traffic, doubling read throughput. With 3P+1R spread across 3 nodes, each node holds one primary and one (different) replica — any single node can fail without losing data.

---

## 8. Reliability & Fault Tolerance

### Failure Scenarios

| Failure Scenario                   | Impact                                 | Mitigation                                                                              | Recovery Time |
|------------------------------------|----------------------------------------|-----------------------------------------------------------------------------------------|---------------|
| Data node failure (with replica)   | Reduced read throughput briefly        | Master promotes replica to primary; shard recovery copies missing replica              | < 60s         |
| Data node failure (no replica)     | Shard unavailable; partial index down  | Query returns partial results (`_shards.failed` > 0); recovery after restart           | Until node recovers |
| Master node failure (1 of 3)       | Brief election pause                   | Raft election; new master elected in ~10-30s; cluster continues                        | < 30s         |
| All 3 master nodes failure         | Cluster goes read-only                 | No writes accepted; existing data readable; must bring up master nodes                 | Manual        |
| Disk failure (data node)           | Shard lost if primary with no replica  | Restore from snapshot; monitor disk health proactively                                 | Hours         |
| JVM OutOfMemoryError               | Node crash                             | Auto-restart; shard recovery; increase heap or fix fielddata usage                     | Minutes       |
| Network partition (minor)          | Increased query latency                | Queries return partial results from reachable shards                                   | Until healed  |
| Index mapping conflict             | Indexing errors                        | Dynamic mapping disabled in production; all mappings explicit; validated in CI         | N/A (preventive) |
| Bulk indexing overload             | Queue backpressure; 429 errors         | Bulk thread pool queue depth; exponential backoff in client                            | Self-healing  |
| Snapshot repository failure        | No recent snapshots                    | Alert on snapshot failure; DR relies on replication if snapshot unavailable            | Managed       |

### Failover Strategy

- **Automatic**: Master node detects data node failure (ping timeout) → promotes replica to primary → allocates new replica on another node → cluster turns green again.
- **Snapshot-based DR**: Automated hourly snapshots to S3/GCS via `_snapshot` API. Cross-region snapshot replication for disaster recovery. Restore takes proportional to data size (~1 hour for 1 TB).
- **Read preference**: Clients can specify `preference=_local` to prefer local node, reducing cross-node RPC overhead.

### Retries & Idempotency

- Elasticsearch uses `_seq_no` and `_primary_term` for optimistic concurrency. Clients can include `if_seq_no` and `if_primary_term` to detect conflicts on retry.
- Bulk indexing with `op_type: "create"` is idempotent if the same `_id` is used — it fails gracefully if the document already exists.
- The `retry_on_conflict` parameter (default 0) causes automatic retries of update operations on version conflicts.

### Circuit Breaker

Elasticsearch has built-in circuit breakers to prevent OOM:
- `request_circuit_breaker`: limits memory for a single request (default 60% of heap).
- `fielddata_circuit_breaker`: limits fielddata loading (default 40% of heap).
- `in_flight_requests_circuit_breaker`: limits total incoming request size (default 100% of heap).
- When a circuit breaker trips, the request fails with `CircuitBreakingException` (HTTP 503) rather than crashing the JVM. Clients receive an actionable error to back off.

---

## 9. Monitoring & Observability

### Key Metrics

| Metric                                 | Type      | Alert Threshold          | Dashboard         |
|----------------------------------------|-----------|--------------------------|-------------------|
| Cluster status (green/yellow/red)      | Gauge     | yellow > 5 min; red > 1 min | Cluster Health |
| Search latency P50/P99                 | Histogram | P99 > 500 ms             | Search Perf       |
| Indexing rate (docs/s)                 | Counter   | Drop > 50% from baseline  | Indexing          |
| JVM heap usage %                       | Gauge     | > 85%                    | JVM               |
| GC pause time (old gen)               | Gauge     | > 500 ms/min             | JVM               |
| Shard count per node                   | Gauge     | > 20/GB heap (node)      | Sharding          |
| Segment count per shard               | Gauge     | > 200 (triggers merge)   | Segments          |
| Index refresh time                     | Gauge     | > 2s                     | NRT               |
| Fielddata memory usage                 | Gauge     | > 30% of heap            | Memory            |
| Disk utilisation %                     | Gauge     | > 85%                    | Disk              |
| Bulk reject rate (thread pool)        | Counter   | > 0                      | Indexing          |
| Circuit breaker trip rate             | Counter   | > 0                      | Memory            |
| Snapshot last success                  | Gauge     | > 24 hours ago           | DR                |

### Distributed Tracing

- Elasticsearch 7.9+ supports built-in OpenTelemetry instrumentation. Each request generates spans for: `search.query_phase` (per shard), `search.fetch_phase`, `index.primary`, `index.replica`.
- Correlate with application traces using `X-Opaque-Id` header (Elasticsearch echoes this in response headers and slow logs).
- Identify hot shards by comparing query phase durations across shards in the same request trace.

### Logging

- **Slow logs**: Elasticsearch's built-in slow query and slow index logs capture requests exceeding configurable thresholds:
  ```
  index.search.slowlog.threshold.query.warn: 5s
  index.search.slowlog.threshold.query.info: 1s
  index.search.slowlog.threshold.fetch.warn: 1s
  index.indexing.slowlog.threshold.index.warn: 2s
  ```
- **Audit logs**: Log all authentication, authorisation, and document-level security events (X-Pack security).
- **GC logs**: JVM garbage collection logs; critical for diagnosing heap pressure and long GC pauses.
- Ship all logs to a separate logging Elasticsearch cluster (not the same one being monitored — avoid monitoring affecting the monitored system).

---

## 10. Trade-offs & Design Decisions Summary

| Decision                            | Chosen                                  | Alternative                          | Why Chosen                                                                              |
|-------------------------------------|-----------------------------------------|--------------------------------------|-----------------------------------------------------------------------------------------|
| Shard count (product)               | 5 primary shards                        | 1 shard or 50 shards                 | 5 balances parallelism vs overhead; 50 shards for 500M docs would be overshareded      |
| NRT refresh interval                | 1 second                                | Immediate or 30 seconds              | 1s is good balance: < 1s would cause too many small segments; 30s reduces throughput   |
| Replica count                       | 1 replica                               | 0 or 2 replicas                      | 1 provides fault tolerance + read scalability without 3x storage overhead              |
| DocValues for agg fields            | Enabled for all keyword/numeric fields  | Fielddata (heap)                     | DocValues avoid heap pressure; fielddata causes OOM; DocValues have negligible latency |
| Dedicated master nodes              | 3 dedicated master-eligible nodes       | Master-eligible data nodes           | Dedicated masters prevent data-node load from disrupting cluster state management      |
| Translog flush policy               | Every 5s OR every 512 MB, whichever first | Per-request fsync               | Per-request fsync is safe but limits throughput to ~1000 TPS; 5s window is sufficient  |
| ILM for logs                        | Hot/warm/cold tiering with rollover     | Single large index                   | Tiering reduces cost 5-10x; rollover prevents single index growing unboundedly         |
| External system of record           | PostgreSQL as primary; ES as read index | ES as primary store                  | ES lacks ACID; PostgreSQL provides durability; ES provides search                     |

---

## 11. Follow-up Interview Questions

1. **Q: Explain the difference between `query` context and `filter` context in Elasticsearch.**
   A: In `query` context, the clause is asked "How well does this document match?" — BM25 scoring is computed and contributes to `_score`. In `filter` context, the clause is asked "Does this document match?" (binary yes/no) — no score computed. Filters are automatically cached as bitsets for reuse across requests. Use filter context for structured criteria (date ranges, keyword terms, boolean flags) and query context for full-text relevance. Incorrect use of query context for non-text fields (e.g., `term` in `must` instead of `filter`) wastes CPU on score computation and misses the filter cache.

2. **Q: What is the "mapping explosion" problem and how do you prevent it?**
   A: Dynamic mapping creates new fields automatically when new document keys appear. In log or event data, if each event has unique field names (e.g., `user_attribute_1234: value`), the mapping can grow to thousands of fields. Each unique field in the mapping increases cluster state size (all nodes must maintain the full mapping in memory). With 100,000 fields, cluster state can exceed hundreds of MB and slow down master node operations. Prevention: (1) `dynamic: "strict"` — reject documents with unmapped fields. (2) `dynamic: "false"` — ignore unknown fields (don't index them but store in `_source`). (3) Use `flattened` field type for key-value pairs (maps entire object to single Lucene field).

3. **Q: How would you implement multi-tenancy in Elasticsearch — separate indices vs single index with tenant_id filter?**
   A: Separate-index approach: one index per tenant; complete data isolation; independent shard allocation; easy deletion. Works well when tenants have different schemas. Problem: with 10,000 tenants, you have 10,000 × (shards) = potentially 50,000 shards — exceeding the recommended 20/GB heap limit. Single-index approach: all tenants share one index with a `tenant_id` field; much smaller shard count; term filter on `tenant_id` is cached as a bitset. Problem: "noisy neighbor" — one large tenant's expensive queries affect others; tenant deletion requires expensive delete-by-query. Hybrid: small tenants share a pool index; large tenants get dedicated indices.

4. **Q: How does Elasticsearch handle schema changes (field type changes)?**
   A: Field type changes (e.g., `text` → `keyword`) cannot be applied to an existing index — Lucene segments are immutable and already written with the old mapping. The only options: (1) Add a new field with the desired type (existing field cannot be reused). (2) Reindex: create a new index with the updated mapping, use `_reindex` API to copy data, swap aliases. During reindex, the application writes to both old and new indices (dual-write) to avoid data loss. After reindex completes, alias is atomically swapped. This is a standard operational procedure; applications should be designed with index aliasing from day one.

5. **Q: What is the "deep pagination" problem in Elasticsearch?**
   A: Querying `from: 10000, size: 10` requires the coordinating node to collect the top 10,010 documents from each shard, then discard all but the last 10. With 5 shards, this means processing 50,050 documents to return 10. Memory and CPU grow linearly with the pagination depth. At `from: 100000`, it's 500,050 documents processed. Solutions: (1) `search_after` — cursor-based pagination using the sort value of the last returned document as a starting point; constant cost regardless of depth. (2) `scroll` API — creates a consistent snapshot of results; server-side cursor; designed for export (large batch retrieval, not interactive pagination). (3) `pit` (Point In Time) + `search_after` — preferred modern approach combining consistent snapshot with cursor-based pagination.

6. **Q: How does the `_reindex` API work and what are its performance considerations?**
   A: `_reindex` reads documents from a source index and indexes them into a destination index. Internally: (1) Scroll through source index (consistent snapshot via PIT), (2) Index each batch into destination using Bulk API, (3) Throttle via `requests_per_second` parameter to avoid overwhelming the cluster. Performance: throttle to 500-1,000 docs/s during business hours; full speed during off-hours. For 500M documents: at 5,000 docs/s, reindex takes 500M/5,000 = 100,000 seconds ≈ 28 hours. Optimisations: disable replicas on destination during reindex (re-enable after), increase refresh_interval to "60s" during reindex, use `slices` parameter to run parallel reindex workers (one per source shard).

7. **Q: How would you implement search-as-you-type in Elasticsearch?**
   A: Two purpose-built options: (1) `search_as_you_type` field type — automatically creates edge N-gram sub-fields and uses a `bool_prefix` query type that handles the last partial term. More accurate than trie-based approaches for multi-word inputs. (2) `completion` suggester — trie-based, extremely fast, but limited to starting from a fixed prefix. For e-commerce, `completion` suggester is preferred for single-term product name suggestions. For multi-word search queries, `search_as_you_type` is better. The `completion` suggester stores its own optimised FST (Finite State Transducer) data structure, separate from the main inverted index, with sub-millisecond lookup.

8. **Q: How do you handle Elasticsearch running out of disk space?**
   A: The first line of defence is the disk-based shard allocator — when a node reaches 85% disk usage, Elasticsearch stops allocating new shards to it; at 90%, it moves shards away from the node. If all nodes are > 90% full, indexing stops (to prevent data corruption from partial writes). Prevention: (1) ILM policies to delete old data. (2) Reduce replica count for cold data. (3) Searchable snapshots (mount index from S3 without local copy). (4) Enable `best_compression` codec to reduce storage by ~30%. Emergency response: `POST /_cluster/settings { "transient": { "cluster.routing.allocation.disk.threshold_enabled": false } }` — temporarily disables the threshold to allow shard moves; then fix disk immediately.

9. **Q: What is the role of the `_source` field, and when would you disable it?**
   A: `_source` stores the original JSON document verbatim; returned in search responses for display and used for `_update` operations (fetch, modify, re-index). Disabling `_source` saves 20-30% storage but with major trade-offs: (1) No `_update` partial updates (must full reindex to update documents). (2) No `reindex` without re-crawling original data. (3) No `highlight` unless stored fields are explicitly configured. Disabling is only appropriate for append-only log data where original content is also in a separate system (e.g., S3), and storage cost is the primary concern. For most use cases, keep `_source` enabled but configure `source_exclude` to omit large unneeded fields.

10. **Q: How would you use Elasticsearch for security event logging and anomaly detection?**
    A: Security use case (SIEM): (1) Time-series log index per day/week via ILM. (2) Correlate events via `trace_id` / `session_id` using `terms` lookup queries. (3) Anomaly detection: use Elasticsearch's built-in ML jobs (X-Pack) — unsupervised time-series anomaly detection using bucket-based statistical models. (4) Alerting: Kibana Alerting rules fire when query returns > threshold results (e.g., > 100 failed logins from a single IP in 5 minutes). (5) Retention: 90 days hot → delete (SIEM compliance requirement). (6) Access control: field-level security (hide sensitive fields from analysts without admin role); document-level security (restrict which documents a user can see based on their clearance level).

11. **Q: What's the difference between `must_not` in a bool query and a must clause with a NOT?**
    A: `must_not` runs in filter context (no score contribution; results cached as bitsets). A `must` clause with `{ "bool": { "must_not": ... } }` nested inside runs in query context (contributes score). Also: `must_not` clauses in the top-level `filter` context are also cached. Use `must_not` in filter context for exclusion criteria that don't affect relevance scoring. For example, excluding adult content: `{ "bool": { "must": [...], "filter": { "term": { "safe": true } }, "must_not": { "term": { "banned": true } } } }`.

12. **Q: How does cross-cluster search work in Elasticsearch?**
    A: `_remote_clusters` configuration defines remote cluster endpoints. Cross-cluster search (CCS) allows a coordinating node in cluster A to fan-out search requests to cluster B's coordinating nodes. Syntax: `GET cluster_b:products,cluster_a:products/_search`. The local coordinating node merges results from both clusters. Use cases: federated search across regions for read locality, querying across prod and DR clusters, tenant isolation with cross-tenant analytics. Limitations: remote clusters must be reachable; full result set size is bounded by `from + size` per cluster; aggregations are merged the same way as intra-cluster scatter/gather.

13. **Q: How do you implement fine-grained access control in Elasticsearch?**
    A: X-Pack Security (included in Elasticsearch 7.0+): (1) Index-level security: roles specify which indices a user can `read` / `write` / `manage`. (2) Field-level security (FLS): roles can specify `grant_fields` or `except_fields` to hide specific document fields (e.g., hide `salary` from non-HR roles). (3) Document-level security (DLS): roles include a query that filters which documents are visible (`{ "term": { "department": "engineering" } }` restricts to only engineering docs). DLS is evaluated at query time — adds CPU overhead. (4) API key authentication for programmatic access; LDAP/SAML/OpenID for user authentication. (5) Audit logging records all access attempts.

14. **Q: What are Elasticsearch data streams and when should you use them?**
    A: Data streams are a higher-level abstraction over time-series indices with ILM. They consist of a sequence of time-based backing indices, exposed as a single virtual index for both reads and writes. Writes are auto-routed to the current write index (the "head" of the stream). When the write index rolls over (by ILM policy), a new backing index is created. Benefits: (1) Append-only semantics enforce correct time-series patterns. (2) No alias management needed — the data stream name IS the alias. (3) Per-backing-index ILM lifecycle. Use when: all documents have a `@timestamp`; workload is append-only (logs, metrics, traces, events); long retention with tiered storage. Do NOT use for mutable data that requires updates by ID.

15. **Q: How would you debug a sudden latency spike in Elasticsearch search queries?**
    A: Systematic debugging process: (1) Check cluster health (`GET /_cluster/health`) — yellow/red indicates shard issues. (2) Check node stats (`GET /_nodes/stats`) for JVM heap pressure, GC frequency, thread pool queue depths. (3) Enable slow logs and check for queries exceeding threshold. (4) `GET /_nodes/hot_threads` — shows which Java threads are consuming most CPU (identifies if GC or search threads are the bottleneck). (5) Check segment counts (`GET /_cat/shards?v`) — high segment count causes more Lucene files to search; force merge reduces it. (6) `GET /_cat/thread_pool?v` — check for rejected bulk or search requests. (7) Profile a specific slow query with `"profile": true` in the search request — returns per-shard per-phase timing breakdown. (8) Check index fielddata usage — fielddata eviction under heap pressure causes significant latency.

---

## 12. References & Further Reading

1. **Elasticsearch Official Documentation** — The Definitive Guide to Elasticsearch internals, mapping, query DSL. https://www.elastic.co/guide/en/elasticsearch/reference/current/index.html

2. **McCandless, M., Hatcher, E., Gospodnetic, O. (2010)** — *Lucene in Action* (2nd ed.). Manning Publications. (Lucene internals: segments, codecs, posting lists.)

3. **Gormley, C. & Tong, Z. (2015)** — *Elasticsearch: The Definitive Guide*. O'Reilly Media. https://www.elastic.co/guide/en/elasticsearch/guide/current/index.html

4. **Robertson, S. & Zaragoza, H. (2009)** — "The Probabilistic Relevance Framework: BM25 and Beyond." *Foundations and Trends in IR, 3(4)*. (BM25 mathematical foundations.) https://dl.acm.org/doi/10.1561/1500000019

5. **Flajolet, P. et al. (2007)** — "HyperLogLog: the analysis of a near-optimal cardinality estimation algorithm." *DMTCS 2007*. (Foundation for `cardinality` aggregation.) https://dl.acm.org/doi/10.5555/2785861.2785865

6. **Elastic Blog — "How to size your shards"** — https://www.elastic.co/blog/how-many-shards-should-i-have-in-my-elasticsearch-cluster

7. **Elastic Blog — "Stays in cache: understanding Elasticsearch cache efficiency"** — https://www.elastic.co/blog/elasticsearch-caching-deep-dive-boosting-query-speed-one-cache-at-a-time

8. **Elastic Blog — "Hot-Warm-Cold Architecture in Elasticsearch"** — https://www.elastic.co/blog/implementing-hot-warm-cold-in-elasticsearch-with-index-lifecycle-management

9. **Apache Lucene Architecture Documentation** — https://lucene.apache.org/core/documentation.html

10. **Dean, J. & Ghemawat, S. (2004)** — "MapReduce." OSDI. (Distributed aggregation patterns applicable to Elasticsearch shard-level reduce.)

11. **Raft Consensus Algorithm** — Ongaro, D. & Ousterhout, J. (2014). "In Search of an Understandable Consensus Algorithm." *USENIX ATC*. https://raft.github.io/ (Raft is the consensus algorithm used in Elasticsearch 7.0+ for master election.)
