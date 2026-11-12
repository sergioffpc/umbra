# umbra-geometry

Where a ray hits: `Ray` and `Bounds`, the interactions a hit produces, the
Shapes behind their closed `ShapeKind` (ADR 0006), and the BVH node layout and
its traversal. An interaction names its Material, area Light and Medium by
`u32` ID (ADR 0008); turning it into a BSDF happens in `umbra-shading`.

It is a core crate: `#![no_std]` without `alloc` (ADR 0014, ADR 0015). It
depends on `umbra-math` and `umbra-sampling`, and `umbra-shading`,
`umbra-camera` and, on the GPU, `umbra-kernels` depend on it (arc42 §5.1,
§5.2). It covers PBR 4e chapters 6–7; so far, only what the GPU tracer bullet
of [#126](https://github.com/sergioffpc/umbra/issues/126) and the camera of
[#77](https://github.com/sergioffpc/umbra/issues/77) need.

```rust
use umbra_geometry::{Ray, Sphere};
use umbra_math::{Point3f, Vector3f, translate};

// A sphere of radius 2 moved to x = 10, and a ray along +x from the origin.
let sphere = Sphere::new(translate(Vector3f::new(10.0, 0.0, 0.0)), 2.0);
let ray = Ray::new(Point3f::new(0.0, 0.0, 0.0), Vector3f::new(1.0, 0.0, 0.0));

let hit = sphere.intersect(&ray, f32::INFINITY).unwrap();
assert!((hit.t_hit - 8.0).abs() < 1e-5);
assert_eq!(hit.p_obj, Point3f::new(-2.0, 0.0, 0.0));

let bounds = sphere.bounds();
assert_eq!(bounds.p_min, Point3f::new(8.0, -2.0, -2.0));
```

## What is in it

| Module (pbrt-v4 source)             | Contents                                                                                                                                              |
| ----------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| `ray` (`ray.h`, `util/transform.h`) | `Ray`: an origin and a direction; the point at a parameter (`at`), and its transformation by a `Transform`, its origin offset past its rounding error |
| `bounds` (`vecmath.h`)              | `Bounds3f`, and its transformation by a `Transform`; `Bounds2f`, with `lerp`                                                                          |
| `handle`                            | `Handle<T>`, the typed `u32` index of an object in its Scene's arena of `T`s (ADR 0008)                                                               |
| `sphere` (`shapes.h`)               | `Sphere`, full spheres only: `Sphere::intersect` (`BasicIntersect`), `Sphere::bounds`, `QuadricIntersection`                                          |

Everything is re-exported at the crate root.

Not here yet: `Ray`'s time and medium, ray differentials, partial spheres (`zmin`, `zmax`,
`phimax`), the surface interaction a hit becomes, the other Shapes and
`ShapeKind`, and the BVH. The Shapes arrive with
[#79 Emissive spheres visible to the camera](https://github.com/sergioffpc/umbra/issues/79).

## Design decisions

### One sphere intersection for the CPU and the GPU

`Sphere::intersect` is a `Copy`-value function of the core (ADR 0028): the
CPU calls it directly, and on the GPU the custom intersection program of
`umbra-kernels` calls the same function from OptiX (ADR 0026). It follows
pbrt-v4's `BasicIntersect` operation for operation, with `Interval`
arithmetic, so no hit behind the ray's origin is reported.

### Arena types have a fixed layout

`Sphere` and `Bounds3f` are stored in Scene arenas and copied to the GPU byte
for byte, so they are `#[repr(C)]` with `size_of` and `offset_of`
assertions that hold on `x86_64` and `nvptx64` (ADR 0028, arc42 §8.12).
`Sphere` holds its `render_from_object` `Transform` by value, 272 bytes with
its radius and tail padding. `Ray` is a per-sample value and keeps
`repr(Rust)`.

### Handles live in the core

Objects in a Scene refer to one another by `Handle<T>`, never by reference
(ADR 0008), and an interaction will name its Material, Light and Medium by
handle. Interactions are this crate's, so `Handle<T>` is too: the lowest
crate that needs it. `umbra-scene`'s `Arena<T>` mints them. `Clone`, `Eq`,
`Ord` and `Hash` are written by hand so that a handle has them whatever `T`
has.

## Things to know when using it

- A `Ray`'s direction need not be normalised: `t_hit` is in units of its
  length.
- `QuadricIntersection::p_obj` is in the sphere's object space, refined onto
  its surface.
- `Ray::transform` moves the origin along the direction by its error bound,
  as pbrt-v4 does, so a transformed ray starts slightly further along. A
  handle means nothing outside the Scene that minted it, and that is not
  checked.

## Tests

`tests/umbra_geometry/sphere.rs` checks `Sphere::intersect` and
`Sphere::bounds` on worked examples: a hit on the near side, a miss, a ray
from inside, a `t_max` that stops short, and a moved and a rotated sphere.
`Ray::transform` is checked through `umbra-camera`'s camera rays, against
pbrt-v4's. The GPU's intersection is checked against `Sphere::intersect` by
`umbra-gpu`'s GPU tests, on the P620 (`cargo nextest run --profile gpu`).
