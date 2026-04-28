# vTrustZone OP-TEE/QEMU TZC-400 Core Isolation

This repository is an OP-TEE QEMU virtualization workspace with an experimental
TrustZone Controller (TZC-400) model for core-level isolation. The QEMU `virt`
machine can attach a TZC-400 instance in front of normal DRAM and tag each vCPU
with a static TZC NSAID. TF-A configures the TZC through platform code, OP-TEE
initializes its TZC driver, and a Linux test driver plus userspace program
exercise allowed and denied core access.

See `docs/tzc400-core-isolation.md` for the design notes.

## Key Paths

- `qemu/hw/misc/tzc400.c`: QEMU TZC-400 device model and access checks.
- `qemu/hw/arm/virt.c`: QEMU `virt` machine integration and per-vCPU NSAID
  assignment.
- `trusted-firmware-a/plat/qemu/qemu/`: QEMU platform TZC setup and SMC
  handling.
- `optee_os/core/drivers/tzc400.c`: OP-TEE TZC-400 driver usage.
- `linux/drivers/misc/qemu_tzc400.c`: Linux test-only ioctl bridge.
- `optee_examples/qemu_tzc_core_isolation/`: userspace core-isolation test.
- `build/qemu_v8.mk`: build, run, and regression-test targets.

## Automated Full-Guest Test

Run the booted guest regression with:

```sh
make -C build -f qemu_v8.mk check-tzc400-core-isolation
```

The target forces:

```text
QEMU_TZC400=y
QEMU_SMP=2
QEMU_TZC400_CPU_NSAIDS=0,1
```

It builds the normal OP-TEE QEMU stack, builds `qemu_tzc400.ko`, builds the
`qemu_tzc_core_isolation` host binary, boots QEMU, logs into Buildroot, mounts
the repo over 9p, loads the module, and runs:

```sh
/tmp/tzc400/qemu_tzc_core_isolation 1 0
```

Expected pass condition:

```text
allowed cpu write ok
denied cpu read begins
Internal error: synchronous external abort ...
Segmentation fault
Status: PASS (allowed CPU write, denied CPU synchronous external abort)
```

The segfault is intentional. It proves the denied CPU generated a TZC external
abort when the kernel touched the protected page.

## Interactive Manual Test

First build the stack and test artifacts:

```sh
make -C build -f qemu_v8.mk \
  QEMU_TZC400=y \
  QEMU_SMP=2 \
  QEMU_TZC400_CPU_NSAIDS=0,1 \
  all qemu-tzc400-test-artifacts
```

Then start QEMU directly:

```sh
cd out/bin
../../qemu/build/qemu-system-aarch64 \
  -nographic \
  -smp 2 \
  -cpu max,sme=on,pauth-impdef=on \
  -d unimp \
  -semihosting-config enable=on,target=native \
  -m 1057 \
  -bios bl1.bin \
  -initrd rootfs.cpio.gz \
  -kernel Image \
  -append 'console=ttyAMA0,38400 keep_bootcon root=/dev/vda2' \
  -machine 'virt,acpi=off,secure=on,mte=off,gic-version=3,virtualization=false,tzc400=on,tzc400-cpu-nsaids=0,,1' \
  -object rng-random,filename=/dev/urandom,id=rng0 \
  -device virtio-rng-pci,rng=rng0,max-bytes=1024,period=1000 \
  -netdev user,id=vmnic \
  -device virtio-net-device,netdev=vmnic \
  -fsdev local,id=fsdev0,path=../..,security_model=none \
  -device virtio-9p-device,fsdev=fsdev0,mount_tag=host \
  -serial mon:stdio \
  -serial file:serial1.log
```

Log in as `root`, then run:

```sh
mkdir -p /mnt/host
mount -t 9p -o trans=virtio host /mnt/host

mkdir -p /tmp/tzc400
cp /mnt/host/linux/drivers/misc/qemu_tzc400.ko /tmp/tzc400/
cp /mnt/host/optee_examples/qemu_tzc_core_isolation/host/qemu_tzc_core_isolation /tmp/tzc400/
chmod +x /tmp/tzc400/qemu_tzc_core_isolation

insmod /tmp/tzc400/qemu_tzc400.ko
/tmp/tzc400/qemu_tzc_core_isolation 1 0
```

To quit QEMU from the `-serial mon:stdio` console, press `Ctrl-A`, then `x`.

## What the Test Components Do

`qemu_tzc400.ko` creates `/dev/qemu_tzc400` and exposes three test-only ioctls:

- `ALLOC`: allocate one kernel page and return its physical address.
- `CONFIG`: ask secure firmware to configure one TZC region around that page.
- `TOUCH`: read or write the allocated page from kernel context.

The module does not implement TZC access control. It is only a Linux bridge to
the TF-A SMC service and a controlled way to trigger kernel memory accesses.

`qemu_tzc_core_isolation` is the userspace driver for the test. It opens
`/dev/qemu_tzc400`, allocates one page, configures TZC region 1 so only the
allowed NSAID can read/write it, pins itself to the allowed CPU and writes the
page, then pins itself to the denied CPU and reads the same page.

With `QEMU_TZC400_CPU_NSAIDS=0,1`, CPU0 has NSAID 0 and CPU1 has NSAID 1. The
normal test allows CPU1 and denies CPU0:

```sh
qemu_tzc_core_isolation 1 0
```

## Notes

- QEMU machine-option comma escaping requires doubled commas inside
  `tzc400-cpu-nsaids`, so `0,1` becomes `tzc400-cpu-nsaids=0,,1` in the raw
  QEMU command.
- The standard `run` target uses separate serial terminals and starts QEMU
  paused for debugger attach. The direct QEMU command above is the simplest
  manual path for this TZC test.
- Secure-world serial output is written to `out/bin/serial1.log` in the manual
  command.
