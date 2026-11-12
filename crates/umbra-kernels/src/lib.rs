#![doc = include_str!("../README.md")]
// The whole crate is device code (ADR 0028): on any other target it is empty,
// so a host build of the workspace compiles it without the GPU toolchain.
#![cfg(target_arch = "nvptx64")]
#![no_std]
// `ptx-kernel` entry points and inline PTX: the only unstable features of
// the workspace (ADR 0027).
#![feature(abi_ptx, asm_experimental_arch)]
// No `f64` on the GPU (ADR 0028): `clippy.toml` lists it.
#![warn(clippy::disallowed_types)]

mod launch;
#[cfg(feature = "math-program")]
mod math;
#[cfg(feature = "math-program")]
mod math_launch;
mod optix;

use core::mem::offset_of;
use core::panic::PanicInfo;

use umbra_geometry::{Ray, Sphere};
use umbra_math::{Point3f, Vector3f};

use crate::launch::{HitKind, HitRecord, LaunchParams, RayRecord};
use crate::optix::{Primitive, TraceRay};

/// The launch parameters: the `.const` variable the host's pipeline names
/// `params`, as large as `LaunchParams`. A module of its own, the narrowest
/// item an `expect` can sit on.
#[expect(
    unsafe_code,
    reason = "OptiX's launch parameters are a `.const` variable (ADR 0007, ADR 0028)"
)]
mod params {
    core::arch::global_asm!(
        ".visible .const .align 8 .b8 params[{size}];",
        size = const size_of::<crate::launch::LaunchParams>(),
    );
}

/// Payload register 0: the bits of the closest hit's ray parameter.
const PAYLOAD_T: u32 = 0;
/// Payload register 1: the `HitKind` of the closest hit.
const PAYLOAD_KIND: u32 = 1;

/// A panic is a bug (ADR 0014): it aborts the launch, which the host reports
/// as a CUDA error.
#[panic_handler]
fn panic(_: &PanicInfo<'_>) -> ! {
    optix::trap()
}

/// Ray generation: traces ray `i` of the launch parameters, for the launch
/// index `i`, and writes what it hit to hit record `i`.
#[expect(
    unsafe_code,
    reason = "an OptiX entry point keeps its name in the PTX (ADR 0007, ADR 0028)"
)]
#[unsafe(no_mangle)]
pub extern "ptx-kernel" fn __raygen__trace() {
    let params = launch_params();
    let i = optix::launch_index_x();
    if i >= params.count {
        return;
    }
    let ray = read_ray(params.rays, i);
    let trace_ray = TraceRay {
        origin: ray.o,
        direction: ray.d,
        tmin: 0.0,
        // `optixTrace` rejects a ray whose `tmax` is not finite.
        tmax: f32::MAX,
    };
    let [t, kind] = optix::trace(params.handle, trace_ray, [0, HitKind::Miss as u32]);
    write_hit(
        params.hits,
        i,
        HitRecord {
            t: f32::from_bits(t),
            kind,
        },
    );
}

/// Ray generation for a math launch: evaluates the launch's function on
/// argument record `i`, for the launch index `i`, and writes result `i`.
/// Only with the `math-program` feature, for the GPU tests (#146).
#[cfg(feature = "math-program")]
#[expect(
    unsafe_code,
    reason = "an OptiX entry point keeps its name in the PTX (ADR 0007, ADR 0028)"
)]
#[unsafe(no_mangle)]
pub extern "ptx-kernel" fn __raygen__math() {
    let params = math::math_params();
    let i = optix::launch_index_x();
    if i >= params.count {
        return;
    }
    let Ok(function) = math_launch::MathFunction::try_from(params.function) else {
        optix::trap()
    };
    let result = function.evaluate(math::read_args(params.args, i));
    math::write_result(params.results, i, result);
}

/// Miss: the ray hit nothing.
#[expect(
    unsafe_code,
    reason = "an OptiX entry point keeps its name in the PTX (ADR 0007, ADR 0028)"
)]
#[unsafe(no_mangle)]
pub extern "ptx-kernel" fn __miss__trace() {
    optix::set_payload(PAYLOAD_KIND, HitKind::Miss as u32);
}

/// Closest hit, for triangles and spheres alike: the hit's ray parameter
/// and which of the two it was.
#[expect(
    unsafe_code,
    reason = "an OptiX entry point keeps its name in the PTX (ADR 0007, ADR 0028)"
)]
#[unsafe(no_mangle)]
pub extern "ptx-kernel" fn __closesthit__trace() {
    let kind = match optix::hit_primitive() {
        Primitive::Triangle => HitKind::Triangle,
        Primitive::Custom => HitKind::Sphere,
    };
    optix::set_payload(PAYLOAD_T, optix::ray_tmax().to_bits());
    optix::set_payload(PAYLOAD_KIND, kind as u32);
}

/// Intersection of a custom primitive with the [`Sphere`] in its hit group
/// record: the core's own `Sphere::intersect`, as on the CPU (ADR 0026).
#[expect(
    unsafe_code,
    reason = "an OptiX entry point keeps its name in the PTX (ADR 0007, ADR 0028)"
)]
#[unsafe(no_mangle)]
pub extern "ptx-kernel" fn __intersection__sphere() {
    let sphere = read_sphere(optix::sbt_data_pointer());
    let [ox, oy, oz] = optix::object_ray_origin();
    let [dx, dy, dz] = optix::object_ray_direction();
    let ray = Ray::new(Point3f::new(ox, oy, oz), Vector3f::new(dx, dy, dz));
    if let Some(hit) = sphere.intersect(&ray, optix::ray_tmax()) {
        optix::report_intersection(hit.t_hit);
    }
}

/// The launch parameters, field by field from the `.const` variable.
fn launch_params() -> LaunchParams {
    LaunchParams {
        handle: optix::launch_param_u64::<{ offset_of!(LaunchParams, handle) }>(),
        rays: optix::launch_param_u64::<{ offset_of!(LaunchParams, rays) }>(),
        hits: optix::launch_param_u64::<{ offset_of!(LaunchParams, hits) }>(),
        count: optix::launch_param_u32::<{ offset_of!(LaunchParams, count) }>(),
    }
}

/// Ray record `i` of the array at device address `rays`.
#[expect(
    unsafe_code,
    reason = "reads a buffer the host wrote, by its device address (ADR 0007, ADR 0028)"
)]
fn read_ray(rays: u64, i: u32) -> RayRecord {
    let record = (rays as *const RayRecord).wrapping_add(i as usize);
    // SAFETY: the host points `rays` at `count` records, aligned and
    // initialised, and the caller checked `i < count`.
    unsafe { record.read() }
}

/// Writes hit record `i` of the array at device address `hits`.
#[expect(
    unsafe_code,
    reason = "writes a buffer the host reads, by its device address (ADR 0007, ADR 0028)"
)]
fn write_hit(hits: u64, i: u32, hit: HitRecord) {
    let record = (hits as *mut HitRecord).wrapping_add(i as usize);
    // SAFETY: the host points `hits` at `count` aligned records that only
    // thread `i` writes, and the caller checked `i < count`.
    unsafe { record.write(hit) }
}

/// The `Sphere` at device address `data`, the data of a hit group record.
#[expect(
    unsafe_code,
    reason = "reads the shader binding table the host wrote (ADR 0007, ADR 0028)"
)]
fn read_sphere(data: u64) -> Sphere {
    // SAFETY: the host gives the sphere's hit group the record data of one
    // `Sphere`, copied byte for byte (its layout is the same on both
    // targets), 16-byte aligned as the record is.
    unsafe { (data as *const Sphere).read() }
}
