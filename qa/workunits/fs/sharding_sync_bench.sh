#!/bin/bash
#
# Sharding Sync Benchmark
#
# Simulates NFS sync behavior (kernel mount with -o sync,wsync) across
# multiple subvolumes to measure MDS sharding impact on synchronous workloads.
#
# Can run locally on a Ceph node or remotely from any machine with ceph CLI
# and mount.ceph installed.
#
# Usage: ./sharding_sync_bench.sh [OPTIONS]
#
# See --help for full option list.

set -euo pipefail

# ─── Defaults ───────────────────────────────────────────────────────────────

NUM_CLIENTS=8
DURATION=120
WORKER_GRACE=0
SYNC_MODE=1
SHARDED=0
DATA_ONLY=0
META_ONLY=0
BS="4k"
FILESIZE="64m"

# Connectivity (auto-detect by default)
MON_ADDR=""
CEPH_CONF_PATH=""
KEYRING_PATH=""
CLIENT_NAME=""
FS_NAME=""

# ─── Parse arguments ───────────────────────────────────────────────────────

usage() {
    cat <<'EOF'
Usage: sharding_sync_bench.sh [OPTIONS]

Workload options:
  --clients N       Number of subvolume clients (default: 8)
  --duration SECS   Test duration in seconds (default: 120)
  --worker-grace SECS After duration, seconds to wait before killing stuck
                    workers (default: 0 = kill immediately)
  --sync 0|1        Mount with -o sync,wsync (1) or nowsync (0). Default: 1
  --sharded         Enable MDS sharding on subvolumes
  --data-only       Skip metadata workload
  --meta-only       Skip data workload
  --bs SIZE         Block size for data I/O (default: 4k)
  --filesize SIZE   File size for data I/O (default: 64m)

Cluster connectivity (for remote execution):
  --mon ADDR        Monitor address(es), comma-separated
  --ceph-conf PATH  Path to ceph.conf
  --keyring PATH    Path to keyring file
  --client-name ID  Ceph client name (default: client.admin)
  --fs-name NAME    CephFS filesystem name (default: auto-detect)

Examples:
  # Local, sync, no sharding (baseline)
  ./sharding_sync_bench.sh --sync 1 --clients 8

  # Local, sync, with sharding
  ./sharding_sync_bench.sh --sync 1 --clients 8 --sharded

  # Remote execution
  ./sharding_sync_bench.sh --sync 1 --clients 8 --sharded \
    --mon 10.0.0.1,10.0.0.2 --ceph-conf ./ceph.conf --keyring ./keyring
EOF
    exit 0
}

require_arg() {
    local opt="$1"
    if [[ $# -lt 2 || -z "${2:-}" || "$2" == --* ]]; then
        echo "ERROR: $opt requires a value" >&2
        exit 1
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --clients)
            require_arg "$1" "${2:-}"
            NUM_CLIENTS="$2"; shift 2 ;;
        --duration)
            require_arg "$1" "${2:-}"
            DURATION="$2"; shift 2 ;;
        --worker-grace)
            require_arg "$1" "${2:-}"
            WORKER_GRACE="$2"; shift 2 ;;
        --sync)
            require_arg "$1" "${2:-}"
            SYNC_MODE="$2"; shift 2 ;;
        --sharded)      SHARDED=1; shift ;;
        --data-only)    DATA_ONLY=1; shift ;;
        --meta-only)    META_ONLY=1; shift ;;
        --bs)
            require_arg "$1" "${2:-}"
            BS="$2"; shift 2 ;;
        --filesize)
            require_arg "$1" "${2:-}"
            FILESIZE="$2"; shift 2 ;;
        --mon)
            require_arg "$1" "${2:-}"
            MON_ADDR="$2"; shift 2 ;;
        --ceph-conf)
            require_arg "$1" "${2:-}"
            CEPH_CONF_PATH="$2"; shift 2 ;;
        --keyring)
            require_arg "$1" "${2:-}"
            KEYRING_PATH="$2"; shift 2 ;;
        --client-name)
            require_arg "$1" "${2:-}"
            CLIENT_NAME="$2"; shift 2 ;;
        --fs-name)
            require_arg "$1" "${2:-}"
            FS_NAME="$2"; shift 2 ;;
        --help|-h)      usage ;;
        *) echo "Unknown option: $1"; usage ;;
    esac
done

# ─── Connectivity setup ────────────────────────────────────────────────────

CEPH_ARGS=""

if [[ -n "$CEPH_CONF_PATH" ]]; then
    :
elif [[ -f /etc/ceph/ceph.conf ]]; then
    CEPH_CONF_PATH="/etc/ceph/ceph.conf"
elif [[ -n "${CEPH_CONF:-}" ]]; then
    CEPH_CONF_PATH="$CEPH_CONF"
fi

if [[ -n "$KEYRING_PATH" ]]; then
    :
fi

if [[ -n "$CLIENT_NAME" ]]; then
    :
else
    CLIENT_NAME="client.admin"
fi

if [[ -n "$MON_ADDR" ]]; then
    :
fi

# Prefer build tree ceph when running from a vstart checkout.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
if [[ -z "${CEPH_BIN:-}" ]]; then
    if [[ -x "$REPO_ROOT/build/bin/ceph" ]]; then
        CEPH_BIN="$REPO_ROOT/build/bin/ceph"
    else
        CEPH_BIN="ceph"
    fi
fi

rebuild_ceph_args() {
    CEPH_ARGS=""
    if [[ -n "$CEPH_CONF_PATH" && -f "$CEPH_CONF_PATH" ]]; then
        CEPH_ARGS+=" --conf $(readlink -f "$CEPH_CONF_PATH")"
    fi
    if [[ -n "$KEYRING_PATH" && -f "$KEYRING_PATH" ]]; then
        CEPH_ARGS+=" --keyring $(readlink -f "$KEYRING_PATH")"
    fi
    CEPH_ARGS+=" --name $CLIENT_NAME"
    if [[ -n "$MON_ADDR" ]]; then
        CEPH_ARGS+=" -m $MON_ADDR"
    fi
}
rebuild_ceph_args
export CEPH_ARGS

ceph_cmd() {
    # shellcheck disable=SC2086
    "$CEPH_BIN" $CEPH_ARGS "$@"
}

ceph_cmd_timeout() {
    local secs="$1"
    shift
    # timeout(1) cannot invoke shell functions; call ceph directly.
    # shellcheck disable=SC2086
    run_timeout "$secs" "$CEPH_BIN" $CEPH_ARGS "$@"
}

run_timeout() {
    local secs="$1"
    shift
    if command -v timeout >/dev/null 2>&1; then
        timeout --kill-after=5 "$secs" "$@"
    else
        "$@" &
        local pid=$!
        local waited=0
        while kill -0 "$pid" 2>/dev/null && [[ $waited -lt $secs ]]; do
            sleep 1
            waited=$((waited + 1))
        done
        if kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            return 124
        fi
        wait "$pid" 2>/dev/null || true
    fi
}

# wait(1) is a bash builtin; timeout(1) cannot invoke it. Poll worker PIDs
# instead and reap exited children.
wait_workers_timeout() {
    local secs="${1:-5}"
    local waited=0
    while [[ $waited -lt $secs ]]; do
        local alive=0
        for pid in "${WORKER_PIDS[@]}"; do
            if kill -0 "$pid" 2>/dev/null; then
                alive=$((alive + 1))
            else
                wait "$pid" 2>/dev/null || true
            fi
        done
        [[ $alive -eq 0 ]] && return 0
        sleep 1
        waited=$((waited + 1))
    done
    return 124
}

get_active_mds_for_fs() {
    local fs_name="$1"
    local out
    out=$(ceph_cmd_timeout 10 fs get "$fs_name" --format=json 2>/dev/null \
        | python3 -c "
import sys, json
try:
    fs = json.load(sys.stdin)
except Exception:
    sys.exit(1)
for info in fs.get('mdsmap', {}).get('info', {}).values():
    if 'up:active' in info.get('state', ''):
        print(info['name'])
        sys.exit(0)
sys.exit(1)
" 2>/dev/null) || return 1
    [[ -n "$out" ]] || return 1
    echo "$out"
}

resolve_mon_addr() {
    if [[ -n "$MON_ADDR" ]]; then
        echo "$MON_ADDR"
        return 0
    fi
    ceph_cmd_timeout 10 mon dump --format=json 2>/dev/null \
        | python3 -c "
import sys, json, re
d = json.load(sys.stdin)
addrs = []
for m in d.get('mons', []):
    a = m.get('addr', '')
    if not a:
        continue
    if 'v1:' in a or 'v2:' in a:
        m1 = re.search(r'v1:([^,\\]]+)', a)
        if m1:
            addrs.append(m1.group(1))
            continue
        m2 = re.search(r'v2:([^,\\]]+)', a)
        if m2:
            addrs.append(m2.group(1))
            continue
    addrs.append(a.split('/')[0])
print('/'.join(addrs))
" 2>/dev/null
}

setup_mount_env() {
    if [[ "$CEPH_BIN" == "$REPO_ROOT/build/bin/ceph" ]]; then
        export PATH="$REPO_ROOT/build/bin:$PATH"
        export LD_LIBRARY_PATH="${REPO_ROOT}/build/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    fi
    if [[ -n "$CEPH_CONF_PATH" && -f "$CEPH_CONF_PATH" ]]; then
        export CEPH_CONF="$(readlink -f "$CEPH_CONF_PATH")"
    fi
}

resolve_mount_ceph() {
    if [[ -x "$REPO_ROOT/build/bin/mount.ceph" ]]; then
        echo "$REPO_ROOT/build/bin/mount.ceph"
    elif command -v mount.ceph &>/dev/null; then
        command -v mount.ceph
    fi
}

build_mount_opts() {
    local opts=""
    if [[ $SYNC_MODE -eq 1 ]]; then
        opts="sync,wsync"
    else
        opts="nowsync"
    fi
    if [[ -n "$CEPH_CONF_PATH" && -f "$CEPH_CONF_PATH" ]]; then
        opts="${opts},conf=$(readlink -f "$CEPH_CONF_PATH")"
    fi
    if [[ -n "${MOUNT_SECRET_FILE:-}" && -f "$MOUNT_SECRET_FILE" ]]; then
        opts="${opts},secretfile=$MOUNT_SECRET_FILE"
    fi
    if [[ -n "${CLUSTER_FSID:-}" ]]; then
        opts="${opts},fsid=$CLUSTER_FSID"
    fi
    local mon
    mon=$(resolve_mon_addr)
    if [[ -n "$mon" ]]; then
        opts="${opts},mon_addr=$mon"
    fi
    echo "$opts"
}

preflight_check() {
    echo "=== Preflight checks ==="
    if ! ceph_cmd_timeout 10 status >/dev/null 2>&1; then
        echo "ERROR: Cannot reach cluster with: $CEPH_BIN $CEPH_ARGS status"
        echo "       Check --ceph-conf / --keyring paths (vstart: build/ceph.conf build/keyring)."
        return 1
    fi

    if ! ceph_cmd_timeout 10 fs ls --format=json 2>/dev/null \
        | python3 -c "import sys,json; fs=json.load(sys.stdin); names=[f['name'] for f in fs]; sys.exit(0 if '$FS_NAME' in names else 1)" 2>/dev/null; then
        echo "ERROR: Filesystem '$FS_NAME' not found."
        echo "       Available:"
        ceph_cmd_timeout 10 fs ls 2>/dev/null || true
        return 1
    fi

    if ! ceph_cmd_timeout 10 fs volume ls --format=json 2>/dev/null \
        | python3 -c "import sys,json; vols=json.load(sys.stdin); names=[v['name'] for v in vols]; sys.exit(0 if '$FS_NAME' in names else 1)" 2>/dev/null; then
        echo "ERROR: '$FS_NAME' is not a subvolume-capable volume."
        echo "       Create one with: ceph fs volume create $FS_NAME"
        return 1
    fi

    if ! ceph_cmd_timeout 10 mgr module ls --format=json 2>/dev/null \
        | python3 -c "
import sys, json
m = json.load(sys.stdin)
mods = set(m.get('enabled_modules', [])) | set(m.get('always_on_modules', []))
sys.exit(0 if 'volumes' in mods else 1)
" 2>/dev/null; then
        echo "ERROR: mgr volumes module is not enabled."
        echo "       Try: ceph mgr module enable volumes"
        return 1
    fi

    local active_mds mds_state mount_helper
    if ! active_mds=$(get_active_mds_for_fs "$FS_NAME"); then
        mds_state=$(ceph_cmd_timeout 10 mds stat 2>/dev/null \
            || echo "unavailable (timed out or no MDS)")
        echo "ERROR: No active MDS for filesystem '$FS_NAME'."
        echo "       Start or recover MDS before running the benchmark."
        echo "       mds stat: $mds_state"
        return 1
    fi

    setup_mount_env
    mount_helper=$(resolve_mount_ceph || true)
    if [[ -z "$mount_helper" ]]; then
        echo "ERROR: mount.ceph not found (kernel CephFS client required)."
        echo "       vstart: build/bin/mount.ceph or install ceph-common."
        return 1
    fi

    mds_state=$(ceph_cmd_timeout 10 mds stat 2>/dev/null || echo "unknown")
    echo "  Cluster: OK"
    echo "  FS:      $FS_NAME"
    echo "  MDS:     $mds_state (active: $active_mds)"
    echo "  mount:   $mount_helper"
    echo ""
    return 0
}

if [[ -z "$FS_NAME" ]]; then
    FS_NAME=$(ceph_cmd fs ls --format=json 2>/dev/null \
        | python3 -c "import sys,json; print(json.load(sys.stdin)[0]['name'])" 2>/dev/null) \
        || { echo "ERROR: Cannot detect filesystem name. Use --fs-name."; exit 1; }
fi

# Extract client id from client name (e.g. "client.admin" -> "admin")
CLIENT_ID="${CLIENT_NAME#client.}"

echo "============================================"
echo "  Sharding Sync Benchmark"
echo "============================================"
echo "Filesystem:  $FS_NAME"
echo "Clients:     $NUM_CLIENTS"
echo "Duration:    ${DURATION}s"
echo "Sync mode:   $SYNC_MODE (1=sync+wsync, 0=nowsync)"
echo "Sharded:     $SHARDED"
echo "Data I/O:    bs=$BS filesize=$FILESIZE"
if [[ $DATA_ONLY -eq 1 ]]; then echo "Workload:    data-only"; fi
if [[ $META_ONLY -eq 1 ]]; then echo "Workload:    meta-only"; fi
if [[ -n "$MON_ADDR" ]]; then echo "Monitors:    $MON_ADDR"; fi
if [[ -n "$CEPH_CONF_PATH" ]]; then echo "Ceph conf:   $CEPH_CONF_PATH"; fi
echo "Ceph bin:    $CEPH_BIN"
echo "============================================"
echo ""

preflight_check || exit 1

# ─── Privilege check ───────────────────────────────────────────────────────

if [[ $EUID -ne 0 ]]; then
    echo "ERROR: Kernel mount requires root. Run with sudo."
    exit 1
fi

# ─── Working directories ──────────────────────────────────────────────────

WORK_DIR=$(mktemp -d /tmp/sharding_bench_XXXXXX)
MOUNT_BASE="$WORK_DIR/mnt"
RESULTS_DIR="$WORK_DIR/results"
METRICS_DIR="$WORK_DIR/metrics"
mkdir -p "$MOUNT_BASE" "$RESULTS_DIR" "$METRICS_DIR"

# ─── Cleanup ──────────────────────────────────────────────────────────────

CLEANUP_DONE=0
CLEANUP_CEPH_TIMEOUT=10
CLEANUP_EXIT=0
BENCH_EXIT=0
WORKER_PIDS=()
WORKER_LABELS=()

umount_mount() {
    local mp="$1"
    mountpoint -q "$mp" 2>/dev/null || return 0

    echo "  Unmounting $mp ..."
    if run_timeout 5 umount "$mp" 2>/dev/null; then
        return 0
    fi

    echo "  Force unmount $mp (-f) ..."
    if run_timeout 5 umount -f "$mp" 2>/dev/null; then
        return 0
    fi

    echo "  Lazy unmount $mp (-l) ..."
    if run_timeout 5 umount -l "$mp" 2>/dev/null; then
        return 0
    fi

    if command -v fuser >/dev/null 2>&1; then
        echo "  Killing processes using $mp ..."
        fuser -km "$mp" 2>/dev/null || true
        sleep 1
        if run_timeout 5 umount -f "$mp" 2>/dev/null \
            || run_timeout 5 umount -l "$mp" 2>/dev/null; then
            return 0
        fi
    fi

    if mountpoint -q "$mp" 2>/dev/null; then
        echo "  ERROR: could not unmount $mp"
        return 1
    fi
    return 0
}

remove_subvolume() {
    local vol="$1"
    echo "  Removing subvolume $vol ..."
    if ceph_cmd_timeout "$CLEANUP_CEPH_TIMEOUT" \
        fs subvolume rm "$FS_NAME" "$vol" --force; then
        return 0
    fi

    echo "  Force retry $vol (longer timeout) ..."
    if ceph_cmd_timeout $((CLEANUP_CEPH_TIMEOUT * 3)) \
        fs subvolume rm "$FS_NAME" "$vol" --force; then
        return 0
    fi

    echo "  ERROR: could not remove subvolume $vol"
    return 1
}

cleanup() {
    [[ "$CLEANUP_DONE" -eq 1 ]] && return 0
    CLEANUP_DONE=1
    trap - EXIT INT TERM
    set +e

    local cleanup_fail=0

    echo ""
    echo "=== Cleaning up ==="

    if [[ -n "${WORKER_PIDS:-}" ]]; then
        for pid in "${WORKER_PIDS[@]}"; do
            kill -0 "$pid" 2>/dev/null && kill -9 "$pid" 2>/dev/null || true
        done
    fi
    jobs -p 2>/dev/null | xargs -r kill -9 2>/dev/null || true
    wait_workers_timeout 5 || true

    if [[ -n "${MOUNT_BASE:-}" ]]; then
        for i in $(seq 1 "$NUM_CLIENTS"); do
            local mp="$MOUNT_BASE/$i"
            umount_mount "$mp" || cleanup_fail=1
        done
    fi

    if [[ -n "${FS_NAME:-}" ]]; then
        if ceph_cmd_timeout 5 status >/dev/null 2>&1; then
            for i in $(seq 1 "$NUM_CLIENTS"); do
                remove_subvolume "bench_$i" || cleanup_fail=1
            done
        else
            echo "  ERROR: cluster unreachable; cannot remove subvolumes bench_1..bench_$NUM_CLIENTS"
            cleanup_fail=1
        fi
    fi

    if [[ -n "${WORK_DIR:-}" && -d "$WORK_DIR" ]]; then
        if ! rm -rf "$WORK_DIR" 2>/dev/null; then
            echo "  ERROR: could not remove work dir $WORK_DIR"
            cleanup_fail=1
        fi
    fi

    if [[ $cleanup_fail -ne 0 ]]; then
        CLEANUP_EXIT=1
        echo ""
        echo "ERROR: cleanup incomplete (see errors above)"
    else
        echo "Cleanup complete"
    fi

    if [[ ${BENCH_EXIT:-0} -ne 0 || ${CLEANUP_EXIT:-0} -ne 0 ]]; then
        exit 1
    fi
}
trap cleanup EXIT INT TERM

# ─── Create subvolumes ────────────────────────────────────────────────────

echo "=== Creating $NUM_CLIENTS subvolumes ==="
for i in $(seq 1 "$NUM_CLIENTS"); do
    if ! ceph_cmd fs subvolume create "$FS_NAME" "bench_$i" --mode=777; then
        echo "ERROR: failed to create subvolume bench_$i on fs $FS_NAME"
        exit 1
    fi
    echo "  Created bench_$i"
done

if [[ $SHARDED -eq 1 ]]; then
    echo ""
    echo "=== Enabling MDS parallel sharding POC ==="
    if ceph_cmd config set mds mds_subvolume_sharding_parallel_poc true; then
        echo "  mds_subvolume_sharding_parallel_poc = true"
    else
        echo "  ERROR: failed to set mds_subvolume_sharding_parallel_poc" >&2
        exit 1
    fi
else
    echo ""
    echo "=== Ensuring MDS parallel sharding POC is disabled (baseline) ==="
    ceph_cmd config set mds mds_subvolume_sharding_parallel_poc false 2>/dev/null \
        || echo "  WARNING: failed to reset mds_subvolume_sharding_parallel_poc"
fi

# ─── Mount subvolumes ─────────────────────────────────────────────────────

echo ""
echo "=== Mounting subvolumes (kernel client) ==="

setup_mount_env
MOUNT_CEPH=$(resolve_mount_ceph)
MOUNT_SECRET_FILE="$WORK_DIR/mount.secret"
if ! ceph_cmd auth get-key "$CLIENT_NAME" > "$MOUNT_SECRET_FILE" 2>/dev/null; then
    echo "ERROR: cannot get auth key for $CLIENT_NAME"
    exit 1
fi
chmod 600 "$MOUNT_SECRET_FILE"
CLUSTER_FSID=$(ceph_cmd_timeout 10 fsid 2>/dev/null | tr -d '[:space:]')
if [[ -z "$CLUSTER_FSID" ]]; then
    echo "ERROR: cannot determine cluster fsid"
    exit 1
fi
MOUNT_OPTS=$(build_mount_opts)
echo "  mount helper: $MOUNT_CEPH"
echo "  mount opts:   $MOUNT_OPTS"

MOUNTED=0
for i in $(seq 1 "$NUM_CLIENTS"); do
    SUBVOL_PATH=$(ceph_cmd fs subvolume getpath "$FS_NAME" "bench_$i" 2>/dev/null) \
        || { echo "  ERROR: cannot get path for bench_$i"; continue; }

    mkdir -p "$MOUNT_BASE/$i"

    DEVICE="${CLIENT_ID}@.${FS_NAME}=${SUBVOL_PATH}"

    MOUNT_ERR=$(mktemp)
    if "$MOUNT_CEPH" "$DEVICE" "$MOUNT_BASE/$i" -o "$MOUNT_OPTS" 2>"$MOUNT_ERR"; then
        echo "  Mounted bench_$i at $MOUNT_BASE/$i (sync=$SYNC_MODE)"
        MOUNTED=$((MOUNTED + 1))
    else
        echo "  ERROR: mount failed for bench_$i: $(tr '\n' ' ' < "$MOUNT_ERR")"
    fi
    rm -f "$MOUNT_ERR"
done

echo ""
echo "Mounted: $MOUNTED / $NUM_CLIENTS"

if [[ $MOUNTED -eq 0 ]]; then
    echo "ERROR: No mounts succeeded."
    exit 1
fi

# ─── Detect tools ─────────────────────────────────────────────────────────

HAS_FIO=0
if command -v fio &>/dev/null; then
    HAS_FIO=1
    echo "fio detected — using for data I/O"
else
    echo "fio not found — falling back to dd"
fi

# ─── Collect baseline metrics ─────────────────────────────────────────────

collect_metrics() {
    local tag="$1"
    # Use 'ceph tell' for remote-safe metrics collection
    local active_mds
    active_mds=$(ceph_cmd status --format=json 2>/dev/null \
        | python3 -c "
import sys,json
d=json.load(sys.stdin)
fs=d.get('fsmap',{}).get('by_rank',[])
for r in fs:
    if r.get('status','') == 'up:active':
        print(r['name']); break
" 2>/dev/null) || true

    if [[ -z "$active_mds" ]]; then
        active_mds=$(ceph_cmd mds stat --format=json 2>/dev/null \
            | python3 -c "
import sys,json
d=json.load(sys.stdin)
for fs in d.get('fsmap',{}).get('filesystems',[]):
    for info in fs.get('mdsmap',{}).get('info',{}).values():
        if 'up:active' in info.get('state',''):
            print(info['name']); break
" 2>/dev/null) || true
    fi

    if [[ -n "$active_mds" ]]; then
        ceph_cmd tell "mds.$active_mds" counter dump \
            > "$METRICS_DIR/${tag}_counter_dump.json" 2>/dev/null || true
        ceph_cmd tell "mds.$active_mds" perf dump mds_server \
            > "$METRICS_DIR/${tag}_perf_mds_server.json" 2>/dev/null || true
    fi
    ceph_cmd fs perf stats "$FS_NAME" --format=json \
        > "$METRICS_DIR/${tag}_perf_stats.json" 2>/dev/null || true
}

echo ""
echo "=== Collecting baseline metrics ==="
collect_metrics "before"

# ─── Worker functions ─────────────────────────────────────────────────────

metadata_worker() {
    local id=$1
    local mnt="$MOUNT_BASE/$id"
    local dur=$2
    local result_file="$RESULTS_DIR/meta_$id"
    local start_time
    start_time=$(date +%s)
    local end_time=$(( start_time + dur ))
    local ops=0
    local cycles=0

    echo "[$(date +%H:%M:%S)] [meta $id] worker started on $mnt (${dur}s)"
    cd "$mnt" || return 1

    while [[ $(date +%s) -lt $end_time ]]; do
        cycles=$((cycles + 1))
        local elapsed=$(( $(date +%s) - start_time ))
        local d="batch_${cycles}"

        echo "[$(date +%H:%M:%S)] [meta $id] cycle $cycles (${elapsed}s): mkdir tree $d/"
        mkdir -p "$d/a/b/c/d/e" 2>/dev/null || true
        ops=$((ops + 5))

        echo "[$(date +%H:%M:%S)] [meta $id] cycle $cycles: sync writes (100 files)"
        for f in $(seq 1 100); do
            dd if=/dev/zero of="$d/a/b/f_$f" bs=1 count=1 oflag=sync 2>/dev/null || true
        done
        ops=$((ops + 100))

        echo "[$(date +%H:%M:%S)] [meta $id] cycle $cycles: stat/rename/ls/rm"
        for f in $(seq 1 100); do
            stat "$d/a/b/f_$f" &>/dev/null || true
        done
        ops=$((ops + 100))

        for f in $(seq 1 50); do
            mv "$d/a/b/f_$f" "$d/a/b/c/f_$f" 2>/dev/null || true
        done
        ops=$((ops + 50))

        ls "$d/a/b/" &>/dev/null || true
        ls "$d/a/b/c/" &>/dev/null || true
        ops=$((ops + 2))

        rm -rf "$d" 2>/dev/null || true
        ops=$((ops + 100))

        elapsed=$(( $(date +%s) - start_time ))
        echo "[$(date +%H:%M:%S)] [meta $id] cycle $cycles done: ~$ops ops total (${elapsed}s elapsed)"
    done

    elapsed=$(( $(date +%s) - start_time ))
    echo "$ops" > "$result_file"
    echo "[$(date +%H:%M:%S)] [meta $id] finished: $cycles cycles, $ops ops in ${elapsed}s"
}

data_worker() {
    local id=$1
    local mnt="$MOUNT_BASE/$id"
    local dur=$2
    local result_file="$RESULTS_DIR/data_$id"
    local start_time
    start_time=$(date +%s)
    local end_time=$(( start_time + dur ))
    local bytes_written=0
    local cycles=0
    local datafile="$mnt/data_bench_file"

    echo "[$(date +%H:%M:%S)] [data $id] worker started on $mnt (${dur}s)"
    while [[ $(date +%s) -lt $end_time ]]; do
        cycles=$((cycles + 1))
        local elapsed=$(( $(date +%s) - start_time ))
        echo "[$(date +%H:%M:%S)] [data $id] cycle $cycles (${elapsed}s): write+read bs=$BS size=$FILESIZE"

        if [[ $HAS_FIO -eq 1 ]]; then
            fio --name="write_$id" \
                --rw=write --bs="$BS" --size="$FILESIZE" \
                --sync=1 --direct=1 \
                --filename="$datafile" \
                --output=/dev/null 2>/dev/null || true

            fio --name="read_$id" \
                --rw=read --bs="$BS" --size="$FILESIZE" \
                --filename="$datafile" \
                --output=/dev/null 2>/dev/null || true
        else
            dd if=/dev/zero of="$datafile" bs="$BS" count=16384 \
                oflag=sync,dsync 2>/dev/null || true

            dd if="$datafile" of=/dev/null bs="$BS" 2>/dev/null || true
        fi

        rm -f "$datafile" 2>/dev/null || true
        bytes_written=$((bytes_written + 1))

        elapsed=$(( $(date +%s) - start_time ))
        echo "[$(date +%H:%M:%S)] [data $id] cycle $cycles done (${elapsed}s elapsed)"
    done

    elapsed=$(( $(date +%s) - start_time ))
    echo "$cycles" > "$result_file"
    echo "[$(date +%H:%M:%S)] [data $id] finished: $cycles write+read cycles in ${elapsed}s"
}

# ─── Run benchmark ────────────────────────────────────────────────────────

echo ""
echo "=== Running benchmark (${DURATION}s) ==="
echo ""

START_TS=$(date +%s)
DEADLINE=$((START_TS + DURATION + WORKER_GRACE))

kill_stuck_workers() {
    local reason="$1"
    local alive=0
    local stuck=""
    for j in "${!WORKER_PIDS[@]}"; do
        if kill -0 "${WORKER_PIDS[$j]}" 2>/dev/null; then
            alive=$((alive + 1))
            stuck+="${WORKER_LABELS[$j]} "
            kill -9 "${WORKER_PIDS[$j]}" 2>/dev/null || true
        fi
    done
    jobs -p 2>/dev/null | xargs -r kill -9 2>/dev/null || true
    wait_workers_timeout 5 || true
    if [[ $alive -gt 0 ]]; then
        echo ""
        echo "ERROR: $reason"
        echo "  Killed $alive stuck worker(s): ${stuck}"
        BENCH_EXIT=1
    fi
}

for i in $(seq 1 "$MOUNTED"); do
    if [[ $DATA_ONLY -ne 1 ]]; then
        metadata_worker "$i" "$DURATION" &
        WORKER_PIDS+=($!)
        WORKER_LABELS+=("meta-$i")
    fi
    if [[ $META_ONLY -ne 1 ]]; then
        data_worker "$i" "$DURATION" &
        WORKER_PIDS+=($!)
        WORKER_LABELS+=("data-$i")
    fi
done

echo "[$(date +%H:%M:%S)] launched ${#WORKER_PIDS[@]} workers (clients=$MOUNTED, duration=${DURATION}s, grace=${WORKER_GRACE}s)"
echo ""

PROGRESS_INTERVAL=10
while true; do
    now=$(date +%s)
    alive=0
    for pid in "${WORKER_PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            alive=$((alive + 1))
        fi
    done

    elapsed=$((now - START_TS))
    remaining=$((DURATION - elapsed))
    [[ $remaining -lt 0 ]] && remaining=0

    if [[ $alive -eq 0 ]]; then
        echo "[$(date +%H:%M:%S)] all workers finished (${elapsed}s elapsed)"
        break
    fi

    if [[ $now -ge $DEADLINE ]]; then
        kill_stuck_workers \
            "${alive} worker(s) still running after ${DURATION}s (+${WORKER_GRACE}s grace)"
        break
    fi

    echo "[$(date +%H:%M:%S)] progress: ${elapsed}s / ${DURATION}s (${remaining}s left), ${alive}/${#WORKER_PIDS[@]} workers running"
    if mds_line=$(ceph_cmd_timeout 5 mds stat 2>/dev/null); then
        echo "  MDS: $mds_line"
        if [[ "$mds_line" == *"laggy"* || "$mds_line" == *"crashed"* \
              || "$mds_line" == *"damaged"* ]]; then
            echo "  ERROR: MDS is laggy, crashed, or damaged — aborting benchmark"
            BENCH_EXIT=1
            kill_stuck_workers \
                "MDS unhealthy; killed ${alive} worker(s) after ${elapsed}s"
            break
        fi
    else
        echo "  MDS: (status unavailable)"
        echo "  ERROR: MDS unreachable — aborting benchmark"
        BENCH_EXIT=1
        kill_stuck_workers \
            "MDS unreachable; killed ${alive} worker(s) after ${elapsed}s"
        break
    fi

    running=""
    for j in "${!WORKER_PIDS[@]}"; do
        if kill -0 "${WORKER_PIDS[$j]}" 2>/dev/null; then
            running+="${WORKER_LABELS[$j]} "
        fi
    done
    echo "  Active: ${running:-none}"
    echo ""

    sleep_time=$PROGRESS_INTERVAL
    until_deadline=$((DEADLINE - now))
    if [[ $until_deadline -lt $sleep_time ]]; then
        sleep_time=$until_deadline
    fi
    [[ $sleep_time -lt 1 ]] && sleep_time=1
    sleep "$sleep_time"
done

END_TS=$(date +%s)
ELAPSED=$((END_TS - START_TS))

# ─── Collect final metrics ────────────────────────────────────────────────

echo ""
echo "=== Collecting final metrics ==="
if [[ ${BENCH_EXIT:-0} -eq 0 ]]; then
    collect_metrics "after"
else
    echo "  Skipped (benchmark already failed)"
fi

# ─── Report ───────────────────────────────────────────────────────────────

echo ""
echo "============================================"
echo "  Results"
echo "============================================"
echo "Config: clients=$NUM_CLIENTS sync=$SYNC_MODE sharded=$SHARDED duration=${DURATION}s"
echo "Actual elapsed: ${ELAPSED}s"
echo ""

# Aggregate metadata ops
TOTAL_META_OPS=0
if [[ $DATA_ONLY -ne 1 ]]; then
    echo "--- Metadata workers ---"
    for i in $(seq 1 "$MOUNTED"); do
        f="$RESULTS_DIR/meta_$i"
        if [[ -f "$f" ]]; then
            ops=$(cat "$f")
            TOTAL_META_OPS=$((TOTAL_META_OPS + ops))
            rate=$((ops / (ELAPSED > 0 ? ELAPSED : 1)))
            echo "  Client $i: $ops ops  ($rate ops/s)"
        fi
    done
    meta_rate=$((TOTAL_META_OPS / (ELAPSED > 0 ? ELAPSED : 1)))
    echo "  TOTAL:    $TOTAL_META_OPS ops  ($meta_rate ops/s)"
    echo ""
fi

# Aggregate data cycles
TOTAL_DATA_CYCLES=0
if [[ $META_ONLY -ne 1 ]]; then
    echo "--- Data workers ---"
    for i in $(seq 1 "$MOUNTED"); do
        f="$RESULTS_DIR/data_$i"
        if [[ -f "$f" ]]; then
            cyc=$(cat "$f")
            TOTAL_DATA_CYCLES=$((TOTAL_DATA_CYCLES + cyc))
            echo "  Client $i: $cyc write+read cycles"
        fi
    done
    echo "  TOTAL:    $TOTAL_DATA_CYCLES write+read cycles"
    echo ""
fi

echo "--- Summary ---"
if [[ $DATA_ONLY -ne 1 ]]; then
    echo "  Total metadata ops/s: $meta_rate"
fi
echo "  Total data cycles:    $TOTAL_DATA_CYCLES"
echo ""

# Show metric diffs if available
if [[ -f "$METRICS_DIR/before_perf_mds_server.json" && \
      -f "$METRICS_DIR/after_perf_mds_server.json" ]]; then
    echo "--- MDS server perf (before / after) ---"
    echo "  Before:"
    python3 -c "
import json,sys
d=json.load(open('$METRICS_DIR/before_perf_mds_server.json'))
s=d.get('mds_server',{})
for k in ('handle_client_request','req','reply_latency'):
    if k in s: print(f'    {k}: {s[k]}')
" 2>/dev/null || true
    echo "  After:"
    python3 -c "
import json,sys
d=json.load(open('$METRICS_DIR/after_perf_mds_server.json'))
s=d.get('mds_server',{})
for k in ('handle_client_request','req','reply_latency'):
    if k in s: print(f'    {k}: {s[k]}')
" 2>/dev/null || true
fi

echo ""
echo "Metrics saved in: $METRICS_DIR"
echo "============================================"

if [[ ${BENCH_EXIT:-0} -ne 0 ]]; then
    echo ""
    echo "ERROR: benchmark failed (workers stuck and/or MDS unhealthy)"
fi

echo "Done."
exit 0
