# NanoClaw Agent Container
# Runs Claude Agent SDK in isolated Linux VM with browser automation

FROM node:22-slim

# Install system dependencies for Chromium
RUN apt-get update && apt-get install -y \
    chromium \
    fonts-liberation \
    fonts-noto-cjk \
    fonts-noto-color-emoji \
    libgbm1 \
    libnss3 \
    libatk-bridge2.0-0 \
    libgtk-3-0 \
    libx11-xcb1 \
    libxcomposite1 \
    libxdamage1 \
    libxrandr2 \
    libasound2 \
    libpangocairo-1.0-0 \
    libcups2 \
    libdrm2 \
    libxshmfence1 \
    curl \
    git \
    gcc \
    make \
    && rm -rf /var/lib/apt/lists/*

# --- Safehouse command-safety layer ---
# Build safehouse shim (single C binary, no external deps)
COPY safehouse/ /tmp/safehouse/
RUN cd /tmp/safehouse && make \
    && mkdir -p /usr/local/lib/safehouse /etc/safehouse/policies \
    && cp bin/safehouse_wrap /usr/local/lib/safehouse/ \
    && cp safehouse_alert.sh /usr/local/lib/safehouse/ \
    && chmod +x /usr/local/lib/safehouse/safehouse_alert.sh \
    && rm -rf /tmp/safehouse

# Copy container-specific policies (overrides defaults)
COPY safehouse-policies/*.policy /etc/safehouse/policies/

# Create symlinks so safehouse intercepts destructive commands
# /usr/local/bin is earlier in PATH than /usr/bin
RUN for cmd in rm mv dd mkfs fdisk chmod chown curl wget kill crontab; do \
      ln -sf /usr/local/lib/safehouse/safehouse_wrap /usr/local/bin/$cmd; \
    done

# Safehouse log directory (use real chmod to bypass safehouse during build)
RUN mkdir -p /var/log/safehouse && /usr/bin/chmod 777 /var/log/safehouse

# Safehouse log and alert configuration
ENV SAFEHOUSE_LOG_DIR=/var/log/safehouse
ENV SAFEHOUSE_ALERT_CMD=/usr/local/lib/safehouse/safehouse_alert.sh

# Remove build tools no longer needed (use real rm to bypass safehouse during build)
RUN apt-get purge -y gcc make && apt-get autoremove -y && /usr/bin/rm -rf /var/lib/apt/lists/*

# Set Chromium path for agent-browser
ENV AGENT_BROWSER_EXECUTABLE_PATH=/usr/bin/chromium
ENV PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH=/usr/bin/chromium

# Install agent-browser and claude-code globally
RUN npm install -g agent-browser @anthropic-ai/claude-code

# Create app directory
WORKDIR /app

# Copy package files first for better caching
COPY agent-runner/package*.json ./

# Install dependencies
RUN npm install

# Copy source code
COPY agent-runner/ ./

# Build TypeScript
RUN npm run build

# Create workspace directories
RUN mkdir -p /workspace/group /workspace/global /workspace/extra /workspace/ipc/messages /workspace/ipc/tasks /workspace/ipc/input

# Create entrypoint script
# Container input (prompt, group info) is passed via stdin JSON.
# Credentials are injected by the host's credential proxy — never passed here.
# Follow-up messages arrive via IPC files in /workspace/ipc/input/
# Entrypoint uses real binaries (bypasses safehouse) for trusted setup operations
RUN printf '#!/bin/bash\nset -e\ncd /app && npx tsc --outDir /tmp/dist 2>&1 >&2\nln -s /app/node_modules /tmp/dist/node_modules\n/usr/bin/chmod -R a-w /tmp/dist\ncat > /tmp/input.json\nnode /tmp/dist/index.js < /tmp/input.json\n' > /app/entrypoint.sh && /usr/bin/chmod +x /app/entrypoint.sh

# Set ownership to node user (non-root) for writable directories
# Use real binaries to bypass safehouse during build
RUN /usr/bin/chown -R node:node /workspace && /usr/bin/chmod 777 /home/node

# Switch to non-root user (required for --dangerously-skip-permissions)
USER node

# Set working directory to group workspace
WORKDIR /workspace/group

# Entry point reads JSON from stdin, outputs JSON to stdout
ENTRYPOINT ["/app/entrypoint.sh"]
