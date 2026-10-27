<p align="center">
  <img src="assets/banner.svg" alt="RDR2 Story Mode Co-op" width="100%">
</p>

<h1 align="center">RDR2 Story Mode Co-op</h1>

<p align="center">
  <a href="https://github.com/cptn73m0/rdr2-coop/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/cptn73m0/rdr2-coop/actions/workflows/ci.yml/badge.svg?branch=main"></a>
  <img alt="Build" src="https://img.shields.io/github/v/tag/cptn73m0/rdr2-coop?style=flat-square&label=build&color=ff6a3d">
  <img alt="Protocol" src="https://img.shields.io/badge/protocol-v19%20wire-ff6a3d?style=flat-square">
  <img alt="Integration" src="https://img.shields.io/badge/integration-42%2F42-2ea44f?style=flat-square">
  <img alt="C++ tests" src="https://img.shields.io/badge/C%2B%2B%20tests-7%2F7-2ea44f?style=flat-square">
  <img alt="Quality gate" src="https://img.shields.io/badge/quality-strict%20pass-2ea44f?style=flat-square">
  <img alt="Platform" src="https://img.shields.io/badge/platform-Windows%20x64-4c8eda?style=flat-square">
  <img alt="License" src="https://img.shields.io/badge/license-source--available-8a8a8a?style=flat-square">
</p>

<p align="center">
  <b>English</b> · <a href="README.ru.md">Русский</a>
</p>

<p align="center">
  <b><a href="#installation">Installation</a></b>
  ·
  <b><a href="#build-from-source">Build from source</a></b>
  ·
  <b><a href="#configuration">Configuration</a></b>
  ·
  <b><a href="#how-it-works">How it works</a></b>
  ·
  <b><a href="#engineering--quality">Engineering</a></b>
</p>

A **two-player co-op replication layer for Red Dead Redemption 2 — Story Mode**.
The host PC keeps playing the world it always played; a second player joins over the LAN
and sees NPCs, animals, horses, wagons, trains, traffic, damage and player presence
replicated into their own game session — smoothly, in real time, without modding the
game's engine.

> Story Mode has no native multiplayer. This project is an **emulation by replication**:
> the host owns the world, the relay server validates and forwards state, and the guest
> renders authoritative proxies. There is no client-side world simulation on the guest.

## At a glance

| | |
| --- | --- |
| Current build | **`npc-e19`** (ASI `DD0EE210`, server `DD9BD9D8`) |
| Wire protocol | **v19**, both parts must match |
| Game build | Red Dead Redemption 2 **1.0.1491.50** + ScriptHookRDR2 |
| Players | **host + guest** (2 players per session) |
| Integration tests | **42 / 42** (Python protocol/server suite) |
| C++ tests | **7 / 7** (frame reader, config, parsing, mission barrier, budgets, fuzz) |
| Quality gate | **strict pass** — 45 files · 505 functions · CRAP>5000 = 3 · TODO = 0 |
| Code size | **~29.5k lines** (~24.6k code) of C++17 + PowerShell/Python tooling |
| Network | LAN, TCP `7777`, typical **4–12 KB/s**, worst **≤ 15 KB/s** |
| Status | `npc-e19` deployed on both PCs; full 2-PC acceptance run in progress |

## What it does

<table>
<tr><td width="50%" valign="top">

**World & actors**
- NPCs, ambient peds and mission actors
- Animals and horses, including draft horses
- Human appearance (components, drawables, tint)
- Animal / horse appearance and tack
- Scenarios, combat, aim, reload, weapon state
- Damage from the guest, host-authoritative
- Headshots are lethal (host resolves impact)
- Corpse persistence (up to 5 minutes, like the original)

</td><td width="50%" valign="top">

**Transport & infrastructure**
- Vehicles, wagons and their occupants
- Drivers, passengers, draft horses and harness
- Mission trains with exact per-carriage passengers
- Mission barrier sync, rewards and save backup
- Player presence (map, pause, satchel, shop)
- Map labels and floating name tags
- Host migration (leave, crash, game thread hang)
- Rollback-ready snapshots for every build

</td></tr>
</table>

## Architecture

<p align="center">
  <img src="assets/architecture.svg" alt="Architecture: host game, relay server, guest game" width="96%">
</p>

Three components, one authority model:

| Component | Runs on | Responsibility |
| --- | --- | --- |
| **ASI client** (`rdr2_coop.asi`) | both PCs | hooks the game, discovers actors, captures state, applies proxies, renders UI |
| **Relay server** (`rdr2_coop_server.exe`) | host PC (or any LAN box) | validates messages at the boundary, enforces roles and rate limits, relays host-only state |
| **Snapshot format** | — | `[len:u32][type:u8][payload]`, versioned wire protocol |

The host is the only writer of world state. The guest sends intentions (player frame,
damage, presence) and renders the host's authoritative snapshots. The server never
simulates and never touches the game process.

## Installation

### Requirements

- Red Dead Redemption 2 **1.0.1491.50** (Story Mode) on **both** PCs
- [ScriptHookRDR2](https://www.dev-c.com/rdr2/scripthookrdr2/) + an ASI loader (`dinput8.dll`)
- Windows 10/11 x64, two PCs on the same LAN
- Server: no runtime dependencies (static CRT)

### 1. Build (or take a prepared pair)

Build from source (see [Build from source](#build-from-source)) or use a prepared
`dist/npc-*` snapshot if you have one. A snapshot contains exactly:

```
rdr2_coop.asi          # client plugin
rdr2_coop_server.exe   # relay server
rdr2-coop.ini.local    # host INI (28 keys)
rdr2-coop.ini.arina    # guest INI template
README.md              # hashes, protocol, rollback instructions
```

### 2. Deploy the client

Copy `rdr2_coop.asi` into the game's `scripts` folder on **both** PCs:

```
<game>\scripts\rdr2_coop.asi
```

### 3. Deploy the server

The server runs on the **host PC** in a **visible console window**:

```powershell
rdr2_coop_server.exe 7777
```

Keep it open — the live log is the primary diagnostic tool. The server prints player
state, per-type traffic counters and host liveness every 5 seconds.

### 4. Configure both PCs

The client reads `scripts\rdr2-coop.ini`:

```ini
server_host = 192.168.1.103   ; IP of the PC running the server
server_port = 7777
player_name = Denuwa
```

Host typically uses `127.0.0.1`, guest uses the host's LAN IP. The 28 configuration
keys are documented in [Configuration](#configuration).

### 5. Play

1. Start the server on the host PC.
2. Start RDR2 on the **host** PC (host is assigned by the server).
3. Start RDR2 on the **guest** PC.
4. Play Story Mode together — the guest sees the host's world.

## Build from source

### Client ASI

```powershell
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Release
# -> build\Release\rdr2_coop.asi
```

### Relay server

```powershell
cmake -S server -B build-server -G "Visual Studio 16 2019" -A x64
cmake --build build-server --config Release
# -> build-server\Release\rdr2_coop_server.exe
```

### Tests and the full proof gate

```powershell
cmake -S tests -B tests\build -G "Visual Studio 16 2019" -A x64
cmake --build tests\build --config Release
powershell -ExecutionPolicy Bypass -File scripts\ci.ps1
```

`scripts/ci.ps1` builds everything, runs the Python integration suite and the C++ tests,
runs the quality scan and enforces the **strict quality gate** (a regression in CRAP,
duplication or TODO count fails the run). Add `-NoClient`, `-NoServer` or `-NoTests`
when you only need part of the pipeline.

### Release snapshots

Every accepted iteration is frozen into a self-contained folder with hashes and a
rollback note:

```powershell
powershell -ExecutionPolicy Bypass -File tools\snapshot_build.ps1 `
  -Label npc-e19 -Protocol 19 -Commit ef49ac8 `
  -Features "..." -Note "..." -Tag npc-e19
```

Snapshots live in `dist/<label>/` (not committed to git — they are release artifacts).
Git tags `npc-e0 … npc-e19` preserve the exact corresponding source trees.

## Configuration

`rdr2-coop.ini` is created on first launch. Unknown keys are ignored; invalid numbers
fall back to safe defaults; `;` / `#` start a comment.

| Group | Key | Default | Meaning |
| --- | --- | --- | --- |
| Network | `server_host`, `server_port` | `127.0.0.1`, `7777` | relay server address |
| Identity | `player_name`, `ped_model` | — | player display name and Story Mode model |
| World | `world_sync`, `world_vehicles`, `world_trains` | `0`, `1`, `0` | full-world flag, vehicles, mission trains |
| Missions | `mission_sync`, `mission_credit`, `save_backup` | `0`, `0`, `1` | mission barrier, reward credit, save backup |
| NPC core | `npc_actor`, `npc_cap`, `npc_radius` | `1`, `12`, `60` | replication on, actor cap, discovery radius |
| NPC detail | `npc_appearance`, `npc_animal_cap`, `npc_damage`, `npc_corpse_max_sec` | `1`, `4`, `1`, `300` | appearance, animal cap, damage, corpse hold |
| Motion | `npc_motion_update`, `npc_motion_marker`, `npc_motion_status_p2`, `npc_frame_hz`, `npc_frame_adaptive` | `1`, `0`, `1`, `15`, `1` | motion updates, debug markers, frame rate |
| Visitor mode | `npc_visitor`, `npc_visitor_vehicles`, `npc_visitor_preserve_events`, `npc_visitor_pause_scenes` | `1`, `1`, `1`, `1` | guest world mode and preserved local NPCs |
| Transport | `npc_occupants`, `npc_vehicle_radius` | `1`, `80` | occupants/draft horses, vehicle scan radius |
| Player | `player_frame_hz`, `player_frame_change_only` | `20`, `1` | player frame rate / change-only sending |
| Appearance | `npc_animal_appearance`, `npc_mount_appearance` | `0`, `0` | animal/mounted-horse appearance assets |
| Wheels (A/B) | `npc_vehicle_wheels` | `0` | experimental wheel spin on vehicle proxies |
| Presence | `npc_presence` | `1` | UI status suffix in name tags |
| Debug | `npc_debug`, `train_debug` | `1`, `0` | verbose actor/train logs |

A startup validator prints `config: warn ...` for dangerous combinations
(for example `world_sync=1` together with `npc_actor=0`).

## How it works

- **Discovery** — the host scans actors, animals, horses and vehicles around the player
  (radius-based, budgeted per tick) and assigns stable network IDs.
- **Replication** — the host sends spawn batches, 60 Hz actor frames, appearance assets,
  outfits, vehicle snapshots, occupants and train state. All intervals are wall-clock
  based, so behavior does not change with FPS.
- **Interpolation** — the guest smooths actor and vehicle motion between snapshots
  (lead-based far-target, teleport only on large desync).
- **Visitor mode** — the guest suppresses its own ambient crowd to avoid duplicates, but
  keeps local vendors, quest NPCs and random-event NPCs as its own world.
- **Authority** — the host resolves damage, death, headshots, outfits, vehicle occupants
  and host migration. The server only validates and relays.
- **Recovery** — if the host game hangs or leaves, the server demotes it and promotes the
  guest with a grace period; if the game thread goes silent during loading, a presence
  heartbeat prevents false timeouts.

Detailed specifications: [docs/PROTOCOL.md](docs/PROTOCOL.md),
[docs/PERF.md](docs/PERF.md), [docs/TRAINS.md](docs/TRAINS.md),
[docs/NPC_E1_TESTING.md](docs/NPC_E1_TESTING.md).

## Engineering & quality

<p align="center">
  <img src="assets/pipeline.svg" alt="Engineering pipeline" width="96%">
</p>

The project is developed and reviewed by a **multi-role engineering team** using the
[ub-mode](https://github.com/unclebob/ub-mode) discipline: *first make it work, then make
it right, then follow practice*. Every change is a small verifiable unit with a written
action log.

| Gate | What it proves |
| --- | --- |
| Build | client ASI + relay server + tests compile clean (`0 warning / 0 error`) |
| Integration | **42/42** protocol/server tests, including join race, host migration, rate limits, fuzz |
| C++ tests | **7/7** deterministic unit suites |
| Quality | strict CRAP/duplication/TODO gate against a committed baseline |
| Independent review | a second agent rebuilds from a clean archive and diffs behavior |
| Rollback | every build has a tagged source tree and a snapshot folder with hashes |

**Quality metrics (current baseline):** 45 files · 505 functions · CRAP>5000 = **3** ·
duplication ≥4 = **3** · TODO = **0**. The scanner and baseline are part of the repo:
[tools/quality_scan.py](tools/quality_scan.py), [docs/UB_MODE.md](docs/UB_MODE.md).

### Team & workflow

| Role | Responsibility |
| --- | --- |
| **Lead / integrator** | architecture, task board, builds, CI, commits, tags, deployment, release snapshots |
| **Network & protocol review** | framing, wire compatibility, server relay, rate limits, host migration |
| **Client gameplay review** | C++ gameplay paths, native calls, interpolation, visitor mode |
| **QA & release verification** | integration/C++ tests, quality gate, acceptance checklists, docs |
| **World-sync architect** | replication design, protocol proposals, independent proof of each slice |
| **Native R&D** | ScriptHookRDR2 native research, signatures, safe fallbacks |

Workflow: **plan → implement in disjoint scopes → per-unit proof gate → independent
proof → small ordered commits → tag + snapshot → deploy to both PCs**. The task board and
write scopes are recorded per task; no agent overwrites another's files.

## Performance

Measured on a 2-PC LAN session (`npc-e19`, host p95 over 300 frames):

| Metric | Value | Budget |
| --- | --- | --- |
| Full frame | p95 **0.39 ms**, max **1.12 ms** | p95 ≤ 4 ms, max ≤ 10 ms |
| World sync | p95 **0.10 ms** | — |
| Actor sync | p95 **0.20 ms** | p95 ≤ 2 ms |
| Coop logic | p95 **0.16 ms** | — |
| Client traffic | **~7.1 KB/s tx**, **~0.5 KB/s rx** | typical ≤ 10–12 KB/s |
| Server traffic | typical **4–12 KB/s**, peaks ≤ 15 KB/s | worst ≤ 15 KB/s |

Every log line is machine-parsable (`perf: frame`, `perf: net`, server `net: ...`) and
checked by [tools/perf_report.py](tools/perf_report.py).

## Testing & acceptance

- [docs/ACCEPTANCE_2PC.md](docs/ACCEPTANCE_2PC.md) — two-PC acceptance checklist
  (blocks A–H for deployed npc-e19 + interactivity block I for v20 P1, which is
  **ready for deploy but not deployed yet**; P1 is **default-off**), exact hashes
  and rollback plan.
- [docs/NPC_E1_TESTING.md](docs/NPC_E1_TESTING.md) — scenario catalogue **S1–S32**
  with expected log lines.
- [docs/TESTING.md](docs/TESTING.md) — headless test strategy and tools.
- [tests/README.md](tests/README.md) — how to run integration and C++ suites.
- [tools/README.md](tools/README.md) — snapshot, deploy, traffic and fake-client tools.

## Roadmap

| Phase | Status | Scope |
| --- | --- | --- |
| E1–E5 | done | actors, motion, appearance, damage, corpses |
| E6–E8 | done | presence, motion polish, host migration / traffic counters |
| E9–E11 | done | animal/horse appearance, mount outfit, exact train passengers |
| E12–E15 | done | performance frame, player frame throttle, traffic accounting, ub-mode refactors |
| E16–E19 | done | quality tooling, soft host migration, loading heartbeat, vendor dedupe, wagon occupants, map labels, wheel A/B |
| **v20 P1** | **ready for deploy** (default-off) | **guest influence**: damage, doors, seats, hijack, draft hitch — host-authoritative intents (`0x23/0x24/0x26`); U1–U5 done on `feature/v20-p1` (integration 50/50, C++ 8/8); `vehicle_interaction=0` by default — enable with the key on both PCs after F1–F4 re-proof; deploy pair `npc-e20` pending |

Full details: [ROADMAP.md](ROADMAP.md), [docs/INTERACTIVITY.md](docs/INTERACTIVITY.md),
[docs/INTERACTIVITY_P1.md](docs/INTERACTIVITY_P1.md),
[docs/NATIVE_RESEARCH_INTERACT.md](docs/NATIVE_RESEARCH_INTERACT.md).

## Limitations

- This is **replication, not a native multiplayer engine**. Story Mode scripts and
  missions remain host-driven; the guest is a visitor in the host's world.
- Some host-only entities are intentionally not replicated (mission-critical scripts,
  local vendors preserved on the guest by design).
- The guest's vehicle proxies are visual until v20 P1 ships host-authoritative
  interactions; wheel spin is an opt-in A/B flag.
- Trains are mission-train replication, not fully dynamic railway simulation.
- Both clients must run the **same protocol version**; mixing v19/v20 will be rejected.
- No license file is present yet: treat the repository as source-available and contact
  the author before redistribution.

## Repository layout

| Path | Contents |
| --- | --- |
| `src/` | ASI client: `coop.cpp`, `actor_sync.cpp`, `world_sync.cpp`, natives, config |
| `net/` | protocol, framing, socket layer, client parser/dispatch |
| `server/` | relay server (validation, relay, rate limits, migration) |
| `tests/` | Python integration suite and C++ unit/fuzz tests |
| `tools/` | snapshot builder, quality scanner, traffic/perf reports, fake clients |
| `docs/` | protocol, performance, acceptance, interactivity and R&D documents |
| `scripts/ci.ps1` | full proof gate (build + tests + quality) |
| `assets/` | README banner, architecture and pipeline diagrams |

## Credits

- [ScriptHookRDR2](https://www.dev-c.com/rdr2/scripthookrdr2/) and the RDR2 modding
  community for native research and tooling.
- [unclebob/ub-mode](https://github.com/unclebob/ub-mode) for the engineering discipline
  used by the team.
- Everyone who reported symptoms from real 2-PC sessions — every bug fix in the history
  started with a log line from a live playtest.

---

<p align="center">
  <sub>Built for Story Mode co-op experiments. Not affiliated with Rockstar Games or Take-Two Interactive.</sub>
</p>
