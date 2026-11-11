//! The OptiX device API, written against OptiX 9.0 (ABI 105): each
//! function is one `call` to an `_optix_*` built-in in inline PTX, which
//! OptiX resolves when it compiles the module (ADR 0028).
//!
//! The built-ins' names, operands and constants are interoperability facts
//! of OptiX's ABI, written by hand from its documentation and headers; no
//! header text is copied (ADR 0026, NOTICE). Each built-in is valid only in
//! some program types, given on its wrapper: OptiX rejects a module that
//! calls one elsewhere, so a misuse fails when the host creates the module,
//! not at run time.

use core::arch::asm;

/// `OPTIX_HIT_KIND_TRIANGLE_FRONT_FACE`: the hit kind of a built-in
/// triangle hit from its front.
const HIT_KIND_TRIANGLE_FRONT_FACE: u32 = 0xFE;
/// `OPTIX_HIT_KIND_TRIANGLE_BACK_FACE`: the same from its back.
const HIT_KIND_TRIANGLE_BACK_FACE: u32 = 0xFF;

/// The primitive a closest-hit program was called for.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Primitive {
    /// A built-in triangle.
    Triangle,
    /// A custom primitive, whose intersection program reported the hit.
    Custom,
}

/// The launch index in `x`: `optixGetLaunchIndex().x`. Any program type.
#[expect(
    unsafe_code,
    reason = "OptiX device API in inline PTX (ADR 0007, ADR 0028)"
)]
pub(crate) fn launch_index_x() -> u32 {
    let x: u32;
    // SAFETY: the built-in reads the launch state, writes only `x` and has
    // no precondition.
    unsafe {
        asm!("call ({x}), _optix_get_launch_index_x, ();", x = out(reg32) x, options(nomem, nostack));
    }
    x
}

/// The 64-bit word at byte `OFFSET` of the launch parameters, the `.const`
/// variable `params` that `lib.rs` declares. Any program type.
#[expect(
    unsafe_code,
    reason = "OptiX device API in inline PTX (ADR 0007, ADR 0028)"
)]
pub(crate) fn launch_param_u64<const OFFSET: usize>() -> u64 {
    let word: u64;
    // SAFETY: `params` is as large as `LaunchParams` and the callers' offsets
    // are its fields', asserted in `launch.rs`; the host writes it before the
    // launch and nothing writes it during one.
    unsafe {
        asm!("ld.const.u64 {w}, [params+{o}];", w = out(reg64) word, o = const OFFSET, options(pure, readonly, nostack));
    }
    word
}

/// The 32-bit word at byte `OFFSET` of the launch parameters, as
/// [`launch_param_u64`].
#[expect(
    unsafe_code,
    reason = "OptiX device API in inline PTX (ADR 0007, ADR 0028)"
)]
pub(crate) fn launch_param_u32<const OFFSET: usize>() -> u32 {
    let word: u32;
    // SAFETY: as in `launch_param_u64`.
    unsafe {
        asm!("ld.const.u32 {w}, [params+{o}];", w = out(reg32) word, o = const OFFSET, options(pure, readonly, nostack));
    }
    word
}

/// Sets payload register `i` of the current ray: `optixSetPayload_i`.
/// Intersection, any-hit, closest-hit and miss programs.
#[expect(
    unsafe_code,
    reason = "OptiX device API in inline PTX (ADR 0007, ADR 0028)"
)]
pub(crate) fn set_payload(i: u32, value: u32) {
    // SAFETY: the built-in writes payload register `i` of the current ray,
    // one of the pipeline's `numPayloadValues`, and nothing else.
    unsafe {
        asm!("call _optix_set_payload, ({i}, {v});", i = in(reg32) i, v = in(reg32) value, options(nostack));
    }
}

/// The current ray's `tmax`: in an intersection or any-hit program the
/// closest hit so far, in a closest-hit program the hit's distance:
/// `optixGetRayTmax`.
#[expect(
    unsafe_code,
    reason = "OptiX device API in inline PTX (ADR 0007, ADR 0028)"
)]
pub(crate) fn ray_tmax() -> f32 {
    let t: f32;
    // SAFETY: the built-in reads the current ray's state and writes only `t`.
    unsafe {
        asm!(
            "{{ .reg .f32 %t; call (%t), _optix_get_ray_tmax, (); mov.b32 {t}, %t; }}",
            t = out(reg32) t,
            options(nostack)
        );
    }
    t
}

/// The current ray's origin in the space of the primitive being
/// intersected: `optixGetObjectRayOrigin`. Intersection and any-hit
/// programs.
pub(crate) fn object_ray_origin() -> [f32; 3] {
    [
        object_ray_origin_x(),
        object_ray_origin_y(),
        object_ray_origin_z(),
    ]
}

/// The current ray's direction in the space of the primitive being
/// intersected: `optixGetObjectRayDirection`. Intersection and any-hit
/// programs.
pub(crate) fn object_ray_direction() -> [f32; 3] {
    [
        object_ray_direction_x(),
        object_ray_direction_y(),
        object_ray_direction_z(),
    ]
}

/// Defines `$name` as the call to the built-in `$builtin`, which takes no
/// operand and returns one `f32` read from the current ray's state.
macro_rules! ray_component {
    ($($name:ident = $builtin:literal),+ $(,)?) => {$(
        #[expect(unsafe_code, reason = "OptiX device API in inline PTX (ADR 0007, ADR 0028)")]
        fn $name() -> f32 {
            let value: f32;
            // SAFETY: the built-in reads the current ray's state and writes
            // only `value`.
            unsafe {
                asm!(
                    concat!("{{ .reg .f32 %v; call (%v), ", $builtin, ", (); mov.b32 {v}, %v; }}"),
                    v = out(reg32) value,
                    options(nostack)
                );
            }
            value
        }
    )+};
}

ray_component! {
    object_ray_origin_x = "_optix_get_object_ray_origin_x",
    object_ray_origin_y = "_optix_get_object_ray_origin_y",
    object_ray_origin_z = "_optix_get_object_ray_origin_z",
    object_ray_direction_x = "_optix_get_object_ray_direction_x",
    object_ray_direction_y = "_optix_get_object_ray_direction_y",
    object_ray_direction_z = "_optix_get_object_ray_direction_z",
}

/// The device address of the data after the header of the current program's
/// shader binding table record: `optixGetSbtDataPointer`. Any program type.
#[expect(
    unsafe_code,
    reason = "OptiX device API in inline PTX (ADR 0007, ADR 0028)"
)]
pub(crate) fn sbt_data_pointer() -> u64 {
    let pointer: u64;
    // SAFETY: the built-in reads the launch state and writes only `pointer`.
    unsafe {
        asm!("call ({p}), _optix_get_sbt_data_ptr_64, ();", p = out(reg64) pointer, options(nomem, nostack));
    }
    pointer
}

/// The primitive the current hit is on, from `optixGetHitKind`.
/// Closest-hit and any-hit programs.
#[expect(
    unsafe_code,
    reason = "OptiX device API in inline PTX (ADR 0007, ADR 0028)"
)]
pub(crate) fn hit_primitive() -> Primitive {
    let kind: u32;
    // SAFETY: the built-in reads the current hit's state and writes only
    // `kind`.
    unsafe {
        asm!("call ({k}), _optix_get_hit_kind, ();", k = out(reg32) kind, options(nostack));
    }
    match kind {
        HIT_KIND_TRIANGLE_FRONT_FACE | HIT_KIND_TRIANGLE_BACK_FACE => Primitive::Triangle,
        _ => Primitive::Custom,
    }
}

/// Reports a hit at ray parameter `t`, with hit kind 0 and no attributes:
/// `optixReportIntersection`. OptiX takes it if `t` lies within the ray's
/// interval and drops it otherwise, which needs nothing from the caller.
/// Intersection programs.
#[expect(
    unsafe_code,
    reason = "OptiX device API in inline PTX (ADR 0007, ADR 0028)"
)]
pub(crate) fn report_intersection(t: f32) {
    // SAFETY: the built-in records a candidate hit of the current ray, with
    // the user hit kind 0, and writes only its unused result register.
    unsafe {
        asm!(
            "{{ .reg .f32 %t; mov.b32 %t, {t}; call ({a}), _optix_report_intersection_0, (%t, {k}); }}",
            a = out(reg32) _,
            t = in(reg32) t,
            k = in(reg32) 0u32,
            options(nostack)
        );
    }
}

/// A ray to trace, in render space, from `tmin` to `tmax`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) struct TraceRay {
    /// The origin.
    pub(crate) origin: [f32; 3],
    /// The direction.
    pub(crate) direction: [f32; 3],
    /// The smallest ray parameter a hit may have.
    pub(crate) tmin: f32,
    /// The largest ray parameter a hit may have.
    pub(crate) tmax: f32,
}

/// Traces `ray` through `handle` with two payload registers, returned after
/// the closest-hit or miss program has run: `optixTrace` with the default
/// payload type, ray time 0, visibility mask `0xFF`, no ray flags, the
/// first hit group record (SBT offset 0, stride 1) and miss record 0.
/// Ray-generation, closest-hit and miss programs.
#[expect(
    unsafe_code,
    reason = "OptiX device API in inline PTX (ADR 0007, ADR 0028)"
)]
#[expect(
    clippy::too_many_lines,
    reason = "one call whose 32 payload registers in and out are one operand each"
)]
pub(crate) fn trace(handle: u64, ray: TraceRay, payload: [u32; 2]) -> [u32; 2] {
    let [mut p0, mut p1] = payload;
    let [ox, oy, oz] = ray.origin;
    let [dx, dy, dz] = ray.direction;
    // SAFETY: `_optix_trace_typed_32` takes 32 payload registers in and out,
    // of which the first `size` (2) are live and match the pipeline's
    // `numPayloadValues`; `handle` is a traversable the host built for this
    // pipeline, whose shader binding table has a hit group record 0.
    unsafe {
        asm!(
            "{{ .reg .f32 %rf<9>;",
            "mov.b32 %rf0, {ox}; mov.b32 %rf1, {oy}; mov.b32 %rf2, {oz};",
            "mov.b32 %rf3, {dx}; mov.b32 %rf4, {dy}; mov.b32 %rf5, {dz};",
            "mov.b32 %rf6, {tmin}; mov.b32 %rf7, {tmax}; mov.b32 %rf8, {time};",
            "call ({p0},{p1},{p2},{p3},{p4},{p5},{p6},{p7},{p8},{p9},{p10},{p11},{p12},{p13},{p14},{p15},{p16},{p17},{p18},{p19},{p20},{p21},{p22},{p23},{p24},{p25},{p26},{p27},{p28},{p29},{p30},{p31}),",
            "_optix_trace_typed_32,",
            "({ty},{handle},%rf0,%rf1,%rf2,%rf3,%rf4,%rf5,%rf6,%rf7,%rf8,{mask},{flags},{sbt_offset},{sbt_stride},{miss},{size},{p0},{p1},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero},{zero}); }}",
            ty = in(reg32) 0u32,
            handle = in(reg64) handle,
            ox = in(reg32) ox,
            oy = in(reg32) oy,
            oz = in(reg32) oz,
            dx = in(reg32) dx,
            dy = in(reg32) dy,
            dz = in(reg32) dz,
            tmin = in(reg32) ray.tmin,
            tmax = in(reg32) ray.tmax,
            time = in(reg32) 0.0f32,
            mask = in(reg32) 0xFFu32,
            flags = in(reg32) 0u32,
            sbt_offset = in(reg32) 0u32,
            sbt_stride = in(reg32) 1u32,
            miss = in(reg32) 0u32,
            size = in(reg32) 2u32,
            zero = in(reg32) 0u32,
            p0 = inout(reg32) p0,
            p1 = inout(reg32) p1,
            p2 = out(reg32) _,
            p3 = out(reg32) _,
            p4 = out(reg32) _,
            p5 = out(reg32) _,
            p6 = out(reg32) _,
            p7 = out(reg32) _,
            p8 = out(reg32) _,
            p9 = out(reg32) _,
            p10 = out(reg32) _,
            p11 = out(reg32) _,
            p12 = out(reg32) _,
            p13 = out(reg32) _,
            p14 = out(reg32) _,
            p15 = out(reg32) _,
            p16 = out(reg32) _,
            p17 = out(reg32) _,
            p18 = out(reg32) _,
            p19 = out(reg32) _,
            p20 = out(reg32) _,
            p21 = out(reg32) _,
            p22 = out(reg32) _,
            p23 = out(reg32) _,
            p24 = out(reg32) _,
            p25 = out(reg32) _,
            p26 = out(reg32) _,
            p27 = out(reg32) _,
            p28 = out(reg32) _,
            p29 = out(reg32) _,
            p30 = out(reg32) _,
            p31 = out(reg32) _,
            options(nostack),
        );
    }
    [p0, p1]
}

/// Stops the thread with PTX's `trap`, which aborts the launch.
#[expect(
    unsafe_code,
    reason = "OptiX device API in inline PTX (ADR 0007, ADR 0028)"
)]
pub(crate) fn trap() -> ! {
    // SAFETY: `trap` ends the kernel with an error the host sees from the
    // launch; it touches no memory.
    unsafe { asm!("trap;", options(noreturn, nostack)) }
}
