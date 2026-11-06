# umbra-math

The numeric foundation of Umbra's rendering core: vectors, points, normals,
frames, transforms, interval arithmetic and floating-point error bounds, the
scalar helpers pbrt-v4 relies on, and the `Vec4`/`Mask4` SIMD facade.

It is the bottom of the core (arc42 §5.1): every other core crate depends on
it, and it depends only on [`libm`](https://crates.io/crates/libm) and
[`wide`](https://crates.io/crates/wide).

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
| `float` (`util/float.h`)                 | `MACHINE_EPSILON`, `ONE_MINUS_EPSILON`, `gamma(n)`, `next_float_up`/`down`, rounded-up/-down `+ − × ÷ √` and FMA, `exponent`/`significand`, `fma`                                                                                                                                                                            |
| `math` (`util/math.h`)                   | `lerp`, `clamp`, `sqr`, `safe_sqrt`/`asin`/`acos`, `evaluate_polynomial`, `difference_of_products`, `sum_of_products`, `two_prod`, `two_sum`, `inner_product`, `CompensatedSum`, `erf_inv`, `fast_exp`, `gaussian`, `gaussian_integral`, `find_interval`, `quadratic`, `log2_int`, `round_up_pow2`, `min`/`max`, π constants |
| `interval` (`util/math.h`)               | `Interval`, a closed `[low, high]` that bounds the rounding error of the computation that produced it                                                                                                                                                                                                                        |
| `vecmath` (`util/vecmath.h`)             | `Vector2/3`, `Point2/3`, `Normal3` (generic, with `f`, `i` and `fi` aliases), `dot`, `cross`, `face_forward`, `angle_between`, `coordinate_system`, `Frame`, `OctahedralVector`, spherical geometry                                                                                                                          |
| `square_matrix` (`util/math.h`)          | `SquareMatrix<N>`, with determinant and inverse for N = 2, 3, 4                                                                                                                                                                                                                                                              |
| `transform` (`util/transform.h`, `.cpp`) | `Transform` (a matrix and its inverse), `translate`, `scale`, `rotate*`, `rotate_from_to`, `look_at`, `perspective`, `orthographic`                                                                                                                                                                                          |
| `simd`                                   | `Vec4`, `Mask4`                                                                                                                                                                                                                                                                                                              |

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
can never allocate, and CI builds it for `aarch64-unknown-none` to prove it.
All types are `Copy`. There is no recursion; pbrt-v4's variadic recursive
helpers (`InnerProduct`, `SumSquares`, `EvaluatePolynomial`) are folds over
const-generic arrays here.

### Bit-identical across architectures

The same input must give the same bits on arm64 (macOS) and x86-64 (Linux)
(ADR 0002). Hence:

- **Transcendentals come from `libm`**, never from the platform's math
  library or `std`'s float methods (which `clippy.toml` bans). `core` has no
  `sqrt`, `floor` or `mul_add` without `std`, so those come from `libm` too;
  with its `arch` feature, `sqrt` and `fma` compile to the hardware
  instruction where there is one.
- **FMA is explicit**: `umbra_math::fma` exactly where pbrt-v4 uses `FMA`, and
  nowhere else. Rust never contracts `a * b + c` on its own.
- **`min` and `max` have C++ `std::min`/`std::max` semantics**: `min(a, b)` is
  `if b < a { b } else { a }`. With a NaN the comparison is false and `a` is
  returned. This is not `f32::min`, which prefers the non-NaN operand; use
  `umbra_math::min`/`max` (and the tuple and `Vec4` methods built on them)
  wherever pbrt-v4 calls `std::min`/`std::max`.
- **No approximate instructions** (`rcpps`, `rsqrtps`, …) and no change of
  the floating-point environment.

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

`Vec4` is four `f32` lanes in one SSE/NEON register, the width of
`SampledSpectrum` (N = 4 wavelengths). It wraps `wide::f32x4`, but pins down
everything `wide` leaves platform-dependent:

- `min`/`max` are a compare plus a select, with `std::min`/`std::max`
  semantics, not `minps`/`fminnm`, whose NaN rules differ. So
  `t0.max(t_near)` ignores a NaN slab bound, as pbrt-v4's ray–box test does.
- `mul_add` is always fused. Without a hardware FMA target feature (CI's
  portable build), `wide` would round twice, so `Vec4` falls back to
  `libm::fmaf` per lane.
- Horizontal reductions (`reduce_add`, `reduce_min`, `reduce_max`) fold left
  to right, like a scalar loop.

The scalar operation is the test oracle: every lane of every operation is
checked bit for bit against it. The one thing outside the contract is the
payload of a NaN created from non-NaN inputs, which differs between CPU
vendors in scalar code too.

## Things to know when using it

- **Angle units follow pbrt-v4**: `rotate`, `rotate_x/y/z` and `perspective`
  take degrees (as scene files do); everything else (`spherical_direction`,
  `angle_between`, `libm` trigonometry) works in radians. Convert with
  `radians`/`degrees`.
- **Division by a scalar divides each component**; it does not multiply by a
  reciprocal, so `v / 3.0` and `v * (1.0 / 3.0)` can differ in the last bit.
- **`Transform::from(matrix)` computes the inverse**; for a singular matrix
  the inverse is all NaN, as in pbrt-v4. Use `Transform::new(m, m_inv)` when
  you already have the inverse.
- **`Transform` equality compares the forward matrix only**, as in pbrt-v4.
- **Prefer `umbra_math::min`/`max` over `f32::min`/`max`** in code that ports
  pbrt-v4, for the NaN reason above.
- **The `serde` feature** (off by default; `umbra-scene` turns it on) derives
  `Serialize`/`Deserialize` for the types stored in Scene arenas: the tuples,
  `Frame`, `OctahedralVector` and `Transform`.

## Performance

Speed was not the goal; reproducibility and robustness were. Several choices
cost cycles on purpose: `difference_of_products` is three operations where
`a*b − c*d` is two, `inner_product` (used by matrix products and the 4 × 4
inverse) is compensated, every `Interval` operation is several float
operations plus ULP steps, `libm` is somewhat slower than glibc, and `fma` is a
software routine on targets built without hardware FMA (build with
`-C target-cpu=x86-64-v3` or native for Workers). `fast_exp` is the one
deliberate approximation, as in pbrt-v4, for the Gaussian filter.

Nothing here has been benchmarked yet. Any faster variant falls under the
aggressive-optimisation rule (ADR 0007): a benchmark, an isolated contract,
and tests against this implementation as the reference.

## Tests

`tests/umbra_math/` is one integration-test crate:

- `pbrt_v4/*_test.rs` port pbrt-v4's unit tests, one module per C++ test file,
  each test naming the `TEST(Suite, Name)` it ports. Tests that pbrt-v4 does
  not have are marked "Not in pbrt-v4's tests".
- `rng.rs` and `sampling.rs` port pbrt-v4's `RNG` (PCG32) and the two
  sampling routines the tests draw from, so the ported tests see the same
  random values as the originals.
- `simd_test.rs` checks `Vec4`/`Mask4` against the scalar reference.

Run them with `cargo nextest run -p umbra-math`. To exercise the
software-FMA fallback as CI does, build without `target-cpu=native`:
`RUSTFLAGS="-D warnings" cargo test -p umbra-math`.
