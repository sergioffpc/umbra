# umbra-math

The numeric foundation of Umbra's rendering core: vectors, points, normals,
frames, transforms, interval arithmetic and floating-point error bounds, the
scalar helpers pbrt-v4 relies on, and the `Vec4`/`Mask4` SIMD facade.

It is a core crate, `#![no_std]` without `alloc` (ADR 0014, ADR 0015), and
the bottom of the core (arc42 §5.1): every other core crate depends on it,
and it depends only on [`libm`](https://crates.io/crates/libm). It covers
PBR 4e chapter 3, §6.8 and §B.2.

```rust
use umbra_math::{Point3f, Point3fi, Vector3f, cross, dot, look_at, perspective};

let camera_from_world = look_at(
    Point3f::new(0.0, 1.0, -5.0),
    Point3f::new(0.0, 0.0, 0.0),
    Vector3f::new(0.0, 1.0, 0.0),
)
.expect("up is not parallel to the viewing direction");
let screen_from_camera = perspective(45.0, 0.01, 1000.0);

let p = camera_from_world.apply(Point3f::new(1.0, 2.0, 3.0));
let n = cross(Vector3f::new(1.0, 0.0, 0.0), Vector3f::new(0.0, 1.0, 0.0));
assert_eq!(dot(n, Vector3f::new(0.0, 0.0, 1.0)), 1.0);

// A point with conservative error bounds, as shape intersections return it.
let pi = camera_from_world.apply(Point3fi::from(Point3f::new(1.0, 2.0, 3.0)));
assert!(pi.x.contains(p.x));

// Projective transforms divide by w.
let ndc = screen_from_camera.apply(p);
assert!(ndc.z > 0.0 && ndc.z < 1.0);
```

## What is in it

| Module (pbrt-v4 source)                  | Contents                                                                                                                                                                                                                                                                                                                     |
| ---------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `float` (`util/float.h`)                 | `MACHINE_EPSILON`, `ONE_MINUS_EPSILON`, `gamma(n)`, `next_float_up`/`down`, rounded-up/-down `+ − × ÷ √` and FMA, `exponent`/`significand`, `fma`, `sqrt`, `floor`, `ceil`                                                                                                                                                   |
| `math` (`util/math.h`)                   | `lerp`, `clamp`, `sqr`, `safe_sqrt`/`asin`/`acos`, `evaluate_polynomial`, `difference_of_products`, `sum_of_products`, `two_prod`, `two_sum`, `inner_product`, `CompensatedSum`, `erf_inv`, `fast_exp`, `gaussian`, `gaussian_integral`, `find_interval`, `quadratic`, `log2_int`, `round_up_pow2`, `min`/`max`, π constants |
| `transcendental`                         | `sin_cos`, `atan2`, `erf`: Umbra's own scalar transcendentals, within 1 ULP and faster than `libm`'s                                                                                                                                                                                                                         |
| `interval` (`util/math.h`)               | `Interval`, a closed `[low, high]` that bounds the rounding error of the computation that produced it                                                                                                                                                                                                                        |
| `vecmath` (`util/vecmath.h`)             | `Vector2/3`, `Point2/3`, `Normal3` (generic, with `f`, `i` and `fi` aliases), `dot`, `cross`, `face_forward`, `angle_between`, `coordinate_system`, `Frame`, `OctahedralVector`, spherical geometry                                                                                                                          |
| `square_matrix` (`util/math.h`)          | `SquareMatrix<N>`, with determinant and inverse for N = 2, 3, 4                                                                                                                                                                                                                                                              |
| `transform` (`util/transform.h`, `.cpp`) | `Transform` (a matrix and its inverse), `translate`, `scale`, `rotate*`, `rotate_from_to`, `look_at`, `perspective`, `orthographic`                                                                                                                                                                                          |
| `simd`                                   | `Vec4`, `Mask4`: four `f32` lanes over `core::arch` (SSE4.1, AVX2 and FMA on x86-64), with `exp` and `pow` within 1 ULP                                                                                                                                                                                                      |

Everything is re-exported at the crate root, so `use umbra_math::{…}` is all
you need.

Not here, on purpose: `Bounds2`/`Bounds3`, `Ray` and `DirectionCone` live in
`umbra-geometry`, and transforming a ray or a bounding box belongs with those
types. The RNG, hashing and sampling routines live in `umbra-sampling`.

## Design decisions

### pbrt-v4, ported faithfully

The code follows pbrt-v4 (and _Physically Based Rendering_, 4th edition, cited
as "PBR 4e §x.y" in the doc comments). Names are pbrt's in snake case
(`DifferenceOfProducts` → `difference_of_products`, `Point3fi`, `Frame::from_z`),
so the book and the C++ source remain the documentation.

"Faithfully" includes operation order and FMA placement, since both decide
the last bit. A few examples that look odd on their own but match pbrt-v4:

- `dot(Vector3, Vector3)` is a plain `x·x' + y·y' + z·z'`, while every dot
  product with a `Normal3` uses `fma(n.x, v.x, sum_of_products(…))`.
- `dot(Vector2, Vector2)` uses `sum_of_products`.
- Transforming a `Point3f` sums each matrix row left to right; the inverse
  sums it in pairs.
- `lerp` is `(1 − x)·a + x·b`, without FMA.

Where Umbra departs from pbrt-v4, the doc comment says so:

- `Transform::apply_inverse(Point3fi)` includes the translation column in the
  error bound of an exact point, which pbrt-v4 leaves out. The bound is the
  conservative one, and forward and inverse share one rule.
- `SquareMatrix<2>::inverse` uses the adjugate instead of pbrt-v4's generic
  Gauss–Jordan elimination.
- `look_at` returns `None` for an `up` parallel to the viewing direction,
  where pbrt-v4 stops with a fatal error.

### `f32` only, `no_std`, no `alloc`

The core computes in `f32` (ADR 0001); there is no `Float` alias and no `f64`
build. The crate is `#![no_std]` without `alloc` (ADR 0014, ADR 0015), so it
can never allocate.
All types are `Copy`. There is no recursion; pbrt-v4's variadic recursive
helpers (`InnerProduct`, `SumSquares`, `EvaluatePolynomial`) are folds over
const-generic arrays here.

### Deterministic on one machine, x86-64 only

A computation gives the same bits every time it runs on one machine with one
binary; across machines, results only have to be statistically equal
(ADR 0022). The crate supports x86-64 only (ADR 0023). Hence:

- **Transcendentals come from `libm` or from Umbra's own kernels**, never
  from the platform's math library or `std`'s float methods (which
  `clippy.toml` bans). Umbra's own replace `libm` where they measured faster:
  `sin_cos`, `atan2` and `erf`, and `Vec4::exp` and `Vec4::pow`. Each states
  its error bound, 1 ULP, and is tested against `libm`'s `f64` routines
  (ADR 0022).
- **`x86-64-v3` is required** (FMA, SSE4.1, AVX2), which every target
  machine has: without it the crate stops the build with a `compile_error!`,
  so there is no software-FMA path. Build with `target-cpu=native` (the
  local default) or `-C target-cpu=x86-64-v3`. Any other architecture stops
  the build too; a GPU port adds its own backend (ADR 0024).
- **`fma`, `sqrt`, `floor` and `ceil` are the hardware instruction**. `fma`
  and `sqrt` are correctly rounded and `floor`/`ceil` are exact, so they
  compute the bits of `libm`; the hardware forms are inlined, where `libm`'s
  `fmaf` is an indirect call through a runtime CPU check and its `sqrtf`
  opaque inline assembly.
- **FMA is explicit**: `umbra_math::fma` wherever pbrt-v4 uses `FMA`, and
  elsewhere where it is faster or more accurate, except in code whose error
  bounds assume pbrt-v4's exact operations. Rust never contracts `a * b + c`
  on its own.
- **`min` and `max` have C++ `std::min`/`std::max` semantics**: `min(a, b)` is
  `if b < a { b } else { a }`. With a NaN the comparison is false and `a` is
  returned. This is not `f32::min`, which prefers the non-NaN operand; use
  `umbra_math::min`/`max` (and the tuple and `Vec4` methods built on them)
  wherever pbrt-v4 calls `std::min`/`std::max`.
- **No approximate instructions** (`rcpps`, `rsqrtps`, …), whose bits differ
  between CPU vendors, and no change of the floating-point environment.

### Generic tuples, one `Real` trait

`Vector3<T>`, `Point3<T>` and `Normal3<T>` are distinct generic types, so a
point never silently becomes a vector: `Point − Point` is a `Vector`,
`Point + Vector` is a `Point`, normals transform by the inverse transpose.
The aliases are pbrt's: `Vector3f`, `Vector3i`, `Point3f`, `Point2i`,
`Normal3f`, and the error-bounded `Point3fi = Point3<Interval>` and
`Vector3fi = Vector3<Interval>`.

Lengths, normalisation, `dot` and `cross` are written once over the `Real`
trait, which both `f32` and `Interval` implement. That is how
`distance(p, q)` works for `Point3f` and returns an `Interval` for `Point3fi`.

pbrt-v4's C++ overloads (`Dot(Vector3, Normal3)`, `Cross(Normal3, Vector3)`,
…) become traits (`Dot<Rhs>`, `Cross<Rhs>`, `FaceForward<Rhs>`) with free
functions in front of them, so calls read as in pbrt: `dot(n, v)`,
`cross(a, b)`, `face_forward(n, wo)`. The same goes for
`Transform::apply(x)`, which dispatches through `Transformable` to the right
rule for points, vectors, normals, `Point3fi` and `Vector3fi`.

### Interval arithmetic for robust intersections

`Interval` and `Point3fi` implement pbrt-v4's conservative floating-point
error bounds (PBR 4e §6.8): each operation rounds its bounds outward by one
ULP, and transforms propagate the error with `gamma(3)`. Shapes return
intersection points as `Point3fi`, and ray origins are offset by that error,
so rays do not re-hit the surface they leave, at any scene scale and without
epsilons.

### `Vec4`/`Mask4`: SIMD that computes what scalar code computes

`Vec4` is four `f32` lanes in one SSE register, the width of
`SampledSpectrum` (N = 4 wavelengths). The lanes are stored as a
16-byte-aligned `[f32; 4]`, so `new`, `splat` and `ZERO` are `const`; each
operation moves them through one `core::arch` intrinsic, chosen so that every
lane computes what the scalar operation computes:

- `min`/`max` have `std::min`/`std::max` semantics: `minps`/`maxps` with the
  operands swapped (`minps(b, a)` returns `b` when `b < a` and `a`
  otherwise, so `a` for a NaN in either). So `t0.max(t_near)`
  ignores a NaN slab bound, as pbrt-v4's ray–box test does.
- `mul_add` is always fused (`vfmaddps`), never a multiply and an add.
- Horizontal reductions (`reduce_add`, `reduce_min`, `reduce_max`) fold left
  to right, like a scalar loop.

`exp` and `pow` are the exception: no IEEE operation computes them, so they
are within 1 ULP of the correctly rounded result rather than equal to a
scalar routine (below).

There is one backend under the facade, `x86_64`: SSE with `blendvps` and
`vfmaddps`, and AVX2 `f64` kernels for `exp` and `pow`.
The scalar operation is the oracle of the facade
(`tests/umbra_math/simd_test.rs`), lane by lane, over random bit patterns and
the special values. The one thing outside the
contract is the payload of a NaN created from non-NaN inputs, which differs
between CPU vendors in scalar code too.

### Aggressive optimisations, each with its reference (ADR 0007)

The crate's hot operations run as `Vec4` kernels, written so that each lane
performs the operations of pbrt-v4's scalar code on the same values in the
same order; the only reordering is across lanes, which are independent, so
every result is the same bits:

- **`Interval` arithmetic** (`src/interval/kernels.rs`): the four bound
  products of `×`, `÷` and `fma` in one lane-wise operation, their outward
  rounding (pbrt-v4's `NextFloatUp`/`NextFloatDown`, as three selects and
  one integer add per lane) in one pass, and the extremes by two
  deinterleaves and `std::min`/`std::max` selects. `+`, `−`, scalar `×`
  and `÷`, `sqrt`, `sqr`, `from_value_and_error` and
  `difference_of_products` round both bounds at once. No branch depends on
  the values.
- **`SquareMatrix` products and the 4 × 4 inverse**
  (`src/square_matrix/kernels.rs`): one output row at a time with its columns
  in the lanes, so the compensated `InnerProduct` (`TwoProd`, `TwoSum`) of
  every element of the row is one lane-wise chain; the adjugate reads the
  matrix down its columns with the row order `1, 0, 3, 2` and alternating
  signs, as pbrt-v4's formulas do element by element.
- **`Transform` application** (`src/transform/kernels.rs`): the four rows of
  the matrix in the lanes, so one chain of multiplies and adds transforms a
  point or a vector, and the `gamma(3)` error bound of a `Point3fi` comes out
  of the same lanes. The lanes are the columns of the matrix, which a
  `Transform` stores next to its two matrices when it is built: transposing
  them on every call cost as much as the arithmetic. A `Transform` is
  therefore 256 bytes instead of 128.
- **Scalar primitives**: `fma`, `sqrt`, `floor` and `ceil` as the hardware
  instruction (above), and `round_up_pow2` by a count of leading zeros
  instead of pbrt-v4's five shift-or steps.

The transcendentals cannot keep the bits of `libm`, so they keep an error
bound instead (ADR 0022): every result is within 1 ULP of the correctly
rounded one, with C's values at signed zeros, infinities and NaNs.

- **`Vec4::exp` and `Vec4::pow`** (`src/simd/x86_64/transcendental.rs`), for
  `SampledSpectrum::exp` and `pow`: the four lanes widened to `f64` in one
  AVX register, which makes every `f32` subnormal normal, then
  `2^(x log₂ e)` and `2^(e log₂ x)` from Taylor polynomials and exponent-bit
  arithmetic, and one rounding back to `f32`. The facade
  (`src/simd/transcendental.rs`) gives `pow` C's special values.
- **`sin_cos`, `atan2` and `erf`** (`src/transcendental.rs`): scalar `f64`
  code with the `f64` FMA. `sin_cos` shares one argument reduction (π/2 in
  two parts, beyond ±2²⁰ `libm`'s), `atan2` reduces to `[0, tan(π/8)]` with
  one division, and `erf` reads one of eight half-unit segments. Their
  polynomials are interpolants of `libm`'s `f64` routines at Chebyshev
  nodes, of the lowest degree whose error is below 2⁻³⁰. The quadrant,
  octant and segment are picked by selects and bit operations, not branches.

The `unsafe` is confined to the backend of `Vec4` and to
`src/float/arch.rs`: one intrinsic per `unsafe` block, each with its
`// SAFETY:` comment, under `#[expect(unsafe_code)]` on that function. The
kernels and the facade are safe code.

Every optimised operation keeps its reference in
`tests/umbra_math/reference/`: pbrt-v4's code in plain scalar Rust over
`libm`, working on the public types through their public interface, so it
shares nothing with the kernels. It lives with the tests, not in `src/`
(nothing in `src/` exists only for tests), and `benches/kernels.rs` includes
it by `#[path]`. `tests/umbra_math/equivalence/` checks each kernel against its
reference bit for bit over random bit patterns, the special values and values
of renderer magnitude; `benches/kernels.rs` times the two side by side.

What was tried and did not win stays in its reference form: a branch-free
`next_float_up`/`next_float_down` (the branch on infinity is perfectly
predicted; the scalar form is 20 % faster). A `Vec4` transform of a
`Vector3f` lost while it transposed the matrix, and wins now that the
columns are stored. A `sin_cos` that picked its quadrant by a `match` was
slower than `libm`'s `sinf` and `cosf` together on random angles, and Taylor
polynomials for `sin`, `cos` and `atan` cost a third more than the
Chebyshev ones.

## Things to know when using it

- **Angle units follow pbrt-v4**: `rotate`, `rotate_x/y/z` and `perspective`
  take degrees (as scene files do); everything else (`spherical_direction`,
  `angle_between`, `sin_cos`, `atan2`, `libm` trigonometry) works in radians. Convert with
  `radians`/`degrees`.
- **Division by a scalar divides each component**; it does not multiply by a
  reciprocal, so `v / 3.0` and `v * (1.0 / 3.0)` can differ in the last bit.
- **`Transform::from(matrix)` computes the inverse**; for a singular matrix
  the inverse is all NaN, as in pbrt-v4. Use `Transform::new(m, m_inv)` when
  you already have the inverse.
- **`Transform` equality compares the forward matrix only**, as in pbrt-v4.
- **Prefer `umbra_math::min`/`max` over `f32::min`/`max`** in code that ports
  pbrt-v4, for the NaN reason above.
- **Use `umbra_math::sqrt`, `floor`, `ceil` and `fma`** rather than `libm`'s
  directly: the same bits, as one inlined instruction.
- **Use `umbra_math::sin_cos`, `atan2` and `erf`** rather than `libm`'s
  `sinf`/`cosf`, `atan2f` and `erff`, and `Vec4::exp`/`pow` rather than
  `libm` per lane: within 1 ULP, but not the same bits.
- **An `Interval` bound must not be NaN.** The outward rounding steps the bits
  of its operand, as pbrt-v4's `NextFloatUp`/`NextFloatDown` do; for a NaN
  that depends on its payload (a NaN whose payload is all ones steps to a
  zero), and a NaN's payload is not stable across compilations. pbrt-v4 has
  the same property. A NaN bound is garbage in; the kernels and the reference
  agree on every finite and infinite input, and on NaNs produced from them.
- **The `serde` feature** (off by default; `umbra-scene` turns it on) derives
  `Serialize`/`Deserialize` for the types stored in Scene arenas: the tuples,
  `Frame`, `OctahedralVector` and `Transform`.

## Performance

Several choices cost cycles on purpose, for reproducibility and robustness:
`difference_of_products` is three operations where `a*b − c*d` is two,
`inner_product` (matrix products and the 4 × 4 inverse) is compensated, every
`Interval` operation is several float operations plus ULP steps, and the
`libm` routines Umbra keeps are somewhat slower than glibc's. `fast_exp` is
pbrt-v4's coarse approximation for the Gaussian filter, kept as it is.

Within those choices, the kernels above take back most of the cost.
`cargo bench -p umbra-math` runs `benches/kernels.rs` (`divan`): each bench
runs a kernel over a batch of 1024 inputs of renderer magnitude, once with the
reference and once with the optimised implementation. Medians on an AMD
Ryzen Threadripper PRO 3975WX (Linux x86-64, `target-cpu=native`, Rust
1.99.1), in nanoseconds per call:

| Kernel                                 | Reference | Optimised | Speed-up |
| -------------------------------------- | --------: | --------: | -------: |
| `fma`                                  |      1.52 |     0.463 |    3.3 × |
| `difference_of_products`               |      2.81 |     0.526 |    5.3 × |
| `sqrt`                                 |      1.67 |      1.67 |    1.0 × |
| `round_up_pow2`                        |      1.48 |     0.817 |    1.8 × |
| `Interval + Interval`                  |      3.73 |      2.04 |    1.8 × |
| `Interval − Interval`                  |      3.75 |      2.28 |    1.6 × |
| `Interval × Interval`                  |      14.3 |      4.69 |    3.0 × |
| `Interval ÷ Interval`                  |        15 |      6.22 |    2.4 × |
| `Interval::fma`                        |      19.2 |      5.29 |    3.6 × |
| `Interval::sqr`                        |      4.19 |      3.35 |    1.2 × |
| `Interval::from_value_and_error`       |      4.02 |      1.79 |    2.2 × |
| `difference_of_products` on `Interval` |      23.4 |      15.3 |    1.5 × |
| `SquareMatrix<3> × SquareMatrix<3>`    |      49.2 |      11.8 |    4.2 × |
| `SquareMatrix<4> × SquareMatrix<4>`    |       112 |      18.9 |    5.9 × |
| `SquareMatrix<4>::inverse`             |       165 |      42.9 |    3.9 × |
| `Transform::apply(Point3f)`            |      4.08 |      1.99 |    2.0 × |
| `Transform::apply_inverse(Point3f)`    |      3.83 |      1.44 |    2.7 × |
| `Transform::apply(Vector3f)`           |      1.71 |      1.31 |    1.3 × |
| `Transform::apply(Normal3f)`           |      1.95 |      1.67 |    1.2 × |
| `Transform::apply(Point3fi)`           |      25.4 |      14.5 |    1.8 × |
| `Transform::apply(Vector3fi)`          |      16.1 |      9.98 |    1.6 × |
| `Vec4::exp` (`libm::expf` per lane)    |        26 |      4.08 |    6.4 × |
| `Vec4::pow` (`libm::powf` per lane)    |       204 |      18.2 |   11.2 × |
| `sin_cos` (`libm::sinf` and `cosf`)    |      8.79 |      6.51 |    1.3 × |
| `atan2` (`libm::atan2f`)               |      11.4 |      4.42 |    2.6 × |
| `erf` (`libm::erff`)                   |      14.7 |      5.74 |    2.6 × |

`sqrt` is the same instruction either way; the gain is that LLVM sees
through the intrinsic and can fold, schedule and vectorise it, which it
cannot do with `libm`'s inline assembly. The `Transform` benches use a
different transform for each of the 1024 inputs, so they also time the
cache traffic of the 256-byte `Transform`. That is why `apply(Normal3f)`,
which reads the rows as before, measured 1.17 ns before the columns were
stored and 1.67 ns after; with 16 transforms that stay in cache it measured
1.42 ns both times, while `apply(Point3f)` went from 2.41 to 1.61 ns. Micro-benchmarks locate a gain but
do not justify it on their own (arc42 §8.4): the end-to-end proof is
`umbra bench` on the Reference Scenes, once there is a renderer to run it.

## Tests

`tests/umbra_math/` is one integration-test crate:

- `pbrt_v4/*_test.rs` port pbrt-v4's unit tests, one module per C++ test file,
  each test naming the `TEST(Suite, Name)` it ports. Tests that pbrt-v4 does
  not have are marked "Not in pbrt-v4's tests".
- `rng.rs` and `sampling.rs` port pbrt-v4's `RNG` (PCG32) and the two
  sampling routines the tests draw from, so the ported tests see the same
  random values as the originals.
- `simd_test.rs` checks `Vec4`/`Mask4` against the scalar reference.
- `reference/*.rs` are the safe, book-faithful references of the optimised
  kernels, and `equivalence/*.rs` checks every kernel against its reference
  bit for bit (ADR 0007).
- `accuracy/*.rs` checks Umbra's own transcendentals against `libm`'s `f64`
  routines: within 1 ULP, with the reference's sign and NaNs (ADR 0022). The
  unary ones (`sin_cos`, `erf`, `Vec4::exp`) are swept over every 251st
  `f32` on every run; `atan2` and `Vec4::pow` over every 251st value of one
  argument, random pairs and every pair of special values.
- The exhaustive sweeps of every `u32` (two in `equivalence`, three in
  `accuracy`) are `#[ignore]`d; run them with
  `cargo nextest run -p umbra-math --run-ignored all -E 'test(exhaustive)'`
  (about 35 s on the 3975WX).

Run them with `cargo nextest run -p umbra-math`. CI's x86-64 jobs build with
`RUSTFLAGS="-D warnings -C target-cpu=x86-64-v3"`, the baseline rather than
the local CPU. The `unsafe` backends run under Miri in CI (ADR 0007), with
`libm` in software (Miri cannot run its inline assembly) and without the
pbrt-v4 ports (too slow interpreted). Miri runs on the pinned toolchain once
`rustup component add miri rust-src` has added it:

```sh
RUSTFLAGS="-C target-cpu=x86-64-v3" cargo miri nextest run -p umbra-math \
  --all-features --features libm/force-soft-floats -E 'not test(/^pbrt_v4::/)'
```
