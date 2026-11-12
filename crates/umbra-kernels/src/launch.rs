//! The launch parameters and buffer records of the trace and math
//! programs: the layout shared by the host and the device.
//!
//! `umbra-kernels` and `umbra-gpu` both compile this file, the second by
//! `#[path]`, so the two sides read one definition. It depends on `core`
//! only. Every type is `#[repr(C)]` with layout assertions, which hold on
//! `x86_64` and `nvptx64` alike (ADR 0028).

/// What the host passes to one launch, in the `.const` variable `params`
/// that the pipeline names as its launch parameters.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(C)]
pub(crate) struct LaunchParams {
    /// The traversable handle of the acceleration structure to trace.
    pub(crate) handle: u64,
    /// Device address of `count` [`RayRecord`]s.
    pub(crate) rays: u64,
    /// Device address of `count` [`HitRecord`]s, written by the launch.
    pub(crate) hits: u64,
    /// The number of rays, at most the launch width.
    pub(crate) count: u32,
}

const _: () = {
    use core::mem::offset_of;

    assert!(
        size_of::<LaunchParams>() == 32,
        "`LaunchParams` is 32 bytes, the last 4 padding"
    );
    assert!(
        offset_of!(LaunchParams, handle) == 0,
        "`handle` is at byte 0"
    );
    assert!(offset_of!(LaunchParams, rays) == 8, "`rays` is at byte 8");
    assert!(offset_of!(LaunchParams, hits) == 16, "`hits` is at byte 16");
    assert!(
        offset_of!(LaunchParams, count) == 24,
        "`count` is at byte 24"
    );
};

/// One ray to trace: origin and direction in render space.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
#[repr(C)]
pub(crate) struct RayRecord {
    /// The origin.
    pub(crate) o: [f32; 3],
    /// The direction.
    pub(crate) d: [f32; 3],
}

const _: () = {
    use core::mem::offset_of;

    assert!(size_of::<RayRecord>() == 24, "`RayRecord` is 24 bytes");
    assert!(offset_of!(RayRecord, o) == 0, "`RayRecord::o` is at byte 0");
    assert!(
        offset_of!(RayRecord, d) == 12,
        "`RayRecord::d` is at byte 12"
    );
};

/// What one ray hit: the ray parameter of the closest hit and the
/// [`HitKind`] as its `u32` discriminant, which the host converts back (device
/// memory may hold any bits).
#[derive(Debug, Clone, Copy, PartialEq, Default)]
#[repr(C)]
pub(crate) struct HitRecord {
    /// The ray parameter of the closest hit; unspecified on a miss.
    pub(crate) t: f32,
    /// The [`HitKind`] discriminant.
    pub(crate) kind: u32,
}

const _: () = {
    use core::mem::offset_of;

    assert!(size_of::<HitRecord>() == 8, "`HitRecord` is 8 bytes");
    assert!(offset_of!(HitRecord, t) == 0, "`HitRecord::t` is at byte 0");
    assert!(
        offset_of!(HitRecord, kind) == 4,
        "`HitRecord::kind` is at byte 4"
    );
};

/// What a ray hit, as [`HitRecord::kind`] stores it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub(crate) enum HitKind {
    /// Nothing.
    Miss = 0,
    /// A triangle, by OptiX's built-in intersection.
    Triangle = 1,
    /// A sphere, by the custom intersection program.
    Sphere = 2,
}

/// What the host passes to one launch of the math program, in the same
/// `.const` variable `params` as [`LaunchParams`], which is no smaller.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(C)]
pub(crate) struct MathParams {
    /// Device address of `count` argument records, `[f32; 3]` each: the
    /// function's arguments in order, the unused ones ignored.
    pub(crate) args: u64,
    /// Device address of `count` `f32` results, written by the launch.
    pub(crate) results: u64,
    /// The number of argument records, at most the launch width.
    pub(crate) count: u32,
    /// The [`MathFunction`] discriminant.
    pub(crate) function: u32,
}

const _: () = {
    use core::mem::offset_of;

    assert!(size_of::<MathParams>() == 24, "`MathParams` is 24 bytes");
    assert!(
        size_of::<MathParams>() <= size_of::<LaunchParams>(),
        "`MathParams` fits the `params` variable, sized for `LaunchParams`"
    );
    assert!(offset_of!(MathParams, args) == 0, "`args` is at byte 0");
    assert!(
        offset_of!(MathParams, results) == 8,
        "`results` is at byte 8"
    );
    assert!(offset_of!(MathParams, count) == 16, "`count` is at byte 16");
    assert!(
        offset_of!(MathParams, function) == 20,
        "`function` is at byte 20"
    );
};

/// The function of `umbra-math` a launch of the math program evaluates, as
/// [`MathParams::function`] stores it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub(crate) enum MathFunction {
    /// `fma(x, y, z)`.
    Fma = 0,
    /// `sqrt(x)`.
    Sqrt = 1,
    /// `floor(x)`.
    Floor = 2,
    /// `ceil(x)`.
    Ceil = 3,
    /// `roundf(x)`.
    Roundf = 4,
    /// `sinf(x)`.
    Sinf = 5,
    /// `sin_cos(x).0`.
    SinOfSinCos = 6,
    /// `sin_cos(x).1`.
    CosOfSinCos = 7,
    /// `tanf(x)`.
    Tanf = 8,
    /// `asinf(x)`.
    Asinf = 9,
    /// `acosf(x)`.
    Acosf = 10,
    /// `atan2(y, x)`.
    Atan2 = 11,
    /// `erf(x)`.
    Erf = 12,
    /// `expf(x)`.
    Expf = 13,
    /// `powf(x, y)`.
    Powf = 14,
    /// `logf(x)`.
    Logf = 15,
    /// `atanhf(x)`.
    Atanhf = 16,
    /// `coshf(x)`.
    Coshf = 17,
}
