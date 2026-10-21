# Building and running Ferret

## 1. Host requirements

Ferret is developed on an Apple Silicon macOS host but the toolchain runs in
Linux. The only hard requirement on the host is Docker (or any Linux machine
with the tools below installed directly).

| Host element | Value used for checkpoint 001 |
| --- | --- |
| Hardware | Apple M5, 16 GB RAM, arm64 |
| OS | macOS 27.2 (Darwin 27.2.0) |
| Docker | Docker Desktop 29.5.3, aarch64 Linux engine, 10 CPUs, ~7.7 GB RAM |
| Host disk free | ~47 GB at start (kernel tree + build artefacts live in the container) |

The host OS, host kernel and boot configuration are never modified. Kernels are
only ever executed inside QEMU.

### Case-sensitive source tree (required)

The Linux tree contains files whose names differ only in case
(`include/uapi/linux/netfilter/xt_MARK.h` vs `xt_mark.h`, `net/netfilter/
xt_DSCP.c` vs `xt_dscp.c`, ...). On the default case-insensitive macOS APFS
volume, `git checkout` silently merges such pairs and corrupts the tree
(observed during checkout of v6.18.57). The Ferret tree therefore lives on a
case-sensitive APFS sparse bundle mounted at `~/Projects/ferret`:

```sh
tools/ferret/mount-src.sh attach    # create (first time) and mount
tools/ferret/mount-src.sh status
tools/ferret/mount-src.sh detach    # before shutting the host down
```

The bundle is `~/Projects/ferret-src.sparsebundle` (30 GB sparse, ~8.6 GB used
by the tree at checkpoint 001). Docker Desktop exposes the nested mount through
the `~/Projects` share, so the container sees it at `/mnt/projects/ferret`.

### 1.1 Linux-native builds (no Docker)

On any Linux machine (x86_64 or arm64) with the packages listed in
`.github/workflows/kernel-smoke.yml` and the pinned Rust toolchain, run the
in-container scripts directly. They expect two paths:

```sh
sudo mkdir -p /build /mnt/projects
sudo ln -sfn "$PWD" /mnt/projects/ferret      # or bind-mount the checkout
sudo chown -R "$USER" /build

bash tools/ferret/lib/build-in-container.sh rust
bash tools/ferret/lib/build-in-container.sh ref
bash tools/ferret/lib/initramfs-in-container.sh
bash tools/ferret/lib/boot-in-container.sh rust
bash tools/ferret/lib/linkage-in-container.sh rust
```

The Linux filesystem is case-sensitive by default, so the sparsebundle warning
above does not apply. This is the same path GitHub Actions uses; the outer
`tools/ferret/*.sh` wrappers only add the Docker invocation.


## 2. Container image

`tools/ferret/docker/Dockerfile` builds `ferret-buildenv:1` from
`ubuntu:24.04` (aarch64) and pins:

| Tool | Version | Why |
| --- | --- | --- |
| clang / lld / llvm | 18 | kernel minimum is LLVM 15; Ubuntu 24.04 ships 18 |
| rustc | 1.88.0 | kernel minimum is 1.78; 1.88 matches the 6.18 era |
| rust-src | matching 1.88.0 | required for `RUST_LIB_SRC` / `core`+`alloc` |
| bindgen-cli | 0.72.1 | kernel minimum is 0.65.1 |
| QEMU | `qemu-system-x86` + `qemu-system-arm` (Ubuntu 24.04) | TCG boot of the x86_64 and arm64 kernels |
| ccache | 4.9.1 | warm-cache clean rebuilds (DECISIONS D-014) |
| dwarves/pahole, bc, bison, flex, cpio, libelf-dev, libssl-dev | distro | standard kernel build dependencies |

`scripts/min-tool-version.sh` in the tree is the authority for minimums:
`gcc 8.1`, `llvm 15`, `rustc 1.78`, `bindgen 0.65.1`.

Build the image and the container once:

```sh
docker build -t ferret-buildenv:1 tools/ferret/docker
tools/ferret/docker/run.sh true   # creates the persistent container
```

The container mounts `~/Projects` at `/mnt/projects` (source tree and evidence)
and `~/.cache/ferret` at `/mnt/cache`. Kernel build output is kept **inside the
container** at `/build/...` (linux-<variant>, logs, initramfs) to avoid the
virtiofs small-file penalty. Recreating the container discards build output but
not sources or evidence.

## 3. Building

```sh
tools/ferret/build.sh ref            # unmodified upstream 6.18.57 + KUnit + CRC suite
tools/ferret/build.sh rust           # Ferret: Rust CRC-16 + CRC-CCITT
ARCH=arm64 tools/ferret/build.sh rust  # same, for the arm64 lane
```

Each script:

1. mirrors the source tree to `/build/src` inside the container (the host tree
   is a nested virtiofs mount; compiling from it is I/O-bound and roughly 3x
   slower),
2. `make ARCH=x86_64 LLVM=1 O=/build/linux-<variant> defconfig`
3. edits the config with `scripts/config`
4. `make ARCH=x86_64 LLVM=1 olddefconfig`
5. `make ARCH=x86_64 LLVM=1 -j$(nproc) bzImage` (arm64: `Image`)

Only the bootable image is built; test kernels never load modules, so `all`
would waste most of the build on unused drivers. `Module.symvers` is still
produced by the vmlinux link. C compilation is wrapped with `ccache`
(`CCACHE_DIR=/build/ccache`, 20 GiB) so clean rebuilds can hit the cache.

The mirror is an `rsync` of the worktree without `.git` and `artifacts`; after
the first copy it is incremental.

Artifacts: `/build/linux-<variant>/arch/x86_64/boot/bzImage` (or
`/build/linux-<variant>-arm64/arch/arm64/boot/Image`), `vmlinux`,
`Module.symvers`, `.config`; logs in `/build/logs/<variant>/`. After a
successful build the script copies the build log, the effective `.config` and a
completion timestamp into `artifacts/<variant>/`.

`CLEAN=1 tools/ferret/build.sh <variant>` deletes `/build/linux-<variant>`
first, which is what a checkpoint validation run does (DECISIONS D-011).

### Configuration deltas

Both variants add:

```
CONFIG_KUNIT=y
CONFIG_CRC16=y
CONFIG_CRC_CCITT=y
CONFIG_CRC_KUNIT_TEST=y          # upstream CRC suite (independent reference)
```

The Rust variant additionally enables:

```
CONFIG_RUST=y
CONFIG_CRC16_RUST=y              # build lib/crc/crc16_rust.rs, skip crc16.c
CONFIG_CRC16_DIFF_KUNIT_TEST=y   # differential C-vs-Rust KUnit suite
CONFIG_CRC_CCITT_RUST=y          # build lib/crc/crc_ccitt_rust.rs, skip crc-ccitt.c
CONFIG_CRC_CCITT_DIFF_KUNIT_TEST=y
```

`CRC16` and `CRC_CCITT` have no Kconfig prompt; the Rust switches `select`
them so they are forced to `=y` on every architecture (DECISIONS D-015).

`CONFIG_KUNIT_ALL_TESTS` is deliberately **not** enabled: it pulls in unrelated
suites whose failures would obscure the Ferret result.

The reference variant explicitly disables the Ferret symbols.

## 4. Initramfs

```sh
tools/ferret/initramfs.sh
```

Builds `tools/ferret/init` for `x86_64-unknown-none` (freestanding Rust, raw
syscalls, no libc), then packs `/init`, `/dev/console`, `/dev/null`, `/proc`,
`/sys`, `/tmp` into `/build/initramfs.cpio.gz`.

The init checks `uname(2)`, mounts procfs, reads `/proc/version` through the
VFS, prints machine-checkable markers and powers the machine off.

## 5. Booting

```sh
tools/ferret/boot.sh ref
tools/ferret/boot.sh rust
```

QEMU invocation (TCG, no KVM on this host):

```
qemu-system-x86_64 -M pc -cpu max -m 2048 -smp 2 -nographic -no-reboot \
  -kernel /build/linux-<variant>/arch/x86_64/boot/bzImage \
  -initrd /build/initramfs.cpio.gz \
  -append "console=ttyS0 panic=-1 loglevel=7 rdinit=/init"
```

The serial log is written to `artifacts/<variant>/boot.log`. The script checks:

* `Linux version` banner (kernel started),
* `FERRET-INIT: start` and `FERRET-INIT: ok` (reached userspace, ABI sane),
* `FERRET-INIT: sysname=Linux`, `machine=x86_64`,
* `/proc/version` readable,
* `ok N crc16_test` from the upstream CRC KUnit suite,
* no `not ok` KUnit lines, no panic,
* for the Rust variant: the Rust vector suites (`crc16_rust`,
  `crc_ccitt_rust`) and both differential suites (`crc16_diff`,
  `crc_ccitt_diff`).

Exit status is non-zero if any check fails. Timeout is 900 s.

## 6. Linkage evidence

```sh
tools/ferret/linkage.sh ref
tools/ferret/linkage.sh rust
```

Writes `artifacts/<variant>/linkage.txt` and asserts:

* the C objects `lib/crc/crc16.o` and `lib/crc/crc-ccitt.o` exist only in the
  reference build,
* `lib/crc/crc16_rust.o` and `lib/crc/crc_ccitt_rust.o` exist only in the Rust
  build,
* `crc16`, `crc_ccitt` and `crc_ccitt_table` are each defined exactly once in
  `vmlinux`,
* the linked symbols have the same size as in the object that is supposed to
  define them (Rust objects for the Ferret build, C objects for the reference
  build),
* the export shims only reference their symbols (no definitions),
* `__ksymtab_*` entries exist for `crc16`, `crc_ccitt` and `crc_ccitt_table`,
  and `Module.symvers` records `EXPORT_SYMBOL` (non-GPL) for all of them,
* short disassembly fingerprints of both functions are recorded.

`addr2line` provenance is attempted only when the configuration carries DWARF.
`x86_64 defconfig` selects `CONFIG_DEBUG_INFO_NONE`, so on this baseline the
check reports **SKIP** with a reason (DECISIONS D-010); provenance rests on
object ownership and symbol size instead.

## 7. Ubuntu userspace

The usable-system track boots the real Ubuntu 24.04.5 root filesystem
(`noble-server-cloudimg-<arch>-root.tar.xz`, DECISIONS D-012) from a virtio
disk.

```sh
tools/ferret/ubuntu-rootfs.sh amd64     # -> /build/images/ubuntu-amd64.img (4 GiB)
tools/ferret/ubuntu-rootfs.sh arm64

tools/ferret/boot-ubuntu.sh shell   amd64   # init=/sbin/ferret-init
tools/ferret/boot-ubuntu.sh systemd amd64   # systemd as PID 1
tools/ferret/boot-ubuntu.sh shell   arm64
tools/ferret/boot-ubuntu.sh systemd arm64
```

Boot arguments (x86_64; arm64 uses `ttyAMA0` and `qemu-system-aarch64 -M virt`):

```
console=ttyS0 root=/dev/vda rw rootwait rootfstype=ext4 panic=-1 loglevel=7
```

The `shell` mode runs an Ubuntu `/bin/sh` script as PID 1 that reports the
release, kernel, architecture, filesystem type, `sh` and `bash` versions and
procfs, then prints `UBUNTU-CHECK: ok`. The `systemd` mode lets systemd reach
`multi-user.target`, runs a oneshot probe unit (`SYSTEMD-CHECK: ok`,
`systemd --version`, `systemctl is-system-running`, hostname) and starts a
serial autologin getty (`ferret login:`). The harness stops QEMU as soon as the
final marker appears, so a guest that cannot power itself off does not stall the
cycle.

## 8. Full cycle

```sh
tools/ferret/test.sh            # incremental
CLEAN=1 tools/ferret/test.sh    # from-scratch build of both variants
```

Runs build → initramfs → boot → linkage for x86_64, then the same build and
linkage for arm64, then the Ubuntu rootfs and the four Ubuntu boots (shell and
systemd on both architectures), then the registry report. `artifacts/RESULTS.txt`
records PASS / FAIL / NOT RUN per step. A step that depends on a failed
predecessor is recorded as NOT RUN, and the script exits non-zero if anything
did not pass.

## 9. Reproducing from scratch

```sh
git clone https://github.com/overlordxrz-source/ferret.git
cd ferret

# x86_64 reference and Ferret kernels, then a boot and linkage check
tools/ferret/build.sh ref
tools/ferret/build.sh rust
tools/ferret/initramfs.sh
tools/ferret/boot.sh rust
tools/ferret/linkage.sh rust

# full cycle (both architectures, Ubuntu guests, registry)
CLEAN=1 tools/ferret/test.sh
```

The published history starts from a pristine `v6.18.57` snapshot
(`2ff600d29ee478d0a8d46ef8a58afb5e038fe692`), so the baseline is auditable
without cloning upstream's full object database. See
[UPSTREAM.md](UPSTREAM.md) for provenance and synchronization.

Exact versions observed in the development environment are recorded in
`artifacts/env.txt` (generated by `tools/ferret/env.sh`); `artifacts/` is not
published, but the pinned versions are listed in §2.

## 10. Known environment limitations

* x86_64 runs under **QEMU TCG** (no hardware virtualisation on arm64 macOS).
  Boots and KUnit are valid; absolute performance numbers are not.
* The container's Linux is aarch64; the kernel is cross-compiled with
  `LLVM=1`. GNU cross binutils are not installed and are not needed.
* `CONFIG_CRC16_RUST` / `CONFIG_CRC_CCITT_RUST` replace built-in objects only;
  a modular CRC is still the C implementation.
* arm64 runs under QEMU TCG as well; it is a correctness lane, not a
  performance lane.
* The Ubuntu image has no working network yet (virtio-net is configured in the
  kernel, but the guest has no DHCP client configuration); networking is the
  next usable-system milestone.
