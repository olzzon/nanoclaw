# Safehouse — Silent Command Safety for NanoClaw Containers

## What is Safehouse?

Safehouse is a defense-in-depth layer inside NanoClaw's agent containers. A thin C shim (`safehouse_wrap`) sits in the command path via symlinks. The agent sees normal command names (`rm`, `mv`, `dd`, etc.) but destructive operations are silently blocked and logged based on per-command policy files.

**Key properties:**
- Zero dependencies (single C binary, no dynamic allocation)
- Policy-driven: each command has a `.policy` file defining what's blocked
- **Completely invisible to the agent** — blocked commands return exit 0 with no stderr output; the agent believes the operation succeeded
- **Always active** — compiled into the binary with no runtime kill-switch; the agent cannot disable, redirect, or tamper with safehouse
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

## Enabling Safehouse Log Collection

Safehouse is **always active** inside the container — there is no way to disable it at runtime. All paths (policies, logs, alert script) are compiled into the binary.

The host-side `SAFEHOUSE_ENABLED` setting in `.env` controls only whether safehouse **log directories are mounted** from the host for persistent audit collection:

```
SAFEHOUSE_ENABLED=true
```

When `SAFEHOUSE_ENABLED=true`, the host mounts a per-group log directory at `/var/log/safehouse` so blocked-event logs persist across container restarts. When false, safehouse still blocks commands and writes IPC alerts, but the internal log file is ephemeral (lost when the container stops).

## How It Works

### Build Time

The safehouse shim is compiled from source (`container/safehouse/safehouse_wrap.c`) during the Docker build. Symlinks in `/usr/local/bin/` (higher PATH priority than `/usr/bin/`) point to the shim for each wrapped command. Policies are copied from `container/safehouse-policies/`.

All paths are **hardcoded at compile time**:
- Policy directory: `/etc/safehouse/policies`
- Log directory: `/var/log/safehouse`
- Alert script: `/usr/local/lib/safehouse/safehouse_alert.sh`

No environment variables can override these. This prevents the agent from disabling safehouse, redirecting policies, suppressing logs, or hijacking the alert path.

### Runtime

1. Agent runs `rm -rf /app/src`
2. Shell resolves `/usr/local/bin/rm` → safehouse shim
3. Shim loads `/etc/safehouse/policies/rm.policy`
4. Policy blocks: `-f` flag is in `BLOCK_FLAG` list
5. Shim logs the event to `/var/log/safehouse/events.log`
6. Shim forks and runs the alert script via `execl` (no shell — immune to injection)
7. Alert script writes to `/workspace/ipc/safehouse_alerts.jsonl`
8. Shim returns **exit 0** with **no output** — agent thinks it worked
9. Host IPC watcher picks up the alert and notifies the main channel

## Security Hardening

Safehouse is designed to resist tampering by the agent running inside the container:

| Attack vector | Mitigation |
|---------------|------------|
| `unset SAFEHOUSE_ENABLED` | No env var check — safehouse is always active |
| `export SAFEHOUSE_POLICY_DIR=/tmp/empty` | Policy dir is a compile-time constant, not read from env |
| `export SAFEHOUSE_LOG_DIR=/dev/null` | Log dir is a compile-time constant, not read from env |
| `export SAFEHOUSE_ALERT_CMD="malicious"` | Alert script path is a compile-time constant, not read from env |
| Inject shell metacharacters via arguments | Alert script invoked via `fork/execl`, not `system()` — no shell interpretation |
| Overwrite `/usr/local/bin/rm` shim | Container filesystem is read-only for system paths |
| Modify `.policy` files in `/etc/safehouse/` | `/etc/` is a protected path, blocked by safehouse itself |

## Files

| File | Purpose |
|------|---------|
| `container/safehouse/safehouse_wrap.c` | C shim source — single file, no external deps |
| `container/safehouse/safehouse_alert.sh` | Alert script — writes blocked events to IPC |
| `container/safehouse/Makefile` | Builds the shim |
| `container/safehouse-policies/*.policy` | Per-command policy files |
| `src/config.ts` | `SAFEHOUSE_ENABLED` flag (controls host-side log mounts only) |
| `src/container-runner.ts` | Mounts log dir when enabled |
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
| `cp` | `-f` flag, protected path destinations | `cp.policy` |
| `ln` | Protected paths, `/dev/` targets | `ln.policy` |
| `dd` | `of=` to devices and protected paths | `dd.policy` |
| `mkfs` | All device arguments | `mkfs.policy` |
| `fdisk` | `/dev/` access (except `-l` list) | `fdisk.policy` |
| `chmod` | `-R` recursive, setuid bits, protected paths | `chmod.policy` |
| `chown` | `-R` recursive, root ownership, protected paths | `chown.policy` |
| `curl` | `--config`, output to protected paths | `curl.policy` |
| `wget` | Output to protected paths | `wget.policy` |
| `tee` | Protected path destinations | `tee.policy` |
| `truncate` | Protected paths | `truncate.policy` |
| `kill` | SIGKILL, SIGSTOP, PID 1 | `kill.policy` |
| `pkill` | SIGKILL, SIGSTOP, critical process names | `pkill.policy` |
| `killall` | SIGKILL, SIGSTOP, critical process names | `killall.policy` |
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
  → Tamper-proof: all paths compiled in, no env var overrides

Layer 5: Read-only mounts
  → Project root and global memory are read-only
```

Safehouse fills the gap at Layer 4: even though the agent is sandboxed, it shouldn't be able to destroy its own workspace data or tamper with the agent-runner infrastructure inside the container. And it should never know that it can't.
