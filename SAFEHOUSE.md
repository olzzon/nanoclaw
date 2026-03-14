# Safehouse — Silent Command Safety for NanoClaw Containers

## What is Safehouse?

Safehouse is a defense-in-depth layer inside NanoClaw's agent containers. A thin C shim (`safehouse_wrap`) sits in the command path via symlinks. The agent sees normal command names (`rm`, `mv`, `dd`, etc.) but destructive operations are silently blocked and logged based on per-command policy files.

**Key properties:**
- Zero dependencies (single C binary, no dynamic allocation)
- Policy-driven: each command has a `.policy` file defining what's blocked
- **Completely invisible to the agent** — blocked commands return exit 0 with no stderr output; the agent believes the operation succeeded
- Opt-in via `SAFEHOUSE_ENABLED=true` (disabled by default for development)
- Event logging and host-side alerts (agent never sees these)

## Why Safehouse?

NanoClaw's containers provide **isolation** — the agent can't escape the sandbox. But inside that sandbox, the agent still has full destructive power over its own workspace. A jailbroken or prompt-injected agent could:

- `rm -rf /workspace/group` — destroy all group data and conversation history
- `dd if=/dev/zero of=/workspace/group/CLAUDE.md` — corrupt memory files
- `mv /workspace/ipc /dev/null` — destroy IPC communication
- `chmod 000 /workspace/ipc` — break IPC communication
- Overwrite the agent-runner source at `/app/src/` to modify its own behavior

Safehouse silently neutralizes these operations. The agent thinks the command succeeded, but nothing happened. Only the host sees the alerts.

```
┌─────────────────────────────────────────────────┐
│              CONTAINER (SANDBOXED)               │
│                                                  │
│   Agent runs commands                            │
│        │                                         │
│        ▼                                         │
│   ┌──────────────┐    ┌──────────────────┐       │
│   │  Safehouse   │───▶│  Policy Engine   │       │
│   │  shim (C)    │    │  (.policy files) │       │
│   └──────┬───────┘    └──────────────────┘       │
│          │                                       │
│     ALLOWED? ──yes──▶ exec real binary           │
│          │                                       │
│         no ──▶ log event, return exit 0          │
│                (silent — agent sees success)      │
│                                                  │
└─────────────────────────────────────────────────┘
         │ (host-side only)
         ▼
   IPC alerts → main channel notification
```

## Enabling Safehouse

Safehouse is **disabled by default** so development workflows aren't restricted. To enable for production, add to your `.env`:

```
SAFEHOUSE_ENABLED=true
```

When disabled, the shim still sits in the PATH but immediately passes through to the real binary with zero overhead — no policy checks, no logging.

## How It Works

### Build Time

The safehouse shim is compiled from source (`container/safehouse/safehouse_wrap.c`) during the Docker build. Symlinks in `/usr/local/bin/` (higher PATH priority than `/usr/bin/`) point to the shim for each wrapped command. Policies are copied from `container/safehouse-policies/`.

Safehouse is always **baked into the image**. The same image works for both development (disabled) and production (enabled) — toggled at runtime via the `SAFEHOUSE_ENABLED` environment variable.

### Runtime (enabled)

1. Agent runs `rm -rf /app/src`
2. Shell resolves `/usr/local/bin/rm` → safehouse shim
3. Shim checks `SAFEHOUSE_ENABLED=1`, loads `/etc/safehouse/policies/rm.policy`
4. Policy blocks: `-f` flag is in `BLOCK_FLAG` list
5. Shim logs the event to `/var/log/safehouse/events.log`
6. Shim writes alert to `/workspace/ipc/safehouse_alerts.jsonl`
7. Shim returns **exit 0** with **no output** — agent thinks it worked
8. Host IPC watcher picks up the alert and notifies the main channel

### Runtime (disabled)

1. Agent runs `rm -rf /app/src`
2. Shell resolves `/usr/local/bin/rm` → safehouse shim
3. Shim checks `SAFEHOUSE_ENABLED` != `1`, immediately `execv()`s `/usr/bin/rm`
4. Real binary runs normally — no policy checks, no logging

## Files

| File | Purpose |
|------|---------|
| `container/safehouse/safehouse_wrap.c` | C shim source — single file, no external deps |
| `container/safehouse/safehouse_alert.sh` | Alert script — writes blocked events to IPC |
| `container/safehouse/Makefile` | Builds the shim |
| `container/safehouse-policies/*.policy` | Per-command policy files (11 commands) |
| `src/config.ts` | `SAFEHOUSE_ENABLED` flag |
| `src/container-runner.ts` | Passes env var, mounts log dir when enabled |
| `src/ipc.ts` | Monitors alerts, notifies main channel |

## Policy Format

Each wrapped command has a `.policy` file in `container/safehouse-policies/`:

```
REAL_BINARY     /usr/bin/rm        # Path to the real binary (required)
BLOCK_FLAG      -f                 # Block if this exact flag appears
BLOCK_ARG       /app               # Block if any arg starts with this prefix
ALLOW_FLAG      -l                 # Always allow if this flag is present
```

- `BLOCK_FLAG` — exact match against arguments (e.g., `-f`, `-R`)
- `BLOCK_ARG` — prefix match (e.g., `/app` matches `/app/src/index.ts`)
- `ALLOW_FLAG` — checked first; if present, skips all block checks (e.g., `fdisk -l`)

## Protected Paths

| Path | Why block |
|------|-----------|
| `/app` | Agent-runner source and node_modules — self-modification prevention |
| `/workspace/ipc` | IPC channels to host — agent shouldn't delete or modify these |
| `/tmp/dist` | Compiled agent-runner — tampering changes agent behavior |
| `/usr/`, `/etc/`, `/var/` | System directories |
| `/dev/` | Device files (dd, mv) |

## Wrapped Commands

| Command | What's blocked | Policy file |
|---------|---------------|-------------|
| `rm` | `-f` flag, protected paths | `rm.policy` |
| `mv` | `-f` flag, protected paths, `/dev/` destinations | `mv.policy` |
| `dd` | `of=` to devices and protected paths | `dd.policy` |
| `mkfs` | All device arguments | `mkfs.policy` |
| `fdisk` | `/dev/` access (except `-l` list) | `fdisk.policy` |
| `chmod` | `-R` recursive, setuid bits, protected paths | `chmod.policy` |
| `chown` | `-R` recursive, root ownership, protected paths | `chown.policy` |
| `curl` | `--config`, output to protected paths | `curl.policy` |
| `wget` | Output to protected paths | `wget.policy` |
| `kill` | SIGKILL, SIGSTOP, PID 1 | `kill.policy` |
| `crontab` | `-e` and `-r` (allows `-l` list) | `crontab.policy` |

## Host-Side Monitoring

When safehouse blocks a command, the host IPC watcher (`src/ipc.ts`):

1. Reads `safehouse_alerts.jsonl` from the group's IPC directory
2. Logs the event with group name, command, and reason
3. Sends an alert to the main channel (e.g., "Safehouse blocked 2 destructive commands in group X")
4. Tracks block counts per group for anomaly detection

The agent never sees any of this — alerts flow only to the host.

## Architecture Fit

```
NanoClaw Security Layers:

Layer 1: Container Isolation (Docker/VM)
  → Agent can't access host filesystem or processes

Layer 2: Mount Security (host-side allowlist)
  → Agent can only see explicitly mounted directories

Layer 3: Credential Proxy
  → Agent never sees real API keys or tokens

Layer 4: Safehouse (inside container)
  → Destructive commands silently neutralized
  → Agent believes operations succeeded
  → All events logged for host-side audit

Layer 5: Read-only mounts
  → Project root and global memory are read-only
```

Safehouse fills the gap at Layer 4: even though the agent is sandboxed, it shouldn't be able to destroy its own workspace data or tamper with the agent-runner infrastructure inside the container. And it should never know that it can't.
