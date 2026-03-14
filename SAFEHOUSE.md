# Safehouse Integration for NanoClaw

## What is Safehouse?

[Safehouse](https://github.com/qwibitai/safehouse) is an AI-safe Linux overlay that makes destructive OS commands safe by default. It works by placing a thin C shim (`safehouse_wrap`) in the command path. The AI sees normal command names (`rm`, `mv`, `dd`, etc.) but destructive operations are blocked, logged, and trigger alerts based on per-command policy files.

**Key properties:**
- Zero dependencies (single C binary, no dynamic allocation)
- Policy-driven: each command has a `.policy` file defining what's blocked
- Transparent to the AI — it doesn't know commands are wrapped
- Exit code 125 = blocked by policy (distinct from any real binary exit code)
- Full event logging with ISO 8601 timestamps

## Why Add Safehouse Inside NanoClaw Containers?

NanoClaw's containers provide **isolation** — the agent can't escape the sandbox. But inside that sandbox, the agent still has full destructive power over its own workspace. A jailbroken or prompt-injected agent could:

- `rm -rf /workspace/group` — destroy all group data and conversation history
- `dd if=/dev/zero of=/workspace/group/CLAUDE.md` — corrupt memory files
- `mv /workspace/group/conversations /dev/null` — silently destroy archives
- `chmod 000 /workspace/ipc` — break IPC communication
- Overwrite the agent-runner source at `/app/src/` to modify its own behavior

Safehouse adds **defense in depth**: even inside the container, destructive operations are policy-gated. The agent can use `rm`, `mv`, etc. for legitimate work, but dangerous patterns (force flags, system paths, recursive operations on critical directories) are blocked and logged.

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
│         no ──▶ log event, return exit 125        │
│                                                  │
└─────────────────────────────────────────────────┘
```

## Implementation Plan

### Phase 1: Build Safehouse for the Container Image

Safehouse compiles to a single binary with `gcc`. Add it to the NanoClaw Dockerfile.

**Changes to `container/Dockerfile`:**

```dockerfile
# --- Safehouse command-safety layer ---
# Clone and build safehouse (single C binary, no deps beyond gcc)
RUN apt-get update && apt-get install -y gcc git \
    && git clone https://github.com/qwibitai/safehouse.git /tmp/safehouse \
    && cd /tmp/safehouse && make \
    && mkdir -p /usr/local/lib/safehouse /etc/safehouse/policies \
    && cp bin/safehouse_wrap /usr/local/lib/safehouse/ \
    && cp policies/*.policy /etc/safehouse/policies/ \
    && rm -rf /tmp/safehouse \
    && apt-get remove -y gcc \
    && rm -rf /var/lib/apt/lists/*

# Create safe_* symlinks for each wrapped command
# These go in /usr/local/bin which is earlier in PATH than /usr/bin
RUN for cmd in rm mv dd mkfs fdisk chmod chown curl wget kill crontab; do \
      ln -s /usr/local/lib/safehouse/safehouse_wrap /usr/local/bin/$cmd; \
    done

# Safehouse log directory (writable by node user)
RUN mkdir -p /var/log/safehouse && chown node:node /var/log/safehouse
```

**How it works:** When the agent runs `rm -rf /workspace/group`, the shell resolves `/usr/local/bin/rm` (the safehouse symlink) before `/usr/bin/rm`. The shim loads `/etc/safehouse/policies/rm.policy`, checks the args against policy rules, and either blocks (exit 125 + log) or exec's the real `/usr/bin/rm`.

### Phase 2: Container-Specific Policies

The default safehouse policies protect system paths (`/etc/`, `/usr/`, etc.). Inside NanoClaw containers, we also need to protect container-critical paths. Create container-specific policies or extend existing ones.

**New file: `container/safehouse-policies/rm.policy`**
```
REAL_BINARY     /usr/bin/rm
BLOCK_FLAG      -f
BLOCK_ARG       /app/
BLOCK_ARG       /usr/
BLOCK_ARG       /etc/
BLOCK_ARG       /var/
BLOCK_ARG       /workspace/ipc/
BLOCK_ARG       /tmp/dist/
```

**Rationale for each protected path:**
| Path | Why block |
|------|-----------|
| `/app/` | Agent-runner source and node_modules — self-modification prevention |
| `/workspace/ipc/` | IPC channels to host — agent shouldn't delete these |
| `/tmp/dist/` | Compiled agent-runner — tampering changes agent behavior |
| `/usr/`, `/etc/`, `/var/` | System directories (inherited from default policies) |

Similar adjustments for `mv.policy`, `chmod.policy`, `chown.policy`, `dd.policy`.

### Phase 3: Event Log Forwarding

Safehouse logs every invocation (blocked or allowed) to `/var/log/safehouse/events.log`. Forward these events to the host for monitoring.

**Option A — Mount the log directory:**
Add to `container-runner.ts` volume mounts:
```typescript
mounts.push({
  hostPath: path.join(logsDir, 'safehouse'),
  containerPath: '/var/log/safehouse',
  readonly: false,
});
```

**Option B — Alert command:**
Set `SAFEHOUSE_ALERT_CMD` in the container environment to write blocked events to a known IPC location that the host monitors:
```dockerfile
ENV SAFEHOUSE_ALERT_CMD="echo '{\"type\":\"safehouse_blocked\",\"cmd\":\"$SAFEHOUSE_CMD\",\"reason\":\"$SAFEHOUSE_REASON\",\"arg\":\"$SAFEHOUSE_ARG\"}' >> /workspace/ipc/safehouse_alerts.json"
```

The host process can then watch for alerts and notify the user (e.g., via the messaging channel).

### Phase 4: Safehouse-Aware Monitoring (Optional)

Extend NanoClaw's host process to react to safehouse events:

1. **Alert on blocked operations** — If the agent triggers a block, send a message to the main channel: "Agent in group X tried to `rm -rf /app/` — blocked by safehouse"
2. **Anomaly scoring** — Track blocked event frequency. A burst of blocks suggests the agent is actively trying to do something destructive (possible jailbreak)
3. **Session termination** — After N blocks in a time window, kill the container and alert the user

### Phase 5: Skill-Based Installation

Following NanoClaw's philosophy of "skills over features", package this as a Claude Code skill.

**New file: `.claude/skills/add-safehouse/SKILL.md`**

The `/add-safehouse` skill would:
1. Clone the safehouse repo
2. Add the Dockerfile modifications
3. Create container-specific policies
4. Add log mount to `container-runner.ts`
5. Optionally add host-side alert monitoring
6. Rebuild the container image

## What Safehouse Currently Wraps

| Command | What's blocked | Policy file |
|---------|---------------|-------------|
| `rm` | `-f` flag, system paths | `rm.policy` |
| `mv` | `-f` flag, system path sources/destinations | `mv.policy` |
| `dd` | `of=` to devices and system paths | `dd.policy` |
| `mkfs` | All device arguments | `mkfs.policy` |
| `fdisk` | `/dev/` access (except `-l` list) | `fdisk.policy` |
| `chmod` | Recursive, setuid bits, system paths | `chmod.policy` |
| `chown` | Recursive, root ownership, system paths | `chown.policy` |
| `curl` | Output to system paths, `--config` | `curl.policy` |
| `wget` | Output to system paths | `wget.policy` |
| `kill` | SIGKILL, SIGSTOP, PID 1 | `kill.policy` |
| `crontab` | `-e` and `-r` (allows `-l` list) | `crontab.policy` |

## Architecture Fit

```
NanoClaw Security Layers (with Safehouse):

Layer 1: Container Isolation (Docker/VM)
  → Agent can't access host filesystem or processes

Layer 2: Mount Security (host-side allowlist)
  → Agent can only see explicitly mounted directories

Layer 3: Credential Proxy
  → Agent never sees real API keys or tokens

Layer 4: Safehouse (NEW — inside container)
  → Destructive commands are policy-blocked even within the sandbox
  → All command invocations are logged for audit

Layer 5: Read-only mounts
  → Project root and global memory are read-only
```

Safehouse fills the gap at Layer 4: even though the agent is sandboxed, it shouldn't be able to destroy its own workspace data or tamper with the agent-runner infrastructure inside the container.
