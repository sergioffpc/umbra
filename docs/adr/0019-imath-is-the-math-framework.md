# Imath is the math framework

umbra computes with points, vectors, matrices, bounds and scalar helpers
on every side: the render delegate places instances and dices surfaces,
and the devices' single-source code generates rays and transforms them.
Over time each place wrote the math it needed: a vector with its dot and
cross products, an affine transform, a clamp, a box, a matrix as sixteen
doubles. Imath, already a dependency of OpenEXR, OpenVDB and OSL, has all
of it. This decision makes Imath umbra's math framework.

## The rule

- umbra's vectors, points, colors, matrices, quaternions, boxes and
  intervals are Imath's types: `V2f`, `V3f`, `V3d`, `Color3f`, `M44f`,
  `M44d`, `Quatd`, `Box3f`, `Box3d` and their kin.
- umbra writes no function or type that Imath already has. Dot and cross
  products, lengths, normalization, interpolation, clamping, transforms of
  points and vectors, inverses and bounds come from Imath (`dot`, `cross`,
  `normalized`, `Imath::lerp`, `Imath::clamp`, `multVecMatrix`,
  `multDirMatrix`, `inverse`, `extendBy`). umbra writes a math function
  only where Imath has none, and builds it on Imath's types.
- OpenUSD's Gf types stay at the Hydra boundary. The render delegate reads
  them from Hydra and converts them to Imath at once; nothing past that
  read computes with Gf, and nothing below the delegate sees it.
- Code that crosses to a device's own layout, such as Embree's or OptiX's
  arrays of floats, converts from Imath's types at that crossing.

## How umbra calls it

Imath is called directly, from any module, without a bridge. It is a
declared exception to the rule that code outside `src/ext/` reaches a
dependency only through its bridge
(`0017-ext-is-a-thin-layer-over-each-dependency.md`). A bridge over Imath
would only pass calls through: its types are plain values, it holds no
handle, and the functions umbra uses neither allocate nor lock, so they fit
the hot path (`0003-no-allocations-on-the-hot-path.md`). Its functions are
marked `IMATH_HOSTDEVICE`, so the devices' single-source code compiles them
for both the CPU and CUDA.

The parts of Imath that throw are not used: `Frustum`, and the variants of
the matrix functions that take a flag asking for an exception (`exc`,
`singExc`). A singular matrix or a degenerate input is checked by umbra's
code, which reports it as `std::expected` does everywhere else.

Where a layout is fixed by a device, umbra's type keeps that layout and
holds Imath's type inside it, such as a point padded to 16 bytes around a
`V3f`.

## Considered options

- **Keep umbra's own math.** It is small, but it grows with every
  algorithm, it is written more than once, and each copy is code to review
  and test that a maintained library already has.
- **OpenUSD's Gf everywhere.** Gf is not marked for CUDA, so the devices'
  single-source code could not use it, and it would tie the devices to
  OpenUSD, which only the render delegate depends on.
- **Gf inside the render delegate, Imath below it.** The delegate would
  compute with two frameworks, and every value would change type once more
  on its way to the devices.
- **A bridge over Imath.** It would add a layer that only passes calls
  through, in hot code on both the CPU's side and the GPU's.
- **Another math library (GLM, Eigen).** It would be a new dependency for
  what Imath, already built, provides, and neither is the type the VFX
  libraries umbra links (OpenEXR, OpenVDB, OSL) exchange.

## Consequences

- One vocabulary of math from the render delegate down to the kernels;
  reading a kernel needs no knowledge of a home-grown vector type.
- Converting from Gf happens once, where Hydra hands a value over.
- Every module that computes links `Imath::Imath`.
- Review checks the rule; no tool does. A new helper is questioned first by
  whether Imath already has it.
- The math umbra wrote before this decision moves onto Imath in its own
  change.
