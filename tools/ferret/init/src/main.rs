// SPDX-License-Identifier: GPL-2.0-only
//
// Ferret userspace init.
//
// A freestanding x86_64 program that is the first userspace process in the
// Ferret test initramfs.  It exercises a small but meaningful slice of the
// Linux userspace ABI (write, uname, openat/read/close, reboot) and prints
// machine-checkable markers to the serial console so that the QEMU boot test
// can verify that the kernel actually reached userspace and returned correct
// data.
//
// No libc, no runtime: raw syscalls only, built for x86_64-unknown-none.

#![no_std]
#![no_main]
#![allow(clippy::missing_safety_doc)]

use core::arch::asm;
use core::panic::PanicInfo;

const SYS_READ: u64 = 0;
const SYS_WRITE: u64 = 1;
const SYS_CLOSE: u64 = 3;
const SYS_UNAME: u64 = 63;
const SYS_REBOOT: u64 = 169;
const SYS_MOUNT: u64 = 165;
const SYS_OPENAT: u64 = 257;

const AT_FDCWD: i64 = -100;
const O_RDONLY: u64 = 0;

const LINUX_REBOOT_MAGIC1: u64 = 0xfee1dead;
const LINUX_REBOOT_MAGIC2: u64 = 672274793;
const LINUX_REBOOT_CMD_POWER_OFF: u64 = 0x4321fedc;

#[inline(always)]
unsafe fn syscall3(nr: u64, a1: u64, a2: u64, a3: u64) -> i64 {
    let ret: i64;
    // SAFETY: caller guarantees the syscall arguments are valid for `nr`.
    unsafe {
        asm!(
            "syscall",
            inlateout("rax") nr as i64 => ret,
            in("rdi") a1,
            in("rsi") a2,
            in("rdx") a3,
            lateout("rcx") _,
            lateout("r11") _,
            options(nostack)
        );
    }
    ret
}

#[inline(always)]
unsafe fn syscall1(nr: u64, a1: u64) -> i64 {
    let ret: i64;
    // SAFETY: caller guarantees the syscall arguments are valid for `nr`.
    unsafe {
        asm!(
            "syscall",
            inlateout("rax") nr as i64 => ret,
            in("rdi") a1,
            lateout("rcx") _,
            lateout("r11") _,
            options(nostack)
        );
    }
    ret
}

#[inline(always)]
unsafe fn syscall5(nr: u64, a1: u64, a2: u64, a3: u64, a4: u64, a5: u64) -> i64 {
    let ret: i64;
    // SAFETY: caller guarantees the syscall arguments are valid for `nr`.
    // The x86_64 syscall ABI takes the 4th argument in r10 and the 5th in r8.
    unsafe {
        asm!(
            "syscall",
            inlateout("rax") nr as i64 => ret,
            in("rdi") a1,
            in("rsi") a2,
            in("rdx") a3,
            in("r10") a4,
            in("r8") a5,
            lateout("rcx") _,
            lateout("r11") _,
            options(nostack)
        );
    }
    ret
}

#[inline(always)]
unsafe fn write_all(fd: i64, buf: &[u8]) {
    let mut off = 0;
    while off < buf.len() {
        // SAFETY: `buf[off..]` is a valid readable range.
        let n = unsafe {
            syscall3(
                SYS_WRITE,
                fd as u64,
                buf.as_ptr().add(off) as u64,
                (buf.len() - off) as u64,
            )
        };
        if n <= 0 {
            return;
        }
        off += n as usize;
    }
}

fn puts(s: &str) {
    // SAFETY: writing to fd 1 (or 2) with a valid slice.
    unsafe { write_all(1, s.as_bytes()) };
}

/// Print a signed decimal integer without allocating.
fn put_num(v: i64) {
    let mut buf = [0u8; 24];
    let mut i = buf.len();
    let neg = v < 0;
    // Work in i128 to handle i64::MIN without overflow.
    let mut n = (v as i128).unsigned_abs();
    if v == 0 {
        i -= 1;
        buf[i] = b'0';
    }
    while n > 0 {
        i -= 1;
        buf[i] = b'0' + (n % 10) as u8;
        n /= 10;
    }
    if neg {
        i -= 1;
        buf[i] = b'-';
    }
    // SAFETY: `buf[i..]` is a valid, initialised slice.
    unsafe { write_all(1, &buf[i..]) };
}

#[repr(C)]
struct UtsName {
    sysname: [u8; 65],
    nodename: [u8; 65],
    release: [u8; 65],
    version: [u8; 65],
    machine: [u8; 65],
    domainname: [u8; 65],
}

fn cstr(buf: &[u8]) -> &str {
    let end = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
    core::str::from_utf8(&buf[..end]).unwrap_or("<non-utf8>")
}

fn check(cond: bool, what: &str) -> bool {
    if !cond {
        puts("FERRET-INIT: FAIL ");
        puts(what);
        puts("\n");
    }
    cond
}

fn read_small_file(path: &[u8], out: &mut [u8]) -> i64 {
    // SAFETY: `path` is a NUL-terminated byte string, `out` is valid and writable.
    unsafe {
        let fd = syscall3(
            SYS_OPENAT,
            AT_FDCWD as u64,
            path.as_ptr() as u64,
            O_RDONLY,
        );
        if fd < 0 {
            return fd;
        }
        let n = syscall3(SYS_READ, fd as u64, out.as_mut_ptr() as u64, out.len() as u64);
        syscall1(SYS_CLOSE, fd as u64);
        n
    }
}

#[no_mangle]
pub extern "C" fn _start() -> ! {
    let mut ok = true;

    puts("FERRET-INIT: start\n");

    // 1. uname(2): the kernel must return a sane release and machine.
    let mut uts = UtsName {
        sysname: [0; 65],
        nodename: [0; 65],
        release: [0; 65],
        version: [0; 65],
        machine: [0; 65],
        domainname: [0; 65],
    };
    // SAFETY: `uts` is a valid, correctly sized utsname buffer.
    let r = unsafe { syscall1(SYS_UNAME, &mut uts as *mut UtsName as u64) };
    ok &= check(r == 0, "uname");
    if r == 0 {
        puts("FERRET-INIT: sysname=");
        puts(cstr(&uts.sysname));
        puts(" release=");
        puts(cstr(&uts.release));
        puts(" machine=");
        puts(cstr(&uts.machine));
        puts("\n");
        ok &= check(cstr(&uts.sysname) == "Linux", "sysname-is-linux");
        ok &= check(cstr(&uts.machine) == "x86_64", "machine-is-x86_64");
    }

    // 2. mount(2) procfs, then read /proc/version through the VFS: exercises
    //    mount, openat, read and close against the real kernel filesystems.
    // SAFETY: static NUL-terminated strings, no data argument for proc.
    let m = unsafe {
        syscall5(
            SYS_MOUNT,
            b"proc\0".as_ptr() as u64,
            b"/proc\0".as_ptr() as u64,
            b"proc\0".as_ptr() as u64,
            0,
            0,
        )
    };
    ok &= check(m == 0, "mount-proc");
    if m == 0 {
        puts("FERRET-INIT: proc-mounted\n");
    } else {
        puts("FERRET-INIT: mount-proc rc=");
        put_num(m);
        puts("\n");
    }

    let mut buf = [0u8; 256];
    // Static NUL-terminated path, valid output buffer.
    let n = read_small_file(b"/proc/version\0", &mut buf);
    ok &= check(n > 0, "read-proc-version");
    if n > 0 {
        puts("FERRET-INIT: proc-version-prefix=");
        puts(cstr(&buf[..n as usize]));
    }

    if ok {
        puts("FERRET-INIT: ok\n");
    } else {
        puts("FERRET-INIT: failed\n");
    }

    // Power the machine off; with -no-reboot QEMU terminates and the boot test
    // gets a clean exit status.
    // SAFETY: reboot(2) with power-off constants.
    unsafe {
        syscall3(
            SYS_REBOOT,
            LINUX_REBOOT_MAGIC1,
            LINUX_REBOOT_MAGIC2,
            LINUX_REBOOT_CMD_POWER_OFF,
        );
    }
    // SAFETY: exiting the only userspace process.
    unsafe {
        syscall1(60, if ok { 0 } else { 1 });
    }
    loop {
        core::hint::spin_loop();
    }
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    puts("FERRET-INIT: panic\n");
    // SAFETY: exiting the only userspace process.
    unsafe {
        syscall1(60, 2);
    }
    loop {
        core::hint::spin_loop();
    }
}
