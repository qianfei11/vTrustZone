# TZC-400 Core Isolation Notes

This repository models the paper mechanisms as a functional TrustZone access
control path:

- vTZ and kvTZ motivate virtual TrustZone state and virtual security controllers.
- Sanctuary and SafeTEE motivate binding isolated normal-world code to specific cores.
- The accelerator design paper motivates using TZC-400 NSAIDs as bus-master identity.

In QEMU, each ARM CPU receives a static `tzc-nsaid`. TCG memory transactions carry
that ID in `MemTxAttrs.requester_id`. The TZC-400 model checks secure/non-secure
state and requester ID against its region registers before forwarding a memory
transaction to protected DRAM.

The `virt` machine enables the model with:

```text
-machine virt,secure=on,tzc400=on
```

By default CPU `n` uses NSAID `n & 0xf`. A custom mapping can be passed with
`tzc400-cpu-nsaids`; QEMU machine-option comma escaping requires doubled commas
inside the value, for example:

```text
-machine virt,secure=on,tzc400=on,tzc400-cpu-nsaids=0,,1,,2,,3
```

The OP-TEE build wrapper enables the full stack with:

```sh
make -C build -f qemu_v8.mk QEMU_TZC400=y QEMU_SMP=4 all
make -C build -f qemu_v8.mk QEMU_TZC400=y QEMU_SMP=4 run-only
```

With custom CPU-to-NSAID mapping:

```sh
make -C build -f qemu_v8.mk QEMU_TZC400=y QEMU_SMP=4 \
    QEMU_TZC400_CPU_NSAIDS=0,1,2,3 run-only
```

The wrapper escapes the comma-valued QEMU machine property before launching QEMU.

## Configuration Path

1. QEMU maps TZC-400 registers at `0x090c0000` and wraps the `virt` DRAM window
   behind the TZC-400 upstream memory region.
2. TF-A initializes TZC-400 during BL31 setup when `QEMU_TZC400=1`.
3. The Linux test driver sends QEMU platform SiP SMCs to EL3:
   - `0xc200ff00`: configure a TZC region from a physical request structure.
   - `0xc200ff01`: configure region 0 defaults.
   - `0xc200ff02`: enable TZC-400 filters.
4. The Linux driver only accepts region configuration for its allocated test page
   and relaxes that region before returning the page to the kernel allocator.
5. The host example allocates a page, configures it for one allowed NSAID, pins
   worker threads to an allowed and denied CPU, and reports the access result.

Limitations:

- This is a functional access-control model, not a cache or timing model.
- The initial machine integration protects `VIRT_MEM`; QEMU secure-only RAM remains
  protected by the existing secure address-space overlay.
- CPU NSAIDs are static per boot. Changing them at runtime requires flushing all
  affected CPU TLBs before untrusted code runs again.
