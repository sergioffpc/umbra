# Render devices are CPU NUMA nodes and GPUs, chosen per render

umbra renders on devices. A CPU device is one NUMA node: its cores and the
memory attached to them, named `cpu0`, `cpu1`, … by node. A GPU device is one
GPU umbra can use, named `gpu0`, `gpu1`, … by its CUDA ordinal. A machine with
one socket and one GPU has `cpu0` and `gpu0`; a machine with two sockets has
`cpu0` and `cpu1`.

`umbra devices` lists every device with the hardware behind it. It is the
command's result, so it goes to stdout:

```
$ umbra devices
cpu0  AMD Ryzen Threadripper PRO 3975WX
      NUMA node 0: 32 cores, 64 threads, 125 GiB
      L1d 32 KiB, L1i 32 KiB, L2 512 KiB per core, 64 B lines
      L3 16 MiB per 4 cores, 128 MiB in 8 slices
      AVX2, Embree AVX2, OSL batched 8
gpu0  NVIDIA GeForce RTX 5070 Ti (sm_120)
      16 GiB, NUMA node 0
      driver 615.71, CUDA 13.4, OptiX 9.1.0
```

A GPU umbra cannot use (an sm below the minimum, say) is listed without a
name, with the reason. Cores that differ within a node (Intel's P- and
E-cores, an X3D part with V-Cache on one CCD) are listed in groups by their
caches.

`umbra render --devices=cpu0,gpu0` renders on the devices named. Without
`--devices`, a render uses every device: all the CPU NUMA nodes and all the
usable GPUs. When it starts, the render shows the devices it uses on stderr
(docs/adr/0002-cli-diagnostics-on-stderr.md).

A NUMA node is the unit for the CPU because it is the unit of memory
locality: each CPU device renders on a oneTBB arena bound to its node's cores,
from memory allocated on that node, which a whole-CPU device cannot do on a
machine with several sockets. On one node, as on most desktops, the CPU is
`cpu0` and nothing changes.

## Consequences

- Rendering on several devices at once, CPU and GPU together, is the
  default, so the integrator splits work between devices of very different
  speeds from the start.
- Each part reports what it knows about its devices: core reads the CPU
  topology, caches and memory from sysfs; the GPU module describes its
  devices through `ModuleApi`; the Embree and OSL bridges report the ISA and
  the batch width they chose.
- A device name means the same on a given machine from run to run, so it can
  go in scripts. CUDA's ordinals follow `CUDA_VISIBLE_DEVICES` and
  `CUDA_DEVICE_ORDER`, as in every CUDA program.
