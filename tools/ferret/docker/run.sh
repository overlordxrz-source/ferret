#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
#
# Run a command inside the Ferret build container, creating it if needed.
#
#   tools/ferret/docker/run.sh <command> [args...]
#
# The host project directory (default ~/Projects) is mounted at /mnt/projects
# and the persistent Ferret cache (default ~/.cache/ferret) at /mnt/cache.
set -euo pipefail

IMAGE=${FERRET_IMAGE:-ferret-buildenv:1}
NAME=${FERRET_CONTAINER:-ferret-build}
HOST_PROJECTS=${HOST_PROJECTS:-$HOME/Projects}
HOST_CACHE=${HOST_CACHE:-$HOME/.cache/ferret}
HERE=$(cd "$(dirname "$0")" && pwd)

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	echo "==> building $IMAGE from $HERE" >&2
	docker build -t "$IMAGE" "$HERE" >&2
fi

mkdir -p "$HOST_CACHE"

if ! docker inspect "$NAME" >/dev/null 2>&1; then
	echo "==> creating container $NAME" >&2
	docker run -d --name "$NAME" \
		-v "$HOST_PROJECTS:/mnt/projects" \
		-v "$HOST_CACHE:/mnt/cache" \
		-w /build "$IMAGE" sleep infinity >/dev/null
elif [ "$(docker inspect -f '{{.State.Running}}' "$NAME")" != "true" ]; then
	docker start "$NAME" >/dev/null
fi

# The caller names artifacts by their host path; the container sees the
# project tree under /mnt/projects. Passing the host path through unchanged
# makes the boot write into the container's own filesystem, where the host
# script then cannot read it (and reports "the kernel did not boot").
if [ -n "${ART:-}" ]; then
	case "$ART" in
	"$HOST_PROJECTS"/*)
		ART="/mnt/projects/${ART#"$HOST_PROJECTS"/}"
		;;
	/mnt/*)
		# Already a container path.
		;;
	*)
		echo "run.sh: ART=$ART is not under $HOST_PROJECTS, so the" >&2
		echo "        container cannot write to it. Use a path under the" >&2
		echo "        project tree (or /mnt/... as a container path)." >&2
		exit 2
		;;
	esac
fi

exec docker exec -i -e CLEAN -e JOBS -e ARCH -e OUTROOT -e SRC_MOUNT -e EXTRA_CONFIG -e ART "$NAME" "$@"
