# Design: networking in Rust

Status: design target.

## Scope

* `net/core/skbuff.c` — packet buffers (the foundation of everything else).
* `net/core/sock.c`, `net/socket.c` — socket lifetime and syscall entry.
* `net/ipv4/*`, `net/ipv6/*` — TCP/UDP and routing.
* `net/netfilter/*` — hooks (later, security-sensitive).
* `drivers/net/*` — driver interfaces, beginning with virtual drivers
  (`virtio-net`, `loopback`, `veth`) which are testable in QEMU.

## Packet representation

`struct sk_buff` is a header/data-split buffer with headroom, tailroom, shared
data (`skb_shared_info`), cloning and fragmented payloads. Modelling it in Rust
requires preserving the exact memory layouts because drivers, DMA and
zero-copy paths depend on them.

Approach:

* Keep `sk_buff` as a C struct.
* Provide `Skb<'a>` handles in Rust: `SkbOwned` (sole reference),
  `SkbShared<'a>` (data may be shared), with operations that map one-to-one onto
  the C helpers (`skb_push`, `skb_pull`, `skb_reserve`, `skb_clone`).
* No Rust type may be placed inside the skb data area.
* Linearisation and fragment iteration are exposed as iterators over
  `&[u8]` slices whose lifetimes are tied to the skb handle.

Once the skbuff core itself migrates, ownership of the head buffer moves into
a Rust `PacketBuffer` type with the same allocation/free conventions
(`kmalloc_reserve`, page frags) to keep the allocator contract identical.

## Order of work

1. `net/core/skbuff.c` pure helpers: headroom arithmetic, `skb_put`/`skb_pull`
   bounds checks, checksum offset helpers. Differential KUnit with the C
   helpers.
2. `loopback` driver and `netdevsim` interactions — validates the driver
   interface without hardware.
3. `net/core/sock.c` skb queues (`sk_receive_queue`) and socket alloc/free.
4. UDP path end-to-end in a QEMU guest with a virtual NIC.
5. TCP state machine — the largest single component; requires timers and
   locking that depend on the scheduler tier.

## Locking and lifetime

* Socket locks (`sk_lock`, BH disable), RCU-protected route/dst entries and
  per-CPU stats are all documented obligations; only the socket lock gets a
  typed wrapper initially.
* `sk_buff` ownership is reference-counted for shared/cloned data; Rust handles
  represent the *skb head* ownership, not the shared data refcount, until the
  skb core itself is migrated.

## Verification

* KUnit differential tests for headroom arithmetic and checksum helpers.
* `tools/testing/selftests/net` in a QEMU guest with a virtio-net pair.
* packetdrill for TCP/UDP behaviour (later; needs a real userspace).
* `iperf3` throughput comparison C vs Rust (QEMU-relative, same host run).

## Risks

* Zero-copy and DMA paths break silently if layout assumptions differ.
* Softirq/NAPI context: no allocation or sleeping in Rust helpers called from
  NAPI.
* Netfilter and BPF hooks are ABI-visible; they stay C until the packet model
  is fully Rust-owned.
