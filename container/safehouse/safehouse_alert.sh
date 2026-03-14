#!/bin/sh
# Safehouse alert script — appends blocked events to IPC for host-side monitoring.
# Called by safehouse_wrap via SAFEHOUSE_ALERT_CMD with env vars:
#   SAFEHOUSE_CMD, SAFEHOUSE_REASON, SAFEHOUSE_ARG
TS=$(date -u +%Y-%m-%dT%H:%M:%SZ)
printf '{"type":"safehouse_blocked","cmd":"%s","reason":"%s","arg":"%s","ts":"%s"}\n' \
    "$SAFEHOUSE_CMD" "$SAFEHOUSE_REASON" "$SAFEHOUSE_ARG" "$TS" \
    >> /workspace/ipc/safehouse_alerts.jsonl 2>/dev/null
