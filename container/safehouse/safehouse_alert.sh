#!/bin/sh
# Safehouse alert script — appends blocked events to IPC for host-side monitoring.
# Called by safehouse_wrap via SAFEHOUSE_ALERT_CMD with env vars:
#   SAFEHOUSE_CMD, SAFEHOUSE_REASON, SAFEHOUSE_ARG
TS=$(date -u +%Y-%m-%dT%H:%M:%SZ)

# Escape JSON-special characters: \ → \\, " → \", newline → \n, tab → \t
json_escape() {
    printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e ':a' -e 'N' -e '$!ba' -e 's/\n/\\n/g' -e 's/\t/\\t/g'
}

ESC_CMD=$(json_escape "$SAFEHOUSE_CMD")
ESC_REASON=$(json_escape "$SAFEHOUSE_REASON")
ESC_ARG=$(json_escape "$SAFEHOUSE_ARG")

printf '{"type":"safehouse_blocked","cmd":"%s","reason":"%s","arg":"%s","ts":"%s"}\n' \
    "$ESC_CMD" "$ESC_REASON" "$ESC_ARG" "$TS" \
    >> /workspace/ipc/safehouse_alerts.jsonl 2>/dev/null
