#!/bin/bash
# Build the NanoClaw agent container image

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

IMAGE_NAME="nanoclaw-agent"
TAG="${1:-latest}"
CONTAINER_RUNTIME="${CONTAINER_RUNTIME:-docker}"
# SAFEHOUSE=1 (enabled) or SAFEHOUSE=0 (default, disabled)
SAFEHOUSE="${SAFEHOUSE:-0}"

if [ "$SAFEHOUSE" = "1" ]; then
  echo "Building NanoClaw agent container image (safehouse ENABLED)..."
else
  echo "Building NanoClaw agent container image..."
fi
echo "Image: ${IMAGE_NAME}:${TAG}"

${CONTAINER_RUNTIME} build --build-arg SAFEHOUSE="${SAFEHOUSE}" -t "${IMAGE_NAME}:${TAG}" .

echo ""
echo "Build complete!"
echo "Image: ${IMAGE_NAME}:${TAG}"
echo ""
echo "Test with:"
echo "  echo '{\"prompt\":\"What is 2+2?\",\"groupFolder\":\"test\",\"chatJid\":\"test@g.us\",\"isMain\":false}' | ${CONTAINER_RUNTIME} run -i ${IMAGE_NAME}:${TAG}"
