# System Design: OpenStack Neutron Networking

> **Relevance to role:** Neutron is the networking backbone of an OpenStack cloud. As a cloud infrastructure platform engineer, you must understand how tenant isolation is achieved (VXLAN/VLAN segmentation), how the ML2 plugin architecture enables pluggable backends (OVS, OVN, SR-IOV), how distributed virtual routing eliminates network bottlenecks, and how security groups enforce tenant firewalling. Every VM, container, and bare-metal host depends on Neutron for connectivity.

---

## 1. Requirement Clarifications

### Functional Requirements
| # | Requirement |
|---|------------|
| FR-1 | Tenants can create isolated L2 networks (private networks) with DHCP |
| FR-2 | Tenants can create subnets with custom CIDR, gateway, DNS, allocation pools |
| FR-3 | Tenants can create routers connecting private networks to external networks |
| FR-4 | Floating IPs provide 1:1 NAT from public IPs to tenant instance IPs |
| FR-5 | Security groups implement stateful firewalling per port (ingress/egress rules) |
| FR-6 | Provider networks allow admin-created networks mapped directly to physical networks |
| FR-7 | Load Balancer as a Service (LBaaS via Octavia) provides L4/L7 load balancing |
| FR-8 | VPN as a Service (VPNaaS) provides IPSec VPN tunnels between tenant networks |
| FR-9 | Distributed Virtual Routing (DVR) enables east-west traffic without centralized router |
| FR-10 | Quality of Service (QoS) policies control bandwidth per port/network |
| FR-11 | Trunk ports support VLAN sub-interfaces for NFV workloads |
| FR-12 | Port security prevents MAC/IP spoofing |

### Non-Functional Requirements
| # | Requirement | Target |
|---|------------|--------|
| NFR-1 | Network provisioning latency (port ready) | < 5 seconds |
| NFR-2 | Floating IP association | < 2 seconds |
| NFR-3 | Security group rule propagation | < 3 seconds to all affected hosts |
| NFR-4 | East-west throughput (same host) | Line rate (25/100 Gbps NIC) |
| NFR-5 | East-west throughput (cross-host, VXLAN) | > 90% of wire speed |
| NFR-6 | Control plane availability | 99.99% |
| NFR-7 | Tenant network isolation guarantee | Zero cross-tenant traffic leakage |
| NFR-8 | Maximum networks per deployment | 16M (VXLAN VNI space) |
| NFR-9 | Maximum ports per network | 10,000+ |

### Constraints & Assumptions
- ML2 plugin with OVN mechanism driver (preferred for new deployments) or OVS mechanism driver
- VXLAN as primary tenant network type (VLAN for provider networks)
- Physical network: leaf-spine with BGP EVPN underlay
- MTU: 9000 (jumbo frames) on physical network; 8950 effective for VXLAN overhead
- IPv4 primary; IPv6 supported
- Octavia for load balancing (LBaaS v2)
- Keystone v3 for authentication

### Out of Scope
- Physical switch configuration (Netconf/YANG)
- SD-WAN and WAN optimization
- DNS as a Service (Designate) details
- BGP dynamic routing plugin details
- Bare-metal networking (Ironic networking)

---

## 2. Scale & Capacity Estimates

### Users & Traffic

| Metric | Estimate | Calculation |
|--------|----------|-------------|
| Compute hosts | 10,000 | 200 racks x 50 servers/rack |
| VMs | 400,000 | 10,000 hosts x 40 VMs/host |
| Ports (total) | 500,000 | 400K VMs + 50K DHCP/router/LB ports |
| Networks | 20,000 | 5,000 projects x 4 networks avg |
| Subnets | 30,000 | 20K networks x 1.5 subnets avg |
| Routers | 10,000 | 5K projects x 2 routers avg |
| Floating IPs | 50,000 | ~12.5% of VMs have floating IPs |
| Security groups | 25,000 | 5K projects x 5 groups avg |
| Security group rules | 250,000 | 25K groups x 10 rules avg |
| API requests/second | 500 | Port CRUD during VM lifecycle + queries |
| Port create/second (peak) | 100 | 50 VM creates/sec x 2 ports avg |
| DHCP requests/second (peak) | 200 | VM boots + lease renewals |

### Latency Requirements

| Operation | Target | Notes |
|-----------|--------|-------|
| POST /v2.0/ports | < 500 ms | Port creation and DB write |
| Port binding (OVS flow programming) | < 3 s | From port create to traffic flowing |
| DHCP response | < 1 s | VM receives IP after boot |
| Floating IP associate | < 2 s | NAT rule programmed on router namespace or DVR |
| Security group rule update | < 3 s | Propagated to all affected compute hosts |
| GET /v2.0/networks (list) | < 200 ms | Paginated |
| East-west packet latency (same rack) | < 0.5 ms | VXLAN encap/decap |
| East-west packet latency (cross-rack) | < 1 ms | Leaf-spine traversal + VXLAN |
| North-south packet latency | < 2 ms | Router namespace + NAT |

### Storage Estimates

| Data | Size | Calculation |
|------|------|-------------|
| Port records | ~250 MB | 500K ports x 500 bytes |
| Network/subnet records | ~15 MB | 50K records x 300 bytes |
| Security group rules | ~25 MB | 250K rules x 100 bytes |
| IP allocations | ~20 MB | 500K IPs x 40 bytes |
| Total Neutron DB | ~2 GB | Including indexes and metadata |
| OVS flow entries per host | ~50,000 | 40 VMs x ~1,250 flows/VM (security group + forwarding) |
| OVN Southbound DB | ~500 MB | All logical flows for the deployment |

### Bandwidth Estimates

| Flow | Bandwidth | Calculation |
|------|-----------|-------------|
| VXLAN overlay (east-west) | 10 Tbps aggregate | 10K hosts x 2 Gbps avg per host |
| North-south (internet) | 2 Tbps | 400K VMs x 5 Mbps avg external |
| DHCP traffic | Negligible | Small UDP packets |
| ARP/BUM traffic (VXLAN) | ~500 Mbps | Suppressed by OVN/DVR; worst case |
| Control plane (RabbitMQ/OVSDB) | ~100 Mbps | Flow programming, state sync |

---

## 3. High Level Architecture

```
                        ┌────────────────────────────────┐
                        │        External Network         │
                        │     (BGP peering, Internet)     │
                        └──────────────┬─────────────────┘
                                       │
                        ┌──────────────▼─────────────────┐
                        │     Provider Router / Gateway    │
                        │  (external bridge: br-ex)       │
                        └──────────────┬─────────────────┘
                                       │
         ┌─────────────────────────────┼───────────────────────────┐
         │                             │                           │
         ▼                             ▼                           ▼
┌─────────────────┐         ┌─────────────────┐         ┌─────────────────┐
│  Network Node 1 │         │  Network Node 2 │         │  Network Node 3 │
│ (HA / DVR-snat) │         │ (HA / DVR-snat) │         │ (HA / DVR-snat) │
│ ┌─────────────┐ │         │ ┌─────────────┐ │         │ ┌─────────────┐ │
│ │ neutron-l3  │ │         │ │ neutron-l3  │ │         │ │ neutron-l3  │ │
│ │   agent     │ │         │ │   agent     │ │         │ │   agent     │ │
│ ├─────────────┤ │         │ ├─────────────┤ │         │ ├─────────────┤ │
│ │neutron-dhcp │ │         │ │neutron-dhcp │ │         │ │neutron-dhcp │ │
│ │   agent     │ │         │ │   agent     │ │         │ │   agent     │ │
│ ├─────────────┤ │         │ ├─────────────┤ │         │ ├─────────────┤ │
│ │neutron-meta │ │         │ │neutron-meta │ │         │ │neutron-meta │ │
│ │data-agent   │ │         │ │data-agent   │ │         │ │data-agent   │ │
│ └─────────────┘ │         │ └─────────────┘ │         │ └─────────────┘ │
└─────────────────┘         └─────────────────┘         └─────────────────┘

         ┌──────────────────────────────────────────────────────┐
         │                  Control Plane                        │
         │  ┌──────────────┐  ┌─────────┐  ┌───────────────┐   │
         │  │neutron-server│  │Keystone │  │  RabbitMQ     │   │
         │  │ (x3, behind  │  │         │  │  Cluster      │   │
         │  │  HAProxy)    │  │         │  │               │   │
         │  └──────┬───────┘  └─────────┘  └───────┬───────┘   │
         │         │                               │            │
         │  ┌──────▼───────────────────────────────▼────────┐   │
         │  │              ML2 Plugin                        │   │
         │  │  ┌────────────────┐  ┌──────────────────────┐ │   │
         │  │  │ Type Drivers   │  │ Mechanism Drivers    │ │   │
         │  │  │ ─ VXLAN        │  │ ─ OVN (preferred)   │ │   │
         │  │  │ ─ VLAN         │  │ ─ OVS               │ │   │
         │  │  │ ─ GRE          │  │ ─ SR-IOV            │ │   │
         │  │  │ ─ Flat         │  │ ─ Linux Bridge      │ │   │
         │  │  └────────────────┘  └──────────────────────┘ │   │
         │  └───────────────────────────────────────────────┘   │
         │         │                                             │
         │  ┌──────▼───────┐                                    │
         │  │ Neutron DB   │                                    │
         │  │ (MySQL/Galera)│                                   │
         │  └──────────────┘                                    │
         └──────────────────────────────────────────────────────┘

         ┌──────────────────────────────────────────────────────┐
         │                 Compute Hosts                         │
         │                                                       │
         │  ┌────────────────────────────────────────────┐      │
         │  │ Compute Host                                │      │
         │  │  ┌─────────────────────────────────────┐   │      │
         │  │  │  OVS (or OVN controller)             │   │      │
         │  │  │  br-int (integration bridge)         │   │      │
         │  │  │  br-tun (tunnel bridge, VXLAN)       │   │      │
         │  │  │  br-ex  (external bridge, optional)  │   │      │
         │  │  └─────────────────────────────────────┘   │      │
         │  │  ┌──────┐ ┌──────┐ ┌──────┐               │      │
         │  │  │ VM 1 │ │ VM 2 │ │ VM 3 │  ...          │      │
         │  │  │(tap1)│ │(tap2)│ │(tap3)│               │      │
         │  │  └──┬───┘ └──┬───┘ └──┬───┘               │      │
         │  │     └────────┴────────┘                    │      │
         │  │              │                              │      │
         │  │        ┌─────▼─────┐                       │      │
         │  │        │  br-int   │ ← VLAN tagging        │      │
         │  │        └─────┬─────┘   (local VLAN ↔ VNI)  │      │
         │  │              │                              │      │
         │  │        ┌─────▼─────┐                       │      │
         │  │        │  br-tun   │ ← VXLAN encap/decap   │      │
         │  │        └─────┬─────┘                        │      │
         │  │              │                              │      │
         │  │        ┌─────▼─────┐                       │      │
         │  │        │ Physical  │ ← eth0 / bond0        │      │
         │  │        │   NIC     │                        │      │
         │  │        └───────────┘                        │      │
         │  └────────────────────────────────────────────┘      │
         └──────────────────────────────────────────────────────┘
```

### Component Roles

| Component | Role |
|-----------|------|
| **neutron-server** | Stateless REST API. Validates requests, applies RBAC via Keystone, persists resources to DB, notifies agents via RPC (RabbitMQ). Runs N instances behind HAProxy. |
| **ML2 Plugin** | Modular Layer 2 plugin: the core networking plugin. Composes type drivers (how networks are segmented) with mechanism drivers (how ports are bound to hosts). |
| **Type Drivers** | VXLAN: 24-bit VNI, UDP encapsulation, 16M networks. VLAN: 802.1Q tags, 4094 networks max. GRE: IP-in-IP tunnel. Flat: untagged. |
| **Mechanism Drivers** | OVN: distributed virtual networking via OVS + ovn-controller. OVS: Open vSwitch with neutron-openvswitch-agent. SR-IOV: hardware offload via PCI passthrough. LinuxBridge: simple iptables-based. |
| **neutron-dhcp-agent** | Manages dnsmasq processes in network namespaces. Provides DHCP and DNS for tenant networks. HA: multiple agents per network (dhcp_agents_per_network=3). |
| **neutron-l3-agent** | Manages router namespaces (qrouter-*) for L3 forwarding, NAT, and floating IPs. HA via VRRP (l3_ha) or DVR. |
| **neutron-metadata-agent** | Proxies Nova metadata requests from VMs through router or DHCP namespaces. |
| **OVN (ovn-controller)** | Runs on each compute host. Translates logical flows from OVN Southbound DB into OVS flows. Replaces neutron-openvswitch-agent, neutron-l3-agent, and neutron-dhcp-agent. |
| **br-int** | OVS integration bridge. All VM tap devices connect here. Local VLAN tagging for network isolation within the host. |
| **br-tun** | OVS tunnel bridge. Handles VXLAN/GRE encapsulation/decapsulation. Maps local VLAN tags to VXLAN VNIs. |
| **br-ex** | OVS external bridge. Connects to the physical network for north-south (external) traffic. |

### Data Flows

```
East-West Traffic (VM-A on Host-1 → VM-B on Host-2, same tenant network):
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

VM-A (10.0.1.5) → tap-a → br-int (VLAN tag 100)
  → br-tun (match VLAN 100 → encap VXLAN VNI 5001, dst=Host-2 VTEP IP)
  → physical NIC (UDP 4789)
  → [leaf-spine network]
  → Host-2 physical NIC
  → br-tun (match VNI 5001 → VLAN tag 200)  [local VLAN may differ]
  → br-int (VLAN tag 200 → tap-b)
  → VM-B (10.0.1.10)

North-South Traffic (VM → Internet, with floating IP):
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Without DVR (centralized routing):
  VM (10.0.1.5) → br-int → br-tun → [VXLAN to network node]
  → network node br-tun → br-int → qrouter namespace
  → SNAT: 10.0.1.5 → 203.0.113.50 (floating IP)
  → br-ex → physical network → Internet

With DVR (distributed routing):
  VM (10.0.1.5) → br-int → qrouter namespace (on SAME compute host)
  → SNAT: 10.0.1.5 → 203.0.113.50
  → br-ex (on compute host) → physical network → Internet
  [No traffic to network node for floating IP traffic!]
```

---

## 4. Data Model

### Core Entities & Schema

```sql
-- Neutron database (MySQL/Galera)

CREATE TABLE networks (
    id              VARCHAR(36) PRIMARY KEY,
    name            VARCHAR(255),
    project_id      VARCHAR(255),
    admin_state_up  BOOLEAN DEFAULT TRUE,
    status          VARCHAR(16) DEFAULT 'ACTIVE',  -- ACTIVE, BUILD, DOWN, ERROR
    shared          BOOLEAN DEFAULT FALSE,
    mtu             INT DEFAULT 1500,
    vlan_transparent BOOLEAN DEFAULT FALSE,
    availability_zone_hints TEXT,                    -- JSON array
    created_at      DATETIME NOT NULL,
    updated_at      DATETIME,
    INDEX idx_project_id (project_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE network_segments (
    id              VARCHAR(36) PRIMARY KEY,
    network_id      VARCHAR(36) NOT NULL,
    network_type    VARCHAR(32) NOT NULL,           -- 'vxlan', 'vlan', 'flat', 'gre'
    physical_network VARCHAR(64),                   -- physical network name (for vlan/flat)
    segmentation_id INT,                            -- VXLAN VNI or VLAN ID
    segment_index   INT DEFAULT 0,
    is_dynamic      BOOLEAN DEFAULT FALSE,
    FOREIGN KEY (network_id) REFERENCES networks(id) ON DELETE CASCADE,
    INDEX idx_network_id (network_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE subnets (
    id              VARCHAR(36) PRIMARY KEY,
    network_id      VARCHAR(36) NOT NULL,
    name            VARCHAR(255),
    project_id      VARCHAR(255),
    ip_version      INT NOT NULL,                   -- 4 or 6
    cidr            VARCHAR(64) NOT NULL,
    gateway_ip      VARCHAR(64),
    enable_dhcp     BOOLEAN DEFAULT TRUE,
    dns_nameservers TEXT,                            -- JSON array
    host_routes     TEXT,                            -- JSON array
    ipv6_ra_mode    VARCHAR(16),                    -- 'slaac', 'dhcpv6-stateful', 'dhcpv6-stateless'
    ipv6_address_mode VARCHAR(16),
    created_at      DATETIME NOT NULL,
    updated_at      DATETIME,
    FOREIGN KEY (network_id) REFERENCES networks(id),
    INDEX idx_network_id (network_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE subnet_allocation_pools (
    id              VARCHAR(36) PRIMARY KEY,
    subnet_id       VARCHAR(36) NOT NULL,
    first_ip        VARCHAR(64) NOT NULL,
    last_ip         VARCHAR(64) NOT NULL,
    FOREIGN KEY (subnet_id) REFERENCES subnets(id) ON DELETE CASCADE
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE ports (
    id              VARCHAR(36) PRIMARY KEY,
    network_id      VARCHAR(36) NOT NULL,
    name            VARCHAR(255),
    project_id      VARCHAR(255),
    mac_address     VARCHAR(32) NOT NULL,
    admin_state_up  BOOLEAN DEFAULT TRUE,
    status          VARCHAR(16) DEFAULT 'DOWN',     -- ACTIVE, DOWN, BUILD, ERROR
    device_id       VARCHAR(255),                   -- instance UUID or router UUID
    device_owner    VARCHAR(255),                   -- 'compute:nova', 'network:router_interface', 'network:dhcp'
    binding_host_id VARCHAR(255),                   -- compute host
    binding_vif_type VARCHAR(64),                   -- 'ovs', 'bridge', 'hw_veb' (SR-IOV)
    binding_vnic_type VARCHAR(64) DEFAULT 'normal', -- 'normal', 'direct' (SR-IOV), 'macvtap', 'direct-physical'
    binding_profile TEXT,                            -- JSON: PCI slot info for SR-IOV
    binding_vif_details TEXT,                        -- JSON: OVS port details
    created_at      DATETIME NOT NULL,
    updated_at      DATETIME,
    INDEX idx_network_id (network_id),
    INDEX idx_device_id (device_id),
    INDEX idx_project_id (project_id),
    INDEX idx_binding_host (binding_host_id),
    FOREIGN KEY (network_id) REFERENCES networks(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE ip_allocations (
    port_id         VARCHAR(36) NOT NULL,
    subnet_id       VARCHAR(36) NOT NULL,
    ip_address      VARCHAR(64) NOT NULL,
    PRIMARY KEY (port_id, subnet_id, ip_address),
    FOREIGN KEY (port_id) REFERENCES ports(id) ON DELETE CASCADE,
    FOREIGN KEY (subnet_id) REFERENCES subnets(id),
    INDEX idx_subnet_ip (subnet_id, ip_address)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE routers (
    id              VARCHAR(36) PRIMARY KEY,
    name            VARCHAR(255),
    project_id      VARCHAR(255),
    admin_state_up  BOOLEAN DEFAULT TRUE,
    status          VARCHAR(16) DEFAULT 'ACTIVE',
    gw_port_id      VARCHAR(36),                    -- gateway port on external network
    enable_snat     BOOLEAN DEFAULT TRUE,
    ha              BOOLEAN DEFAULT FALSE,           -- VRRP HA router
    distributed     BOOLEAN DEFAULT FALSE,           -- DVR router
    flavor_id       VARCHAR(36),
    availability_zone_hints TEXT,
    created_at      DATETIME NOT NULL,
    updated_at      DATETIME,
    FOREIGN KEY (gw_port_id) REFERENCES ports(id),
    INDEX idx_project_id (project_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE router_ports (
    router_id       VARCHAR(36) NOT NULL,
    port_id         VARCHAR(36) NOT NULL,
    port_type       VARCHAR(255),                   -- 'network:router_interface', 'network:router_gateway'
    PRIMARY KEY (router_id, port_id),
    FOREIGN KEY (router_id) REFERENCES routers(id) ON DELETE CASCADE,
    FOREIGN KEY (port_id) REFERENCES ports(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE floating_ips (
    id              VARCHAR(36) PRIMARY KEY,
    project_id      VARCHAR(255),
    floating_network_id VARCHAR(36) NOT NULL,
    floating_ip_address VARCHAR(64) NOT NULL,
    floating_port_id VARCHAR(36) NOT NULL,           -- port on external network
    fixed_port_id   VARCHAR(36),                     -- port on tenant network (when associated)
    fixed_ip_address VARCHAR(64),
    router_id       VARCHAR(36),
    status          VARCHAR(16) DEFAULT 'DOWN',      -- ACTIVE, DOWN, ERROR
    created_at      DATETIME NOT NULL,
    updated_at      DATETIME,
    FOREIGN KEY (floating_network_id) REFERENCES networks(id),
    INDEX idx_project_id (project_id),
    INDEX idx_fixed_port (fixed_port_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE security_groups (
    id              VARCHAR(36) PRIMARY KEY,
    name            VARCHAR(255) NOT NULL,
    project_id      VARCHAR(255),
    description     VARCHAR(255),
    created_at      DATETIME NOT NULL,
    updated_at      DATETIME,
    INDEX idx_project_id (project_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE security_group_rules (
    id                  VARCHAR(36) PRIMARY KEY,
    security_group_id   VARCHAR(36) NOT NULL,
    direction           ENUM('ingress', 'egress') NOT NULL,
    ethertype           VARCHAR(40) DEFAULT 'IPv4',     -- 'IPv4' or 'IPv6'
    protocol            VARCHAR(40),                     -- 'tcp', 'udp', 'icmp', null (any)
    port_range_min      INT,
    port_range_max      INT,
    remote_ip_prefix    VARCHAR(255),                    -- CIDR: '10.0.0.0/24'
    remote_group_id     VARCHAR(36),                     -- reference another SG
    created_at          DATETIME NOT NULL,
    FOREIGN KEY (security_group_id) REFERENCES security_groups(id) ON DELETE CASCADE,
    INDEX idx_security_group (security_group_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE security_group_port_bindings (
    port_id             VARCHAR(36) NOT NULL,
    security_group_id   VARCHAR(36) NOT NULL,
    PRIMARY KEY (port_id, security_group_id),
    FOREIGN KEY (port_id) REFERENCES ports(id) ON DELETE CASCADE,
    FOREIGN KEY (security_group_id) REFERENCES security_groups(id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- OVN-specific tables (if using OVN mechanism driver)
-- OVN stores its state in OVSDB (Northbound and Southbound databases), not MySQL.
-- Neutron ML2/OVN driver translates Neutron DB state → OVN Northbound DB.
```

### Database Selection

| Database | Justification |
|----------|--------------|
| **MySQL 8.0 + Galera Cluster** | Standard OpenStack DB. Neutron uses SQLAlchemy ORM with oslo.db. Galera provides synchronous replication for HA. |
| **OVN Northbound DB (OVSDB)** | Stores logical network topology (logical switches, routers, ACLs). Written by neutron-server's ML2/OVN driver. Read by ovn-northd. |
| **OVN Southbound DB (OVSDB)** | Stores physical bindings and compiled logical flows. Written by ovn-northd. Read by ovn-controller on each compute host. |

### Indexing Strategy

| Table | Index | Purpose |
|-------|-------|---------|
| ports | (network_id) | List ports in a network |
| ports | (device_id) | Find ports for an instance |
| ports | (binding_host_id) | Find all ports on a compute host (for agent restart reconciliation) |
| ports | (project_id) | List ports by project |
| ip_allocations | (subnet_id, ip_address) | IP uniqueness check; DHCP lease lookup |
| security_group_rules | (security_group_id) | Load all rules for a group |
| floating_ips | (fixed_port_id) | Check if port has a floating IP |
| network_segments | (network_id) | Look up segmentation for a network |

---

## 5. API Design

### REST Endpoints (OpenStack Networking API v2.0)

```
# Networks
POST   /v2.0/networks                                  # Create network
GET    /v2.0/networks                                  # List networks
GET    /v2.0/networks/{network_id}                     # Show network
PUT    /v2.0/networks/{network_id}                     # Update network
DELETE /v2.0/networks/{network_id}                     # Delete network

# Subnets
POST   /v2.0/subnets                                   # Create subnet
GET    /v2.0/subnets                                   # List subnets
GET    /v2.0/subnets/{subnet_id}                       # Show subnet
PUT    /v2.0/subnets/{subnet_id}                       # Update subnet
DELETE /v2.0/subnets/{subnet_id}                       # Delete subnet

# Ports
POST   /v2.0/ports                                     # Create port
GET    /v2.0/ports                                     # List ports
GET    /v2.0/ports/{port_id}                           # Show port
PUT    /v2.0/ports/{port_id}                           # Update port (bind, set SG)
DELETE /v2.0/ports/{port_id}                           # Delete port

# Routers
POST   /v2.0/routers                                   # Create router
GET    /v2.0/routers                                   # List routers
PUT    /v2.0/routers/{router_id}                       # Update router (set gateway)
DELETE /v2.0/routers/{router_id}                       # Delete router
PUT    /v2.0/routers/{router_id}/add_router_interface   # Add subnet to router
PUT    /v2.0/routers/{router_id}/remove_router_interface # Remove subnet

# Floating IPs
POST   /v2.0/floatingips                               # Create floating IP
GET    /v2.0/floatingips                               # List floating IPs
PUT    /v2.0/floatingips/{floatingip_id}               # Associate/disassociate
DELETE /v2.0/floatingips/{floatingip_id}               # Delete floating IP

# Security Groups
POST   /v2.0/security-groups                           # Create security group
GET    /v2.0/security-groups                           # List security groups
PUT    /v2.0/security-groups/{sg_id}                   # Update security group
DELETE /v2.0/security-groups/{sg_id}                   # Delete security group
POST   /v2.0/security-group-rules                      # Create rule
DELETE /v2.0/security-group-rules/{rule_id}            # Delete rule

# QoS Policies
POST   /v2.0/qos/policies                              # Create QoS policy
GET    /v2.0/qos/policies                              # List policies
POST   /v2.0/qos/policies/{policy_id}/bandwidth_limit_rules  # Add BW rule
POST   /v2.0/qos/policies/{policy_id}/dscp_marking_rules     # Add DSCP rule

# Trunk Ports
POST   /v2.0/trunks                                    # Create trunk
PUT    /v2.0/trunks/{trunk_id}/add_subports            # Add VLAN subport
PUT    /v2.0/trunks/{trunk_id}/remove_subports         # Remove subport

# Agents (admin)
GET    /v2.0/agents                                    # List agents
GET    /v2.0/agents/{agent_id}                         # Show agent
DELETE /v2.0/agents/{agent_id}                         # Delete agent

# Availability Zones
GET    /v2.0/availability_zones                         # List AZs

# Network IP Availability
GET    /v2.0/network-ip-availabilities                  # IP usage stats
```

### CLI (openstack CLI commands)

```bash
# Networks
openstack network create --provider-network-type vxlan private-net
openstack network create --provider-network-type vlan \
  --provider-physical-network physnet1 --provider-segment 100 provider-net
openstack network create --external --provider-network-type flat \
  --provider-physical-network physnet-ext external-net

# Subnets
openstack subnet create --network private-net --subnet-range 10.0.1.0/24 \
  --gateway 10.0.1.1 --dns-nameserver 8.8.8.8 --dhcp private-subnet

# Ports
openstack port create --network private-net --fixed-ip subnet=private-subnet \
  --security-group default my-port

# Routers
openstack router create my-router
openstack router set my-router --external-gateway external-net
openstack router add subnet my-router private-subnet

# Floating IPs
openstack floating ip create external-net
openstack server add floating ip my-vm 203.0.113.50

# Security Groups
openstack security group create web-servers
openstack security group rule create --protocol tcp --dst-port 80 \
  --remote-ip 0.0.0.0/0 web-servers
openstack security group rule create --protocol tcp --dst-port 443 \
  --remote-ip 0.0.0.0/0 web-servers
openstack security group rule create --protocol tcp --dst-port 22 \
  --remote-ip 10.0.0.0/8 web-servers

# QoS
openstack network qos policy create high-bandwidth
openstack network qos rule create --type bandwidth-limit \
  --max-kbps 10000000 --max-burst-kbps 10000000 high-bandwidth
openstack port set --qos-policy high-bandwidth <port-id>

# Trunks
openstack network trunk create --parent-port <port-id> my-trunk
openstack network trunk set --subport port=<subport-id>,segmentation-type=vlan,segmentation-id=100 my-trunk

# Agents
openstack network agent list
openstack network agent show <agent-id>
```

---

## 6. Core Component Deep Dives

### Deep Dive 1: ML2 Plugin Architecture — Type and Mechanism Drivers

**Why it's hard:**
The ML2 (Modular Layer 2) plugin must support multiple network segmentation technologies (VXLAN, VLAN, GRE, flat) and multiple backend implementations (OVS, OVN, SR-IOV, hardware switches) in a composable way. Type drivers and mechanism drivers must interoperate correctly, and the mechanism driver must reliably program the data plane on thousands of hosts.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| **Monolithic plugin (pre-ML2)** | Simple; single code path | Can't mix backends; vendor lock-in; one plugin per deployment |
| **ML2 with OVS mechanism driver** | Proven; widely deployed; supports all features | Requires agents on every host; agent-server sync issues; iptables for security groups is slow |
| **ML2 with OVN mechanism driver** | Distributed control plane; no need for L3/DHCP/metadata agents; native OVS integration; better performance | Newer; some features still catching up; OVSDB complexity |
| **ML2 with SR-IOV mechanism driver** | Hardware offload; near-native NIC performance | Limited to VF count on NIC; no security group support (in most NICs); complex PCI management |
| **Third-party SDN (Contrail, NSX, ACI)** | Full-featured SDN; hardware integration | Vendor lock-in; separate control plane; cost |

**Selected approach:** ML2 with OVN mechanism driver (primary) + SR-IOV (for high-performance workloads)

**Justification:** OVN replaces three separate agents (L3, DHCP, metadata) with a single ovn-controller per host, reducing operational complexity. OVN's distributed virtual routing eliminates the network node bottleneck. Its compiled logical flow model is more efficient than OVS agent's reactive flow programming. SR-IOV is added for workloads requiring line-rate performance (NFV, HPC).

**Implementation detail:**

```
ML2 Plugin Architecture:
━━━━━━━━━━━━━━━━━━━━━━

neutron-server
  └── ML2Plugin
      ├── TypeManager
      │   ├── VxlanTypeDriver      → allocates VNIs (1-16777215)
      │   ├── VlanTypeDriver       → allocates VLAN IDs per physical network
      │   ├── FlatTypeDriver       → no segmentation
      │   └── GreTypeDriver        → allocates GRE tunnel IDs
      │
      ├── MechanismManager
      │   ├── OVNMechanismDriver   → translates Neutron DB → OVN Northbound DB
      │   │   ├── create_network_precommit()   → validate
      │   │   ├── create_network_postcommit()  → write to OVN NB DB
      │   │   ├── create_port_precommit()      → validate
      │   │   ├── create_port_postcommit()     → create logical switch port in OVN
      │   │   ├── bind_port()                  → determine VIF type, set binding
      │   │   └── delete_port_postcommit()     → remove from OVN
      │   │
      │   └── SriovMechanismDriver → programs SR-IOV VF bindings
      │       ├── bind_port()      → allocate VF, set VLAN on VF
      │       └── (no agents needed; VF managed by hypervisor)
      │
      └── ExtensionManager
          ├── SecurityGroupExtension
          ├── QoSExtension
          ├── PortSecurityExtension
          └── TrunkExtension

OVN Architecture:
━━━━━━━━━━━━━━━━

  neutron-server (ML2/OVN driver)
        │
        ▼ (writes to)
  ┌─────────────────────┐
  │  OVN Northbound DB  │   ← Logical topology: switches, routers, ACLs
  │  (ovsdb-server)     │      Raft cluster for HA (3 nodes)
  └──────────┬──────────┘
             │
             ▼ (reads and compiles)
  ┌─────────────────────┐
  │     ovn-northd      │   ← Compiles logical topology into logical flows
  │  (active-standby)   │      Similar to a network compiler
  └──────────┬──────────┘
             │
             ▼ (writes to)
  ┌─────────────────────┐
  │  OVN Southbound DB  │   ← Compiled flows + physical bindings
  │  (ovsdb-server)     │      Raft cluster for HA (3 nodes)
  └──────────┬──────────┘
             │
    ┌────────┼────────┐
    ▼        ▼        ▼  (reads and programs)
┌────────┐┌────────┐┌────────┐
│ovn-    ││ovn-    ││ovn-    │   ← One per compute host
│control-││control-││control-│      Translates logical flows → OVS flows
│ler     ││ler     ││ler     │      Programs br-int via OpenFlow
│(host1) ││(host2) ││(host3) │
└────────┘└────────┘└────────┘
```

**Failure modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| OVN Northbound DB leader failure | Writes fail until new leader elected (Raft) | 3-node Raft cluster; election in < 5s; reads continue from followers |
| ovn-northd crash | No new logical flow compilation; existing flows unaffected | Active-standby; standby takes over automatically |
| ovn-controller crash on compute host | New port bindings fail on that host; existing VM connectivity preserved (OVS flows persist) | Systemd auto-restart; OVS flows survive controller restart |
| OVN Southbound DB full replication lag | Stale flows on some hosts; potential connectivity issues | Monitor Raft log replication lag; size OVSDB appropriately |
| ML2 mechanism driver postcommit failure | Neutron DB has record but OVN doesn't; inconsistency | Neutron maintenance task reconciles; OVN audit mechanism |

**Interviewer Q&As:**

**Q1: What is the difference between OVS and OVN, and why prefer OVN?**
A: OVS (Open vSwitch) is a virtual switch that forwards packets based on OpenFlow rules. With the ML2/OVS driver, neutron-openvswitch-agent runs on each host and programs OVS flows reactively (via RPC from neutron-server). OVN is a control plane built on top of OVS. It has its own distributed database (OVSDB), a compiler (ovn-northd), and a per-host controller (ovn-controller). OVN eliminates the need for separate L3, DHCP, and metadata agents. It provides native distributed routing, stateful ACLs (replacing iptables), and DHCP responder (replacing dnsmasq). OVN is preferred because it reduces agent count, improves performance (compiled flows vs reactive), and scales better.

**Q2: How does ML2 handle a deployment that needs both OVN and SR-IOV?**
A: ML2 supports multiple mechanism drivers simultaneously. When a port is created, ML2 calls `bind_port()` on each mechanism driver in order. The first driver that successfully binds the port wins. For SR-IOV ports (vnic_type=direct), the SriovMechanismDriver binds (allocates a VF). For normal ports, OVN binds. The type driver (VXLAN or VLAN) is shared. Neutron stores the binding result (vif_type, vif_details) on the port. Nova reads these details to configure the VM's network interface (OVS port or PCI passthrough device).

**Q3: How does VXLAN segmentation work at the OVS level?**
A: Each tenant network is assigned a unique VXLAN Network Identifier (VNI, 24-bit, up to 16M networks). On each compute host, br-int uses local VLAN tags for internal isolation (e.g., network A = VLAN 100, network B = VLAN 200). br-tun has flow rules that map local VLANs to/from VNIs: outgoing traffic matches local VLAN, strips it, encapsulates in VXLAN with the correct VNI, and sends to the destination host's VTEP (VXLAN Tunnel Endpoint) IP. Incoming traffic matches the VNI, strips VXLAN, adds the local VLAN tag, and sends to br-int. The mapping is per-host (VLAN 100 on Host-1 and VLAN 200 on Host-2 might both map to VNI 5001).

**Q4: What is the precommit/postcommit pattern in ML2?**
A: ML2 mechanism drivers have two phases for each operation. `precommit` runs inside the DB transaction — it can validate and raise exceptions to abort the transaction, but must NOT make external calls (no RPC, no external DB writes). `postcommit` runs after the DB transaction commits — it propagates the change to the backend (OVN DB, agent RPC). If postcommit fails, the Neutron DB already has the record, creating inconsistency. Mitigation: neutron's maintenance task periodically reconciles Neutron DB with the backend. This two-phase design ensures DB consistency while allowing best-effort backend synchronization.

**Q5: How does SR-IOV bypass the virtual switch for better performance?**
A: With SR-IOV, the physical NIC is partitioned into Virtual Functions (VFs), each appearing as a separate PCI device. A VF is passed directly to the VM via PCI passthrough (IOMMU/VT-d). Traffic goes directly from the VM to the physical NIC hardware, bypassing OVS entirely. The NIC hardware handles VLAN tagging and MAC filtering. Benefits: near-native throughput, low latency, reduced CPU overhead. Drawbacks: limited VFs per NIC (typically 64-128), limited security group support (hardware ACLs are basic), no VXLAN (unless NIC supports offload), live migration is more complex (VF must be detached/reattached).

**Q6: How are VNIs allocated and managed across a large deployment?**
A: The VxlanTypeDriver maintains a pool of available VNIs in the Neutron DB (configured via `vni_ranges` in ml2_conf.ini, e.g., `vni_ranges = 1:16000000`). When a new VXLAN network is created, the type driver allocates the next available VNI from the pool (stored in `ml2_vxlan_allocations` table). VNIs are global — the same VNI means the same network everywhere. Deallocation happens when the network is deleted. At 20,000 networks, only 0.12% of the VNI space is used, so exhaustion is not a concern. VLAN is more constrained (4094 IDs per physical network), which is why VXLAN is preferred for tenant networks.

---

### Deep Dive 2: Distributed Virtual Routing (DVR)

**Why it's hard:**
Without DVR, all north-south traffic (VM to/from internet) and all inter-subnet east-west traffic must traverse centralized L3 agents on dedicated network nodes. This creates bandwidth bottlenecks, added latency, and a single point of failure. DVR distributes routing to every compute host, but this requires complex flow management, distributed ARP handling, and coordination of floating IP NAT across hosts.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| **Centralized L3 agent (legacy)** | Simple; single routing namespace | Bottleneck for all routed traffic; extra hop; SPOF |
| **Centralized L3 with HA (VRRP)** | HA via active-standby; automatic failover | Still a bottleneck; failover takes 10-30s; all traffic through network node |
| **DVR (Distributed Virtual Router)** | East-west and floating IP traffic stays local; no bottleneck | Complex; requires SNAT on network node for non-floating-IP traffic; ARP proxy needed; more OVS flows |
| **OVN native routing** | Built-in distributed routing; no separate agents; simpler than DVR | Requires OVN stack; different operational model |

**Selected approach:** OVN native distributed routing (replaces legacy DVR)

**Justification:** OVN provides distributed routing natively without the complexity of legacy DVR. ovn-northd compiles L3 routing rules into logical flows, and ovn-controller programs them as OVS flows on each compute host. No router namespaces needed. For deployments still on ML2/OVS, legacy DVR is the fallback.

**Implementation detail:**

```
OVN Distributed Routing Flow:
━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Scenario: VM-A (10.0.1.5, subnet-1) → VM-B (10.0.2.10, subnet-2), same host

  1. VM-A sends packet to gateway (10.0.1.1, router port MAC)
  2. OVS flow on br-int matches dst MAC = router port MAC
  3. OVS decrements TTL, changes src MAC to router port MAC for subnet-2
  4. OVS does L3 lookup: dst 10.0.2.10 matches subnet-2 (10.0.2.0/24)
  5. ARP lookup for 10.0.2.10 → VM-B's MAC (cached in OVS flows from OVN)
  6. Sets dst MAC = VM-B's MAC
  7. Forwards directly to VM-B's tap port on br-int
  
  Total hops: 0 (same host), or 1 VXLAN hop (cross-host)
  No router namespace, no network node involved!

Scenario: VM-A (10.0.1.5) → Internet (floating IP 203.0.113.50)

  With OVN distributed routing:
  1. VM-A sends packet to gateway (10.0.1.1)
  2. OVS flow matches dst MAC = gateway, does L3 routing
  3. OVS matches floating IP NAT rule: SNAT 10.0.1.5 → 203.0.113.50
  4. Packet sent out br-ex on the COMPUTE host directly to physical network
  5. Return traffic: arrives at compute host br-ex, DNAT 203.0.113.50 → 10.0.1.5
  
  No network node for floating IP traffic!

Scenario: VM without floating IP → Internet (SNAT via network node)

  1. VM-A sends packet to gateway
  2. OVS flow routes to external network
  3. Since VM has no floating IP, traffic must use router's SNAT
  4. Packet encapsulated in VXLAN, sent to "gateway chassis" (network node)
  5. Network node performs SNAT using router's external IP
  6. Packet exits to Internet via network node's br-ex
  
  This is the only case requiring the network node!

Legacy DVR Implementation (ML2/OVS):
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

On each compute host hosting VMs from a routed network:
  - neutron-l3-agent creates a DVR router namespace (qrouter-<uuid>)
  - Router namespace has interfaces on each connected subnet
  - OVS flows redirect inter-subnet traffic into the router namespace
  - Floating IPs: router namespace has the floating IP; NAT happens locally
  - SNAT traffic: forwarded to a centralized "dvr_snat" namespace on network node

ARP Proxy (DVR-specific):
  - Problem: when VM-B on subnet-2 is on a different host, the local router
    namespace needs to respond to ARP for VM-B's IP
  - Solution: dvr_fip agent responds to ARP with the router port's MAC
  - The actual forwarding is handled by VXLAN to the remote host
```

**Failure modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| OVN gateway chassis (network node) failure | SNAT traffic interrupted; floating IP traffic unaffected (distributed) | Multiple gateway chassis with HA scheduling; OVN BFD-based failover < 5s |
| ovn-controller crash on compute host | New flow programming stops; existing flows in OVS persist; VMs keep working | Systemd restart; OVS flows survive controller restart; state rebuilt from Southbound DB |
| br-ex misconfiguration on compute host | Floating IP traffic can't exit; east-west traffic unaffected | Health check: ping external gateway from compute host; alert on failure |
| ARP table overflow in OVN | Connectivity issues for large networks | Tune ARP/ND timeout settings; OVN handles ARP suppression to limit broadcast |
| Gateway chassis overloaded (too much SNAT traffic) | SNAT latency increases | Add more gateway chassis; encourage floating IP usage; monitor bandwidth per gateway |

**Interviewer Q&As:**

**Q1: Why can't all north-south traffic be distributed — why is SNAT still centralized?**
A: Floating IPs provide 1:1 NAT with a dedicated public IP per VM, so return traffic naturally arrives at the correct compute host. But SNAT (many-to-one NAT) uses a single shared IP for all VMs on the router. Return traffic arrives at the NAT IP and must be de-NATted — the connection tracking state for the SNAT lives on the network node. Distributing SNAT would require distributed connection tracking across all compute hosts, which is extremely complex and has been explored (SNAT conntrack sync) but isn't production-ready. The practical solution: encourage floating IPs for VMs that need external access.

**Q2: How does OVN handle ARP for distributed routing without flooding?**
A: OVN programs ARP responder flows directly into OVS on each compute host. When a VM sends an ARP request for the router's gateway IP, OVS responds locally with the router's MAC address — no broadcast needed. For VM-to-VM ARP across subnets, OVN's logical router handles L3 forwarding, so ARP is only needed within the same L2 segment. OVN also proactively programs ARP entries for known ports (GARP on port creation), eliminating most ARP broadcasts. This is a major advantage over legacy DVR, which used ARP proxy hacks.

**Q3: How does DVR handle live migration of a VM with a floating IP?**
A: During live migration, the VM's floating IP must follow it to the destination host. With OVN: (1) ovn-controller on the destination host detects the port binding change (via Southbound DB update). (2) It programs the NAT flows on the destination host's OVS and updates br-ex to attract the floating IP's traffic. (3) Gratuitous ARP is sent from the destination host for the floating IP, updating the physical network's ARP caches. (4) Return traffic converges on the new host within seconds. The window of disruption is typically < 1 second, overlapping with the VM's memory migration downtime.

**Q4: What is the performance impact of VXLAN encapsulation?**
A: VXLAN adds 50 bytes of overhead (outer Ethernet 14 + outer IP 20 + UDP 8 + VXLAN 8). With a standard 1500-byte MTU, effective payload is reduced to 1450 bytes, causing fragmentation for packets at the original MTU. Solution: jumbo frames (MTU 9000) on the physical network; tenant networks advertise MTU 8950 via DHCP. CPU overhead: VXLAN encap/decap in OVS kernel datapath is ~5% CPU per Gbps. Hardware offload (most modern NICs): VXLAN checksum and segmentation offload eliminates CPU overhead. With offload, throughput is typically > 95% of wire speed.

**Q5: How do you troubleshoot connectivity between two VMs on different subnets?**
A: Systematic approach: (1) Verify both VMs have correct IPs: `openstack port show` for each. (2) Check security groups allow the traffic: `openstack security group rule list`. (3) Verify router connects both subnets: `openstack router show`, check interfaces. (4) Check OVS flows on both hosts: `ovs-ofctl dump-flows br-int | grep <MAC>`. (5) With OVN: `ovn-trace` simulates a packet through the logical network — shows every pipeline stage and decision. (6) Check `ovn-sbctl show` for port bindings — ensure ports are bound to correct chassis. (7) `tcpdump` on br-int or tap interfaces as last resort.

**Q6: How does OVN compare to commercial SDN solutions like VMware NSX or Cisco ACI?**
A: OVN is open-source, community-maintained, and tightly integrated with OVS (the most widely deployed virtual switch). It provides L2 switching, L3 routing, NAT, DHCP, ACLs (security groups), load balancing (basic), and QoS. Commercial SDNs add: (1) GUI/management plane, (2) hardware VTEP integration, (3) micro-segmentation with deeper policy models, (4) multi-hypervisor support, (5) vendor support and SLAs. For an OpenStack deployment, OVN covers 90%+ of networking needs without licensing costs. Commercial SDN is justified when you need hardware switch integration, advanced network services, or vendor support guarantees.

---

### Deep Dive 3: Security Groups — Stateful Firewalling

**Why it's hard:**
Security groups must implement stateful firewalling for every port in the deployment — potentially 500,000+ ports across 10,000 hosts. Rules must be evaluated at line rate with minimal CPU overhead. State tracking (connection tracking) must handle millions of concurrent connections. Rule updates must propagate to all affected hosts within seconds. And the system must guarantee zero cross-tenant traffic leakage.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| **iptables (legacy OVS agent)** | Mature; well-understood; stateful | Slow rule updates (iptables-save/restore for every change); poor scaling beyond 1000 rules; CPU overhead at high packet rates |
| **OVS conntrack (newer OVS agent)** | OVS-native; faster rule matching; offloadable to hardware | More complex OVS flow rules; requires OVS 2.5+ with conntrack support |
| **OVN ACLs** | Compiled into OVS flows; distributed; no iptables dependency; conntrack-based | Requires OVN stack |
| **Hardware offload (SmartNIC)** | Line-rate filtering; zero CPU overhead | Expensive; limited rule capacity; vendor-specific; not all NIC models support conntrack offload |

**Selected approach:** OVN ACLs with OVS conntrack

**Justification:** OVN compiles security group rules into OVS flows with conntrack actions. This is faster than iptables, scales better, and can be hardware-offloaded to SmartNICs. OVN ACLs support remote security group references, which enable rules like "allow SSH from members of SG-admin."

**Implementation detail:**

```
Security Group Rule Processing:
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

1. User creates rule: "Allow TCP port 443 from 0.0.0.0/0 to SG:web-servers"
   
   neutron-server → Neutron DB (security_group_rules table)
   ML2/OVN driver → OVN Northbound DB: creates ACL on logical switch

2. OVN Northbound DB ACL entry:
   {
     "direction": "to-lport",           # ingress to the port
     "match": "outport == @pg_web_servers && tcp && tcp.dst == 443",
     "action": "allow-related",         # allow + conntrack (stateful)
     "priority": 1000
   }
   
   Note: @pg_web_servers is a port group — all ports in the security group

3. ovn-northd compiles ACL → logical flows in Southbound DB

4. ovn-controller on each compute host translates logical flows → OVS flows:
   
   # Ingress pipeline for port in web-servers SG:
   
   # Track new connections (conntrack)
   table=0, priority=100, ct_state=-trk, action=ct(table=1)
   
   # Allow established/related connections
   table=1, priority=65535, ct_state=+est+trk, action=output:<port>
   table=1, priority=65535, ct_state=+rel+trk, action=output:<port>
   
   # Allow new TCP 443 from any IP
   table=1, priority=1000, ct_state=+new+trk, tcp, tcp_dst=443,
     action=ct(commit),output:<port>
   
   # Default deny
   table=1, priority=0, action=drop

5. Return traffic (VM responds on TCP 443):
   - Conntrack marks the connection as established
   - Egress flow allows ct_state=+est+trk automatically
   - No explicit egress rule needed for established connections (stateful!)

Remote Security Group Reference:
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

Rule: "Allow SSH from SG:admin-servers"
  
  OVN implementation:
  - OVN creates an address set: $as_admin_servers = {10.0.1.5, 10.0.1.6, 10.0.2.3}
  - ACL match: "ip4.src == $as_admin_servers && tcp && tcp.dst == 22"
  - When a new port joins SG:admin-servers, OVN updates the address set
  - ovn-controller reprograms OVS flows on all affected hosts
  
  Propagation time: typically < 3 seconds for address set update

Conntrack Scaling:
━━━━━━━━━━━━━━━━━

Default conntrack table size per host: 262,144 entries
Recommended for cloud: 1,048,576 (via sysctl net.netfilter.nf_conntrack_max)

With 40 VMs per host, 100 connections per VM average:
  40 x 100 = 4,000 entries (well within capacity)

Peak (web servers): 40 VMs x 10,000 connections = 400,000 entries
  Increase to 1M or 2M for compute hosts running web-facing VMs
```

**Failure modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| OVN ACL not propagated to compute host | Port is unreachable (default deny) or too permissive | Monitor ovn-controller sync lag; health check verifies flow programming |
| Conntrack table full | New connections dropped even if rules allow them | Monitor conntrack entries; tune nf_conntrack_max; alert at 80% |
| Security group rule update race condition | Brief window of incorrect filtering | OVN's transactional model ensures atomic ACL updates |
| Default security group missing | Port has no rules; default deny blocks all traffic | Neutron auto-creates default SG with allow-all-egress and allow-same-SG-ingress |
| Anti-spoofing bypass attempt | Tenant sends packets with spoofed MAC/IP | Port security enforced by OVS flows: match src MAC == port MAC, src IP == port IP; drop mismatches |

**Interviewer Q&As:**

**Q1: How are security groups stateful, and what does that mean for performance?**
A: "Stateful" means that if an ingress rule allows TCP port 443, the return traffic (from the VM back to the client) is automatically allowed without an explicit egress rule. This is implemented via OVS conntrack (ct_state tracking). When a new connection matching an allow rule arrives, conntrack creates a state entry. Subsequent packets in the same connection are matched by ct_state=+est (established) and fast-pathed. Performance impact: conntrack adds ~5-10% CPU overhead per packet vs stateless filtering. For high-throughput VMs, consider using `no_security_groups` port extension for trusted internal networks.

**Q2: What happens when a security group has 1000 rules?**
A: Each rule becomes one or more OVS flow entries. With 1000 rules per SG and 40 VMs per host, that's up to 40,000 additional flows just for security groups. OVS handles this well — its flow table is implemented as a hash table, so lookup time is O(1) regardless of table size (up to millions of flows). However, flow programming time increases linearly. OVN mitigates this by using port groups and address sets (a single flow can reference an address set with thousands of IPs). Best practice: use address sets and CIDR prefixes instead of many individual IP rules.

**Q3: How does port security (anti-spoofing) work?**
A: By default, Neutron enables port security on every port. OVN/OVS programs flows that: (1) on egress from VM: match src MAC must equal the port's MAC address, src IP must be in the port's allowed-address-pairs (default: only the port's own IP). Drop if mismatch. (2) Prevent DHCP server spoofing: drop DHCP offer packets from VMs. (3) Prevent ARP spoofing: ARP replies must have the port's MAC/IP. This prevents a malicious VM from impersonating other VMs or the gateway. Port security can be disabled per-port (`port_security_enabled=False`) for special cases like NFV.

**Q4: How do remote security group references scale?**
A: A remote SG reference (e.g., "allow from SG:database-servers") creates an address set containing all IPs of ports in that SG. When the SG has 500 members, the address set has 500 IPs. OVN compiles this into a single OVS flow with a `conjunction` match or a bitmap lookup — not 500 separate flows. When a port is added/removed from the remote SG, OVN updates the address set (a single OVSDB transaction), and ovn-controller incrementally updates the affected flows. This is much more efficient than the legacy iptables approach, which created individual iptables rules per IP.

**Q5: Can security groups be hardware-offloaded?**
A: Yes, with OVS hardware offload (TC flower offload) on supported SmartNICs (NVIDIA ConnectX, Broadcom Stingray). The OVS flows including conntrack actions are offloaded to the NIC's embedded switch. This provides line-rate security group enforcement with zero CPU overhead. Limitations: (1) conntrack offload requires NIC firmware support, (2) maximum flow/conntrack table size is limited by NIC memory (typically 100K-1M entries), (3) not all OVS actions can be offloaded (complex matches may fall back to software). Best practice: offload common flows, keep edge cases in software.

**Q6: How do you audit security group effectiveness — ensuring no unauthorized traffic passes?**
A: (1) Use `ovn-trace` to simulate packets and verify ACL hit/miss for specific traffic patterns. (2) Enable OVS flow statistics (`ovs-ofctl dump-flows` shows packet/byte counts per flow) — check that deny rules are not receiving unexpected traffic. (3) Neutron log (`neutron-logging` extension): logs accepted and dropped packets per security group rule to syslog. (4) Tempest security group tests: automated tests that verify isolation between tenant networks. (5) Network policy validation: use tools like `neutron-policy-check` to verify that the intended policy matches the deployed flows.

---

### Deep Dive 4: Floating IPs and NAT

**Why it's hard:**
Floating IPs provide the primary mechanism for VMs to be reachable from external networks. They require 1:1 NAT (DNAT for incoming, SNAT for outgoing), gratuitous ARP updates to the physical network, coordination during live migration, and efficient handling of high-throughput traffic. In a DVR/OVN setup, the NAT happens on the compute host itself, requiring careful flow management.

**Approaches:**

| Approach | Pros | Cons |
|----------|------|------|
| **Centralized NAT on network node** | Simple; single point of NAT | Bottleneck; extra latency; SPOF |
| **DVR floating IP (distributed NAT)** | NAT on compute host; no bottleneck; low latency | Complex; requires br-ex on every compute host (or proxy ARP); GARP storms on migration |
| **OVN distributed NAT** | Native OVS NAT; no router namespace; compiled flows | Requires OVN; br-ex needed on compute hosts hosting floating IPs |
| **BGP-based floating IP (Calico-style)** | No NAT; BGP advertises host routes for floating IPs; truly distributed | Requires BGP on every compute host; more physical network complexity; not standard OpenStack |

**Selected approach:** OVN distributed NAT

**Implementation detail:**

```
Floating IP Association Flow:
━━━━━━━━━━━━━━━━━━━━━━━━━━━━

1. User: openstack floating ip create external-net
   → Neutron allocates 203.0.113.50 from external subnet pool
   → Creates floating_ips record (status=DOWN)
   → Creates port on external network (floating_port_id)

2. User: openstack server add floating ip my-vm 203.0.113.50
   → Neutron associates floating IP with VM's fixed port
   → Updates floating_ips: fixed_port_id=<vm-port>, router_id=<router>
   → ML2/OVN driver writes to OVN Northbound DB:
     - NAT entry on logical router: dnat_and_snat, 
       external_ip=203.0.113.50, logical_ip=10.0.1.5

3. ovn-northd compiles NAT → logical flows in Southbound DB:
   - Ingress to router: if ip4.dst == 203.0.113.50, 
     DNAT to 10.0.1.5, forward to logical switch port
   - Egress from router: if ip4.src == 10.0.1.5 AND via external,
     SNAT to 203.0.113.50

4. ovn-controller on compute host programs OVS flows:
   
   # DNAT: External → VM
   table=ingress_dnat, ip4.dst=203.0.113.50,
     action=ct(nat(dst=10.0.1.5), table=next)
   
   # SNAT: VM → External
   table=egress_snat, ip4.src=10.0.1.5, outport=external,
     action=ct(nat(src=203.0.113.50), table=next)

5. Gratuitous ARP:
   - ovn-controller sends GARP for 203.0.113.50 on br-ex
   - Physical switches update their MAC tables
   - Traffic to 203.0.113.50 now arrives at this compute host

6. OVN "gateway chassis" scheduling:
   - If the compute host doesn't have br-ex (no external connectivity),
     OVN schedules the floating IP's NAT on a "gateway chassis" (network node)
   - Traffic is tunneled from compute host to gateway chassis for NAT
   - With br-ex on compute: NAT is truly distributed

Live Migration with Floating IP:
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

1. VM migrated from Host-A to Host-B
2. Port binding updates in OVN Southbound DB: chassis = Host-B
3. ovn-controller on Host-B programs NAT flows
4. ovn-controller on Host-B sends GARP for floating IP on its br-ex
5. Physical switches update: 203.0.113.50 now reachable via Host-B
6. ovn-controller on Host-A removes NAT flows
7. Convergence time: typically < 1 second (overlaps with migration downtime)
```

**Failure modes:**

| Failure | Impact | Mitigation |
|---------|--------|------------|
| GARP lost (switch didn't update ARP) | Traffic still sent to old host | Periodic GARP retransmission; configurable GARP count and interval |
| Floating IP NAT not programmed (ovn-controller lag) | VM unreachable from external | Monitor port binding status; OVN health check |
| External network exhaustion | Can't create more floating IPs | Monitor IP pool usage; add more external subnets; alert at 80% |
| br-ex down on compute host | Floating IP traffic fails; east-west unaffected | Health check br-ex; OVN can reschedule to gateway chassis |

**Interviewer Q&As:**

**Q1: What is the difference between floating IPs and provider networks for external access?**
A: Floating IPs provide 1:1 NAT: the VM has a private IP (e.g., 10.0.1.5) and a public IP (203.0.113.50) mapped via NAT on the router. The VM itself doesn't know about the floating IP. Provider networks directly attach VMs to the physical network — the VM gets a public IP directly on its interface (no NAT). Provider networks are simpler (no NAT overhead) but expose VMs directly to the external network, providing less isolation. Floating IPs are preferred for multi-tenant clouds because they decouple internal addressing from external.

**Q2: How many floating IPs can a single deployment support?**
A: Limited by the external subnet pool. A /16 gives ~65K IPs. Multiple external subnets can be added to the external network. The DB and flow overhead per floating IP is minimal (one row, a few OVS flows). At 50,000 floating IPs, the main concern is GARP traffic during mass migration events. In practice, floating IPs are also constrained by cost (public IPs are expensive) and security policy (fewer external-facing VMs = smaller attack surface).

---

## 7. Scheduling & Resource Management

### Neutron Resource Scheduling

```
DHCP Agent Scheduling:
━━━━━━━━━━━━━━━━━━━━━

- dhcp_agents_per_network = 3 (HA: 3 agents serve each network)
- Scheduler algorithm: least-networks (balance networks across agents)
- When a network is created, scheduler selects 3 DHCP agents
- Each agent runs a dnsmasq process in a network namespace
- If an agent dies, DHCP still served by remaining 2 agents
- Automatic rescheduling if agent is removed

L3 Agent Scheduling:
━━━━━━━━━━━━━━━━━━━

Centralized routers:
  - neutron-l3-agent runs on network nodes
  - Scheduler assigns router to least-loaded L3 agent
  - HA routers: VRRP (keepalived) with active-standby pair
  - l3_ha = true: 2 L3 agents manage each router (active + standby)

DVR routers:
  - Router namespace created on every compute host with connected VMs
  - dvr_snat namespace on network node for centralized SNAT
  - No scheduling decision — distributed by design

OVN gateway chassis scheduling:
  - OVN schedules router gateway ports on gateway chassis (network nodes)
  - Multiple gateway chassis with priority-based failover
  - BFD (Bidirectional Forwarding Detection) for fast failure detection
  - Failover time: < 5 seconds

Network Scheduling Considerations:
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

IP Address Management (IPAM):
  - Built-in IPAM driver (default): subnet pool managed in Neutron DB
  - IP allocation: SELECT from ip_allocations WHERE subnet_id=? 
    to find used IPs, allocate next available from pool
  - Deallocation on port delete: removes ip_allocations row
  - DHCP lease: dnsmasq reads from Neutron DB (via agent)
  
  For large subnets (/16 = 65K IPs):
  - IP allocation becomes slow with many used IPs
  - Optimization: maintain a separate table of available IPs
  - Consider using external IPAM (Infoblox plugin)

Port Binding:
  - When VM boots, nova-compute tells Neutron which host the port is on
  - ML2 mechanism driver binds the port:
    1. OVN: creates logical switch port in OVN NB DB with chassis hint
    2. SR-IOV: allocates VF on the NIC
  - ovn-controller detects new port, programs OVS flows
  - Port status changes: DOWN → BUILD → ACTIVE
```

---

## 8. Scaling Strategy

### Neutron Scaling Dimensions

| Dimension | Strategy | Limit |
|-----------|----------|-------|
| API throughput | Multiple neutron-server instances behind HAProxy | Stateless; horizontal scaling |
| DB writes (port create) | MySQL Galera; batch operations; oslo.db retry | ~5,000 writes/sec per Galera cluster |
| OVN Northbound DB | 3-node Raft cluster; scale reads to followers | ~10,000 writes/sec |
| OVN Southbound DB | 3-node Raft cluster; ovn-controller reads from local copy | Each host reads only its own chassis flows |
| DHCP agents | Scale horizontally; 3 agents per network | ~1,000 networks per agent |
| Flow programming | ovn-controller is per-host; only programs local flows | ~50,000 flows per host |
| Security group updates | OVN address sets: single update propagates to all hosts | Eventual consistency < 3 seconds |

### Interviewer Q&As

**Q1: How does Neutron scale to support 500,000 ports?**
A: The Neutron DB is the primary bottleneck. With Galera, we get ~5,000 writes/sec, handling 100 port creates/sec with headroom. For reads (list operations), MySQL handles millions of rows with proper indexing. OVN's Southbound DB scales by design: each ovn-controller only subscribes to flows relevant to its chassis (via OVSDB monitor with condition). This means a host with 40 VMs only tracks ~2,000 flows, not 500,000. The key insight: Neutron's data plane scales linearly with hosts (each host manages its own flows), while the control plane (DB) is centralized but handles metadata only.

**Q2: What is the impact of having 10,000 security groups with remote group references?**
A: Remote group references create address sets in OVN. Each address set contains the IPs of all ports in that security group. With 10,000 SGs, OVN manages 10,000 address sets. The concern is update fan-out: when a port is added to SG-A, the address set for SG-A is updated, and all hosts with ports that reference SG-A as a remote group must reprogram their flows. With proper OVN event-driven updates (incremental processing in ovn-controller since OVN 20.06), this is efficient — only affected flows are reprogrammed, not the entire flow table.

**Q3: How do you handle a scenario where OVN Southbound DB becomes very large?**
A: The Southbound DB contains logical flows and chassis bindings. At 10,000 hosts with 500K ports, it can reach 500MB-1GB. OVSDB Raft replication handles this but log compaction is important. Mitigation: (1) Enable OVSDB compaction (periodic snapshots that truncate the Raft log). (2) Tune `inactivity_probe` and `max_backoff` settings for ovn-controller connections. (3) Use conditional monitoring (ovn-controller only monitors flows for its chassis). (4) In extreme cases, partition the deployment into multiple OVN clusters (similar to Nova cells), each managing a subset of networks.

**Q4: How does Neutron handle burst traffic (e.g., 1,000 VMs created simultaneously)?**
A: Each VM create triggers 1-2 port creates in Neutron. 1,000 simultaneous creates = 1,000-2,000 port create API calls. Neutron-server handles these in parallel (multiple workers). DB writes are batched via SQLAlchemy session. OVN NB DB writes are coalesced by ovn-northd. ovn-controller on each host processes new ports asynchronously. The bottleneck is typically the DB (port create requires IP allocation with uniqueness constraint). Mitigation: pre-create ports before VM boot, use bulk port create API, tune DB connection pool size.

**Q5: How do you monitor the health of the Neutron data plane across 10,000 hosts?**
A: (1) OVN Southbound DB `Chassis` table: each ovn-controller registers and updates its row periodically. Missing heartbeat = dead host. (2) Neutron agent heartbeat: `openstack network agent list` shows agent status (alive/dead based on heartbeat). (3) Per-host metrics: OVS flow count, conntrack entries, bridge stats (ovs-vsctl get-stats). (4) End-to-end connectivity tests: deploy a monitoring VM in each AZ that pings VMs in all other AZs. Alert on ping failure. (5) Prometheus + openstack-exporter: scrape Neutron API for port/network/router status.

---

## 9. Reliability & Fault Tolerance

### Failure Scenarios

| # | Failure Scenario | Impact | Detection | Recovery | RTO |
|---|-----------------|--------|-----------|----------|-----|
| 1 | neutron-server instance crash | Reduced API capacity | HAProxy health check | HAProxy routes to healthy instances | < 10s |
| 2 | OVN Northbound DB leader fails | Writes fail; reads from followers continue | Raft heartbeat timeout | Automatic leader election (< 5s) | < 5s |
| 3 | ovn-northd crash | No new logical flow compilation | Process monitor / systemd | Standby takes over; or systemd restart | < 10s |
| 4 | ovn-controller crash on compute host | New flow programming stops on that host; existing flows persist in OVS | OVN heartbeat in Southbound DB | Systemd auto-restart; flows rebuilt from Southbound DB | < 30s |
| 5 | OVS crash on compute host | ALL VM connectivity on host lost | OVS heartbeat; VM connectivity check | Systemd restart OVS; flows reprogrammed by ovn-controller | < 60s |
| 6 | DHCP agent crash | Lease renewals fail for networks on that agent; VMs keep existing leases | Agent heartbeat in Neutron DB | Other DHCP agents serve the network (dhcp_agents_per_network=3) | < 10s |
| 7 | Network node failure (gateway chassis) | SNAT traffic fails; floating IPs on other hosts unaffected | BFD (OVN) or VRRP (legacy) | OVN reschedules gateway ports to another chassis | < 5s (BFD) |
| 8 | Physical link failure (uplink to spine) | Reduced bandwidth; possible packet loss | LACP/BFD detection | ECMP/LAG failover in leaf-spine; OVS bond failover | < 1s (LACP) |
| 9 | VXLAN tunnel failure between two hosts | VM-to-VM cross-host connectivity lost for those hosts | BFD on VXLAN tunnels (OVN) | OVN removes failed tunnel from forwarding; physical repair | Varies |
| 10 | MySQL Galera cluster quorum loss | All Neutron API writes fail | Galera monitoring | Bootstrap from most advanced node | 5-15 min |
| 11 | IP address pool exhaustion | No new ports can be created | Monitor subnet utilization | Add new subnets; resize existing | Minutes |
| 12 | Conntrack table exhaustion on host | New connections dropped despite SG allowing them | sysctl monitoring | Increase nf_conntrack_max; identify source of connections | < 5 min |

### OpenStack-Specific HA Patterns

| Pattern | Implementation |
|---------|---------------|
| **DHCP HA** | 3 DHCP agents per network; dnsmasq in each; failover is seamless (VM retries DHCP discover) |
| **L3 HA (legacy)** | VRRP with keepalived; active-standby router pair; failover in ~10s |
| **OVN Gateway HA** | Multiple gateway chassis per logical router; BFD for failure detection; failover in < 5s |
| **OVS Bond** | LACP bond on physical NICs; survives single NIC failure; sub-second failover |
| **OVSDB Raft** | 3-node Raft cluster for NB and SB databases; survives single node failure |
| **neutron-server HA** | Multiple instances behind HAProxy; stateless; round-robin distribution |

---

## 10. Observability

### Key Metrics

| Category | Metric | Source | Alert Threshold |
|----------|--------|--------|-----------------|
| **API** | neutron_api_request_duration_seconds | StatsD | P99 > 2s |
| **API** | neutron_api_requests_total (by status) | StatsD | Error rate > 1% |
| **Ports** | total_ports (by status) | DB query / exporter | Unexpected drops |
| **Ports** | port_binding_duration_seconds | StatsD | P99 > 5s |
| **Networks** | subnet_ip_utilization_percent | Neutron API | > 80% |
| **OVN** | ovn_nb_db_raft_leader_changes | OVSDB metrics | > 2/hour |
| **OVN** | ovn_sb_db_chassis_count | OVSDB query | < expected host count |
| **OVN** | ovn_controller_flow_count_per_host | OVS stats | > 100,000 (unusual) |
| **OVN** | ovn_controller_recompute_duration | OVN metrics | > 1s |
| **OVS** | ovs_dp_flows (datapath flows) | ovs-dpctl | > 1M per host |
| **DHCP** | dhcp_lease_failures | dnsmasq logs | > 0 |
| **L3** | floating_ip_status (DOWN count) | Neutron API | > 0 |
| **Conntrack** | nf_conntrack_count / nf_conntrack_max | sysctl / node_exporter | > 80% |
| **Tunnel** | vxlan_tunnel_bfd_status | OVN BFD | Any DOWN |
| **Bandwidth** | port_tx_bytes / port_rx_bytes | OVS stats | QoS policy violations |
| **Agent** | neutron_agent_heartbeat_age | Neutron API | > 60s (dead agent) |

### OpenStack Ceilometer / Telemetry

```
Neutron metrics via Ceilometer:
  network.create / network.delete          → audit events
  subnet.create / subnet.delete
  port.create / port.delete
  router.create / router.delete
  floatingip.create / floatingip.delete
  
  bandwidth                                → bytes through a port (polling)
  ip.floating.create / ip.floating.update  → floating IP lifecycle

Modern approach (Prometheus):
  - openstack-exporter: scrapes Neutron API for resource counts and status
  - kube-ovn-pinger (or custom): end-to-end network connectivity tests
  - OVN metrics exporter: scrapes OVSDB for Raft status, flow counts
  - OVS stats exporter: per-port byte/packet counters, error counters
  - Grafana dashboards: network topology visualization, traffic heatmaps
```

---

## 11. Security

### Keystone: Auth, Tokens, Projects, Domains

```
Neutron RBAC:
  - Network sharing: RBAC policies allow sharing networks across projects
    openstack network rbac create --type network --target-project <proj-id> \
      --action access_as_shared <network-id>
  
  - Policy enforcement (oslo.policy):
    "create_network": "role:member"
    "create_network:shared": "role:admin"
    "create_network:provider:network_type": "role:admin"
    "create_port:fixed_ips:ip_address": "role:admin or role:advsvc"
    "create_floatingip": "role:member"
    "update_port:binding:host_id": "role:admin"

  - Admin-only operations:
    - Creating provider networks (direct VLAN/flat mapping)
    - Binding ports to specific hosts
    - Viewing agents and their status
    - Creating QoS policies (configurable)

Tenant Network Isolation:
  - VXLAN VNI ensures L2 isolation (different VNI = different broadcast domain)
  - OVN ACLs enforce that ports can only communicate within their logical switch
  - Port security prevents MAC/IP spoofing
  - Router provides controlled inter-network routing (only if explicitly configured)
  - No traffic crosses tenant boundaries without explicit routing + security group rules
```

### Network Security Hardening

| Area | Practice |
|------|----------|
| **Control plane** | TLS on all Neutron API endpoints; TLS for OVSDB connections; TLS for RabbitMQ |
| **Data plane** | VXLAN with IPsec (optional, via OVN IPsec); encrypts tunnel traffic between hosts |
| **Anti-spoofing** | Port security enabled by default; allowed-address-pairs for exceptions |
| **DDoS protection** | Security group rate limiting; QoS bandwidth limits; external DDoS mitigation |
| **Metadata security** | Metadata proxy via Neutron; prevents SSRF; instance identity verified by source IP |
| **DHCP security** | dnsmasq configured to only serve known MAC addresses; prevents rogue DHCP |
| **ARP security** | OVN ARP responder prevents ARP spoofing; ARP inspection via OVS flows |

---

## 12. Incremental Rollout

### Rollout Q&As

**Q1: How do you migrate from ML2/OVS to ML2/OVN without downtime?**
A: This is one of the most complex OpenStack migrations. Approach: (1) Deploy OVN infrastructure (NB/SB databases, ovn-northd) alongside existing OVS. (2) Use the `networking-ovn-migration-tool` to plan the migration. (3) Migration per compute host: stop neutron-openvswitch-agent, start ovn-controller, reprogram OVS flows. (4) Migrate one host at a time, testing connectivity between migrated and non-migrated hosts (OVN and OVS agents can coexist during migration via shared VXLAN tunnels). (5) After all hosts migrated, switch neutron-server from ML2/OVS to ML2/OVN driver. (6) Decommission L3, DHCP, metadata agents. Total duration for 10K hosts: weeks, with rolling per-host migration.

**Q2: How do you roll out a new VXLAN-based network type to replace legacy VLAN?**
A: (1) Add VXLAN type driver to ML2 configuration (alongside existing VLAN). (2) Set `tenant_network_types = vxlan,vlan` (VXLAN preferred). (3) New networks default to VXLAN. (4) Existing VLAN networks continue working — no migration needed. (5) Optionally migrate workloads from VLAN to VXLAN networks by creating new VXLAN networks and migrating VMs. (6) Never remove VLAN type driver while VLAN networks exist.

**Q3: How do you test a security group rule change before deploying it globally?**
A: (1) Use `ovn-trace` to simulate traffic with the proposed rule: `ovn-trace --detailed <datapath> 'inport=="<port>" && eth.src==<mac> && ip4.src==<src> && ip4.dst==<dst> && tcp.dst==443'`. This shows exactly which ACL rules match and what action is taken. (2) Apply the rule to a test security group first, assigned to a canary VM. (3) Verify with actual traffic: `curl` from a test VM. (4) Monitor OVN ACL hit counters for the new rule. (5) Roll out to production security groups.

**Q4: How do you add a new external network without disrupting existing floating IPs?**
A: (1) Create new external network on a new provider physical network: `openstack network create --external --provider-physical-network physnet-ext2 --provider-network-type flat external-net-2`. (2) Create subnet on new network with new IP range. (3) Existing floating IPs on the old network are unaffected. (4) New floating IPs can be created on either external network. (5) Routers can have gateways on different external networks. (6) Ensure br-ex on network nodes and compute hosts (for DVR) has connectivity to the new physical network.

**Q5: How do you handle the rollout of jumbo frames (MTU 9000) across the physical network?**
A: This must be done bottom-up: (1) Enable jumbo frames on all physical switches (leaf and spine) first. (2) Enable jumbo frames on all compute host NICs and bonds. (3) Enable jumbo frames on OVS bridges (br-int, br-tun). (4) Update Neutron network MTU: `openstack network set --mtu 8950 <network-id>` (8950 = 9000 - 50 VXLAN overhead). (5) DHCP advertises new MTU to VMs on next lease renewal. (6) Existing VMs may need DHCP renewal or manual MTU configuration. Do this rack by rack, verifying jumbo frame support with `ping -s 8922 -M do` (8922 + 28 IP/ICMP header = 8950).

---

## 13. Trade-offs & Decision Log

| # | Decision | Options Considered | Selected | Rationale |
|---|----------|--------------------|----------|-----------|
| 1 | Mechanism driver | OVS, OVN, LinuxBridge, SR-IOV, commercial SDN | OVN + SR-IOV | OVN: distributed routing, no agent sprawl, compiled flows. SR-IOV: high-performance workloads |
| 2 | Tenant network type | VLAN, VXLAN, GRE, Geneve | VXLAN | 16M networks (vs 4094 VLAN); standard encapsulation; hardware offload support |
| 3 | Routing model | Centralized, DVR, OVN native | OVN distributed | Eliminates network node bottleneck for floating IP and east-west traffic |
| 4 | DHCP HA | Single agent, 3 agents, OVN native DHCP | OVN native DHCP | No dnsmasq processes to manage; DHCP responds from OVS flows; distributed |
| 5 | Security groups | iptables, OVS conntrack, OVN ACLs, hardware offload | OVN ACLs | Compiled flows; address sets for remote SGs; hardware offloadable |
| 6 | Physical network | VLAN fabric, VXLAN fabric, BGP EVPN | BGP EVPN underlay | Scalable; no STP; ECMP; standard for modern data centers |
| 7 | Load balancing | Neutron LBaaS v2, Octavia, external LB | Octavia | Standalone LB service; amphora (haproxy VM) or OVN provider; active development |
| 8 | MTU | 1500 (standard), 9000 (jumbo) | 9000 jumbo | Avoids VXLAN fragmentation; better throughput; standard for DC networks |
| 9 | External connectivity | Floating IP only, provider network, both | Both | Floating IP for multi-tenant; provider network for high-performance/legacy |
| 10 | OVSDB replication | Standalone, Raft cluster | 3-node Raft | Survives single node failure; built into OVSDB since OVS 2.9 |

---

## 14. Agentic AI Integration

### AI-Enhanced Network Operations

| Use Case | AI Approach | Implementation |
|----------|------------|----------------|
| **Network anomaly detection** | Unsupervised learning on traffic patterns | Baseline per-port bandwidth and flow patterns. Detect DDoS, exfiltration, or misconfiguration (e.g., VM sending traffic to wrong subnet). Alert or auto-quarantine. |
| **Intelligent security group suggestions** | ML model analyzes actual traffic vs allowed rules | Identify overly permissive rules (port 0-65535 open), unused rules (no flow hits), and suggest tighter rules. Similar to AWS Security Hub. |
| **Network capacity forecasting** | Time-series prediction on subnet utilization | Forecast IP exhaustion per subnet. Predict bandwidth bottlenecks. Proactive alerts for capacity planning. |
| **Automated troubleshooting** | LLM-powered diagnosis | "VM-A can't reach VM-B" → agent runs ovn-trace, checks SG rules, verifies port bindings, checks OVS flows, and returns root cause with suggested fix. |
| **Traffic engineering** | RL-based QoS optimization | Agent observes congestion patterns, adjusts QoS policies dynamically, optimizes bandwidth allocation across tenants. |

---

## 15. Complete Interviewer Q&A Bank

**Q1: Explain the full packet path from VM-A to VM-B on different subnets, different hosts.**
A: VM-A (10.0.1.5) sends packet to its gateway (10.0.1.1, which is the router's MAC). (1) OVS br-int on Host-A matches the router MAC, performs L3 routing (decrements TTL, changes src MAC to router MAC for subnet-2, looks up dst 10.0.2.10 in routing table). (2) ARP lookup for 10.0.2.10 resolves to VM-B's MAC (OVN pre-programs this). (3) Sets dst MAC to VM-B's MAC. (4) br-int matches VM-B's port → not local → forward to br-tun. (5) br-tun maps network to VXLAN VNI, encapsulates in UDP (src=Host-A VTEP, dst=Host-B VTEP). (6) Physical network delivers to Host-B. (7) Host-B br-tun decapsulates, maps VNI to local VLAN. (8) br-int delivers to VM-B's tap port. With OVN, this entire path is compiled into OVS flows — no namespace hops, no user-space processing.

**Q2: How does Neutron prevent IP address conflicts?**
A: Neutron's IPAM (IP Address Management) maintains the `ip_allocations` table with a unique constraint on (subnet_id, ip_address). When a port is created, Neutron allocates an IP within a DB transaction, checking the constraint. Concurrent allocations are serialized by MySQL row-level locks. DHCP (dnsmasq/OVN) only offers IPs that Neutron has allocated to specific ports. Port security prevents VMs from using IPs not assigned to their ports. Additional protection: ARP inspection via OVS flows ensures VMs can only claim their assigned IPs.

**Q3: What is OVN's "incremental processing" and why is it important?**
A: Before incremental processing, when any change occurred in the Southbound DB, ovn-controller would recompute ALL OVS flows from scratch — expensive at scale. Incremental processing (introduced in OVN 20.06) tracks dependencies between logical flows and OVS flows. When a single port is created, ovn-controller only computes the new flows for that port, not the entire flow table. This reduced flow recomputation time from seconds to milliseconds for large deployments. It's critical for scale: without it, adding a port to a host with 50,000 existing flows would cause a multi-second recomputation pause.

**Q4: How do you handle MTU issues with VXLAN?**
A: VXLAN adds 50 bytes of overhead. If the physical MTU is 1500, effective tenant MTU is 1450. Solutions: (1) Set physical network to jumbo frames (MTU 9000), giving tenant networks MTU 8950. (2) Neutron advertises the correct MTU via DHCP option 26 and RA (IPv6). (3) Path MTU Discovery (PMTUD) should work but is often broken by firewalls. (4) Configure `global_physnet_mtu` and `path_mtu` in ml2_conf.ini. (5) OVN calculates and sets the tenant MTU automatically based on the overlay overhead and physical MTU. Never rely on fragmentation — it destroys performance.

**Q5: How do you implement network QoS in OpenStack?**
A: Neutron QoS extension provides: (1) Bandwidth limit rules: cap ingress/egress bandwidth per port using OVS QoS (Linux tc or OVS meter). (2) DSCP marking: set DSCP bits in IP header for physical network QoS. (3) Minimum bandwidth: guarantee minimum bandwidth (requires Placement integration for scheduling). QoS policies are applied per-port or per-network. OVN implementation: QoS rules compiled into OVS meter actions. With hardware offload, QoS is enforced in the NIC. Minimum bandwidth uses Placement: a port with `min_bandwidth=1000` creates a resource request for `NET_BW_EGR_KILOBIT_PER_SEC:1000`, and the scheduler ensures the host's NIC has capacity.

**Q6: What is a trunk port, and when would you use it?**
A: A trunk port is a single vNIC that carries multiple VLANs (via 802.1Q tagging). The parent port carries untagged traffic, and sub-ports carry tagged traffic for different networks. Use case: NFV — a virtual firewall or router needs interfaces on multiple networks but has a limited number of PCI slots. Instead of one vNIC per network (limited by PCI bus), use one trunk port with VLAN sub-interfaces. The VM configures VLAN sub-interfaces (e.g., eth0.100, eth0.200) for each network. OVS handles VLAN tagging/untagging on the trunk port.

**Q7: How does Neutron handle DNS for VMs?**
A: Three approaches: (1) DHCP-provided DNS: dnsmasq (or OVN DHCP) provides DNS nameservers from the subnet configuration. VMs use external DNS (e.g., 8.8.8.8). (2) Internal DNS: dnsmasq also provides DNS resolution for instance names within the network (e.g., vm-A.openstacklocal). (3) Designate integration: Neutron can automatically create DNS records in Designate (OpenStack DNS service) when ports are created, enabling DNS resolution across networks and from external.

**Q8: How do you troubleshoot a VM that can't get a DHCP lease?**
A: (1) Check port status: `openstack port show <port-id>` — status should be ACTIVE. (2) Check DHCP agent is alive: `openstack network agent list --network <net-id>`. (3) Check dnsmasq is running (legacy): `ip netns exec qdhcp-<net-id> ps aux | grep dnsmasq`. (4) With OVN: check that the DHCP options are configured in OVN NB DB: `ovn-nbctl list DHCP_Options`. (5) Packet capture: `tcpdump -i tap-<port-id> -n port 67 or port 68` to see DHCP discover/offer. (6) Check security groups allow DHCP (UDP 67/68) — this is usually in the default SG. (7) Check OVS flow for DHCP: `ovs-ofctl dump-flows br-int | grep dhcp`.

**Q9: Explain the difference between tenant networks, provider networks, and external networks.**
A: **Tenant networks**: created by users; isolated via VXLAN/VLAN segmentation; exist only in the virtual overlay. **Provider networks**: created by admins; mapped to a specific physical network and VLAN; VMs get IPs directly from the physical network. **External networks**: a special type of provider network used for floating IP allocation and router gateways; typically flat or VLAN-mapped to the internet-facing physical network. Users can create ports on tenant/external networks (if allowed); only admins create provider networks.

**Q10: How does OVN handle metadata service for VMs?**
A: OVN implements a distributed metadata proxy. When a VM queries the metadata service (169.254.169.254), OVS flows on the compute host intercept the packet and redirect it to a local metadata agent (or OVN's built-in metadata proxy in newer versions). The proxy adds the instance's UUID (determined by the port's MAC/IP) as a header and forwards to nova-metadata-api. This eliminates the need for a separate neutron-metadata-agent process and router/DHCP namespace proxy chains used in the legacy ML2/OVS setup.

**Q11: How do you implement network isolation between projects that share the same physical infrastructure?**
A: Multiple layers: (1) VXLAN segmentation: each project's network gets a unique VNI — packets from different VNIs never mix at L2. (2) L3 routing: no routing between project networks unless explicitly connected via shared routers (not default). (3) Security groups: default deny on all ports; only allow traffic matching rules. (4) Port security: prevents MAC/IP spoofing. (5) RBAC: projects can only see/modify their own networks. (6) Quotas: prevent resource exhaustion by one project. (7) Namespace isolation on network nodes: each router runs in its own Linux network namespace.

**Q12: What happens to network connectivity during a rolling upgrade of Neutron?**
A: Running VMs are unaffected — OVS flows persist even when neutron-server or agents restart. During rolling upgrade: (1) Upgrade neutron-server first (stateless; handles API version negotiation). (2) Upgrade OVN components (NB DB, ovn-northd, SB DB) — Raft handles rolling restart. (3) Upgrade ovn-controller per compute host — existing flows persist; controller reconnects and reconciles. (4) DB migrations: run expand migrations before upgrade, contract after. Key: never take down all instances of any service simultaneously. The data plane (OVS flows) is independent of the control plane.

**Q13: How does Neutron integrate with Octavia for load balancing?**
A: Octavia is a standalone service that provides Load Balancer as a Service. Integration: (1) User creates a load balancer on a Neutron network. (2) Octavia spins up an "amphora" VM (haproxy instance) on that network. (3) A VIP port is created in Neutron and assigned to the amphora. (4) Pool members (backend VMs) are registered with their Neutron port IPs. (5) Octavia configures haproxy in the amphora to balance traffic across members. (6) For OVN provider: no amphora needed — load balancing is implemented natively in OVS flows (L4 only). The OVN provider is lighter weight but less feature-rich than amphora.

**Q14: How do you handle IPv6 in Neutron?**
A: Neutron supports three IPv6 modes: (1) SLAAC: router sends Router Advertisements (RA) with prefix; VMs auto-configure addresses. (2) DHCPv6-stateful: DHCP server assigns IPv6 addresses (like DHCPv4). (3) DHCPv6-stateless: RA provides prefix for auto-configuration; DHCP provides DNS and other options. Configuration: create a subnet with `--ip-version 6 --ipv6-ra-mode slaac --ipv6-address-mode slaac`. OVN handles RA natively. Dual-stack: a network can have both IPv4 and IPv6 subnets; VMs get addresses on both.

**Q15: What is the role of ML2 extension drivers, and name some important ones?**
A: Extension drivers add capabilities to the ML2 plugin: (1) **Port Security**: enables/disables anti-spoofing per port. (2) **QoS**: attaches QoS policies to ports/networks. (3) **DNS Integration**: manages DNS names for ports. (4) **Trunk**: enables trunk ports with VLAN sub-interfaces. (5) **Uplink Status Propagation**: propagates physical link status to ports. (6) **Port Forwarding**: NAT port forwarding (map external_ip:port → internal_ip:port). Extension drivers hook into port/network create/update/delete events and modify the resource before it's passed to the mechanism driver.

---

## 16. References

| # | Resource | URL |
|---|----------|-----|
| 1 | Neutron Developer Documentation | https://docs.openstack.org/neutron/latest/ |
| 2 | Neutron ML2 Plugin | https://docs.openstack.org/neutron/latest/admin/config-ml2.html |
| 3 | OVN Architecture | https://docs.ovn.org/en/latest/ref/ovn-architecture.7.html |
| 4 | OVN OpenStack Integration | https://docs.openstack.org/neutron/latest/admin/ovn/index.html |
| 5 | Neutron DVR | https://docs.openstack.org/neutron/latest/admin/deploy-ovs-ha-dvr.html |
| 6 | Neutron Security Groups | https://docs.openstack.org/neutron/latest/admin/archives/adv-features.html |
| 7 | Octavia (LBaaS) | https://docs.openstack.org/octavia/latest/ |
| 8 | Open vSwitch Documentation | https://docs.openvswitch.org/ |
| 9 | VXLAN RFC 7348 | https://datatracker.ietf.org/doc/html/rfc7348 |
| 10 | OVN Distributed Virtual Routing | https://docs.ovn.org/en/latest/tutorials/ovn-openstack.html |
| 11 | Neutron QoS | https://docs.openstack.org/neutron/latest/admin/config-qos.html |
| 12 | SR-IOV with Neutron | https://docs.openstack.org/neutron/latest/admin/config-sriov.html |
