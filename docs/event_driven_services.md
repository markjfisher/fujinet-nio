# Event-Driven Service Lifecycle Architecture

This document describes the **event-driven service lifecycle model** used in
*fujinet-nio*, and explains how platform-specific signals (such as Wi-Fi
connectivity) are translated into **platform-agnostic events** that drive the
startup of higher-level services (SNTP, web servers, etc.).

This design ensures:
- clean separation of concerns
- no platform `#ifdef`s in core logic
- deterministic service startup
- scalability as new services are added

---

## 1. Motivation

In earlier FujiNet firmware implementations, platform events (such as ESP32
Wi-Fi IP acquisition) directly triggered unrelated side effects:

- starting SNTP
- starting HTTP servers
- toggling LEDs
- updating global state

This tightly coupled approach:
- made ordering implicit and fragile
- mixed transport, platform, and service concerns
- did not generalize well to POSIX or future platforms

*fujinet-nio* replaces this with a **small, explicit event system** that
decouples *event detection* from *service behavior*.

---

## 2. Core Principles

1. **Platforms detect conditions; services react**
   - Platform code reports *what happened*
   - Services decide *what to do*

2. **Events are semantic, not procedural**
   - “Network got an IP address”
   - not “start SNTP now”

3. **Core owns the event surface**
   - All services subscribe via `FujinetCore`
   - No platform-specific knowledge leaks into services

4. **No hidden startup order**
   - Service ordering emerges naturally from event subscriptions

---

## 3. SystemEvents Hub

The central mechanism is the **SystemEvents hub**, owned by `FujinetCore`.

```
FujinetCore
 └── SystemEvents
      ├── NetworkEvent stream
      └── TimeEvent stream
```

Each stream is a synchronous publish/subscribe channel:

- Subscribers register callbacks
- Events are published by platform or core code
- Subscribers are notified in publish order

The hub is intentionally minimal:
- no threads
- no queues by default
- no dependencies on platform APIs

---

## 4. Network Events

Network connectivity is modeled as a **semantic state machine**, not as
platform callbacks.

### NetworkEvent kinds

- `LinkUp`
- `GotIp`
- `LinkDown`

The most important event is:

```
NetworkEventKind::GotIp
```

This indicates the device has:
- a usable network interface
- a configured IP address
- connectivity suitable for higher-level services

---

## 5. Platform Network Links and Monitoring

Platform-specific code implements `INetworkLink`:

- ESP32: Wi-Fi state machine
- POSIX: assumed network availability

To avoid pushing events from low-level handlers, a **NetworkLinkMonitor**
observes the link and publishes events when transitions occur.

```
Esp32WifiLink / PosixNetworkLink
        │
        ▼
NetworkLinkMonitor
        │
        ▼
SystemEvents.network()
```

This keeps platform drivers focused solely on:
- connection state
- IP address reporting
- retries and failures

---

## 6. Service Startup via Events

Higher-level services subscribe to events rather than being started explicitly.

### Example: SNTP Service (ESP32)

- Subscribes to `NetworkEvent::GotIp`
- Starts SNTP exactly once per connection
- Publishes `TimeEvent::Synchronized` on completion

```
NetworkEvent::GotIp
        │
        ▼
   SntpService
        │
        ▼
System clock updated
```

No Wi-Fi code knows SNTP exists.
No core code needs to order services.

---

## 7. Time Events

Time synchronization is modeled explicitly:

- `TimeEventKind::Synchronized`
- `TimeEventKind::ManuallySet`

This allows:
- ClockDevice to remain a simple I/O device
- future services to react to time availability
- tests to simulate time changes deterministically

---

## 8. POSIX Symmetry

On POSIX platforms:
- networking is assumed to be available at startup
- a synthetic `NetworkEvent::GotIp` is published once

This ensures:
- services behave identically on ESP32 and POSIX
- no platform-specific branching in service logic
- consistent test behavior

---

## 9. Future Services

This architecture is intentionally extensible.

Future services can subscribe to the same events:

- Embedded HTTP server
- mDNS / service discovery
- OTA update agent
- Telemetry exporters

Each service:
- subscribes to relevant events
- manages its own lifecycle
- remains isolated from platform details

---

## 10. Why This Is Not Over-Engineering

This design adds:
- one small event hub
- one monitoring class

In return it removes:
- implicit ordering
- platform side effects
- hard-coded startup sequences

It enables the project to grow without architectural debt.

---

## 11. Summary

The event-driven service lifecycle in *fujinet-nio*:

- separates detection from reaction
- keeps core logic platform-agnostic
- supports ESP32 and POSIX equally
- provides a clean foundation for future services

This model should be used for **all cross-cutting service startup concerns**
going forward.
