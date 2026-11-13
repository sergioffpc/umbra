//! The CPU's three implementations of each function: glibc's scalar `libm`,
//! umbra-math as it is, and the best SIMD there is (glibc's `libmvec`, or the
//! vector instruction).

use std::ffi::{CStr, CString, c_char, c_int, c_void};
use std::hint::black_box;

use umbra_math::Vec4;

use crate::funcs::{GAMMA, Inputs};
use crate::{Column, Implementation, Run};

/// glibc's scalar `libm`, declared here rather than taken through
/// umbra-math's wrappers, so the column is glibc and nothing else.
mod glibc {
    #[link(name = "m")]
    unsafe extern "C" {
        pub fn sinf(x: f32) -> f32;
        pub fn cosf(x: f32) -> f32;
        pub fn sincosf(x: f32, s: *mut f32, c: *mut f32);
        pub fn tanf(x: f32) -> f32;
        pub fn asinf(x: f32) -> f32;
        pub fn acosf(x: f32) -> f32;
        pub fn atan2f(y: f32, x: f32) -> f32;
        pub fn coshf(x: f32) -> f32;
        pub fn atanhf(x: f32) -> f32;
        pub fn expf(x: f32) -> f32;
        pub fn exp2f(x: f32) -> f32;
        pub fn logf(x: f32) -> f32;
        pub fn powf(x: f32, y: f32) -> f32;
        pub fn erff(x: f32) -> f32;
        pub fn sqrtf(x: f32) -> f32;
        pub fn fmaf(a: f32, b: f32, c: f32) -> f32;
        pub fn floorf(x: f32) -> f32;
        pub fn ceilf(x: f32) -> f32;
        pub fn roundf(x: f32) -> f32;
    }
}

// ---- batch maps: one call per N elements, each result through black_box,
// so LLVM cannot vectorise a scalar implementation across calls ----

fn chunks<const N: usize>(x: &[f32]) -> &[[f32; N]] {
    x.as_chunks().0
}

fn chunks_mut<const N: usize>(x: &mut [f32]) -> &mut [[f32; N]] {
    x.as_chunks_mut().0
}

/// `out = f(a, b, c)`, `N` elements per call.
fn map<const N: usize>(
    inputs: &Inputs,
    out: &mut [f32],
    f: impl Fn([f32; N], [f32; N], [f32; N]) -> [f32; N],
) {
    let args = chunks(&inputs.a)
        .iter()
        .zip(chunks(&inputs.b))
        .zip(chunks(&inputs.c));
    for (o, ((&a, &b), &c)) in chunks_mut(out).iter_mut().zip(args) {
        *o = black_box(f(a, b, c));
    }
}

/// `(p, q) = f(a)`, `N` elements per call.
fn map_pair<const N: usize>(
    inputs: &Inputs,
    p: &mut [f32],
    q: &mut [f32],
    f: impl Fn([f32; N]) -> ([f32; N], [f32; N]),
) {
    let outs = chunks_mut(p).iter_mut().zip(chunks_mut(q).iter_mut());
    for ((p, q), &a) in outs.zip(chunks(&inputs.a)) {
        (*p, *q) = black_box(f(a));
    }
}

/// One element per call.
fn scalar(f: impl Fn(f32, f32, f32) -> f32 + 'static) -> Run {
    Box::new(move |i, o, _| map::<1>(i, o, |[a], [b], [c]| [f(a, b, c)]))
}

/// One element per call, two results.
fn scalar_pair(f: impl Fn(f32) -> (f32, f32) + 'static) -> Run {
    Box::new(move |i, p, q| {
        map_pair::<1>(i, p, q, |[a]| {
            let (s, c) = f(a);
            ([s], [c])
        });
    })
}

/// One `Vec4` per call.
fn vec4(f: impl Fn(Vec4) -> Vec4 + 'static) -> Run {
    Box::new(move |i, o, _| map::<4>(i, o, |a, _, _| f(Vec4::from(a)).to_array()))
}

/// [`LANES`] elements per call, which LLVM turns into the vector instruction.
fn lanes(f: impl Fn(f32, f32, f32) -> f32 + 'static) -> Run {
    Box::new(move |i, o, _| {
        map::<LANES>(i, o, |a, b, c| std::array::from_fn(|k| f(a[k], b[k], c[k])));
    })
}

// ---- libmvec, found by `dlsym`: a glibc without a function gives no column ----

#[cfg(target_arch = "x86_64")]
mod arch {
    /// AVX2, which umbra-math requires on x86-64.
    pub type V = core::arch::x86_64::__m256;
    pub const LANES: usize = 8;
    pub const PREFIX: &str = "_ZGVdN8";
}

#[cfg(target_arch = "aarch64")]
mod arch {
    /// AdvSIMD (NEON), in every arm64 CPU.
    pub type V = core::arch::aarch64::float32x4_t;
    pub const LANES: usize = 4;
    pub const PREFIX: &str = "_ZGVnN4";
}

use arch::{LANES, PREFIX, V};

// The vector ABI passes `V` in one SIMD register, as the C ABI does with the
// target's vector feature on; arm64's vector PCS saves more registers than
// the base one, so calling it as `extern "C"` is sound.
type Unary = unsafe extern "C" fn(V) -> V;
type Binary = unsafe extern "C" fn(V, V) -> V;
type SinCos = unsafe extern "C" fn(V, *mut f32, *mut f32);

unsafe extern "C" {
    fn dlopen(file: *const c_char, mode: c_int) -> *mut c_void;
    fn dlsym(handle: *mut c_void, name: *const c_char) -> *mut c_void;
}

/// `libmvec.so.1`, or null when it is not installed.
fn libmvec() -> *mut c_void {
    const RTLD_NOW: c_int = 2;
    const LIBMVEC: &CStr = c"libmvec.so.1";
    // SAFETY: a valid C string; loading glibc's libmvec runs no initialiser
    // of ours.
    unsafe { dlopen(LIBMVEC.as_ptr(), RTLD_NOW) }
}

/// The `libmvec` function `name` as the function pointer type `T`.
///
/// # Safety
///
/// `T` must be the function's signature under the vector ABI.
unsafe fn symbol<T: Copy>(name: &str) -> Option<T> {
    const {
        assert!(
            size_of::<T>() == size_of::<*mut c_void>(),
            "T is a function pointer"
        );
    };
    let handle = libmvec();
    if handle.is_null() {
        return None;
    }
    let name = CString::new(name).ok()?;
    // SAFETY: a live handle and a valid C string.
    let address = unsafe { dlsym(handle, name.as_ptr()) };
    // SAFETY: a non-null address of the function, whose type the caller vouches for.
    (!address.is_null()).then(|| unsafe { std::mem::transmute_copy::<*mut c_void, T>(&address) })
}

fn to_vector(x: [f32; LANES]) -> V {
    // SAFETY: `V` is `LANES` f32 lanes in order, the same size, every bit
    // pattern valid for both.
    unsafe { std::mem::transmute(x) }
}

fn from_vector(x: V) -> [f32; LANES] {
    // SAFETY: as in `to_vector`, the other way round.
    unsafe { std::mem::transmute(x) }
}

/// `libmvec`'s unary `name`.
fn libmvec_unary(name: &str) -> Option<(String, Run)> {
    let symbol_name = format!("{PREFIX}v_{name}");
    // SAFETY: `_ZGV?N?v_<f>` takes and returns one vector.
    let f: Unary = unsafe { symbol(&symbol_name) }?;
    // SAFETY: a pure function of its vector argument.
    let run: Run = Box::new(move |i, o, _| {
        map::<LANES>(i, o, |a, _, _| from_vector(unsafe { f(to_vector(a)) }));
    });
    Some((symbol_name, run))
}

/// `libmvec`'s binary `name`.
fn libmvec_binary(name: &str) -> Option<(String, Run)> {
    let symbol_name = format!("{PREFIX}vv_{name}");
    // SAFETY: `_ZGV?N?vv_<f>` takes two vectors and returns one.
    let f: Binary = unsafe { symbol(&symbol_name) }?;
    // SAFETY: a pure function of its vector arguments.
    let run: Run = Box::new(move |i, o, _| {
        map::<LANES>(i, o, |a, b, _| {
            from_vector(unsafe { f(to_vector(a), to_vector(b)) })
        });
    });
    Some((symbol_name, run))
}

/// `libmvec`'s `sincosf` with linear pointers where this glibc has it,
/// else its `sinf` and `cosf`.
fn libmvec_sincos() -> Option<(String, Run)> {
    let symbol_name = format!("{PREFIX}vl4l4_sincosf");
    // SAFETY: `_ZGV?N?vl4l4_sincosf` takes a vector and two pointers to
    // `LANES` consecutive floats.
    if let Some(f) = unsafe { symbol::<SinCos>(&symbol_name) } {
        let run: Run = Box::new(move |i, p, q| map_pair::<LANES>(i, p, q, |a| sin_cos_lanes(f, a)));
        return Some((symbol_name, run));
    }
    let (sin, cos) = (libmvec_vector_fn("sinf")?, libmvec_vector_fn("cosf")?);
    let run: Run = Box::new(move |i, p, q| {
        // SAFETY: pure functions of their vector argument.
        map_pair::<LANES>(i, p, q, |a| unsafe {
            (
                from_vector(sin(to_vector(a))),
                from_vector(cos(to_vector(a))),
            )
        });
    });
    Some((format!("{PREFIX}v_sinf + {PREFIX}v_cosf"), run))
}

/// The sines and cosines of `a` from the linear-pointer `sincosf`.
fn sin_cos_lanes(f: SinCos, a: [f32; LANES]) -> ([f32; LANES], [f32; LANES]) {
    let (mut s, mut c) = ([0.0; LANES], [0.0; LANES]);
    // SAFETY: both point to `LANES` writable floats.
    unsafe { f(to_vector(a), s.as_mut_ptr(), c.as_mut_ptr()) };
    (s, c)
}

fn libmvec_vector_fn(name: &str) -> Option<Unary> {
    // SAFETY: `_ZGV?N?v_<f>` takes and returns one vector.
    unsafe { symbol(&format!("{PREFIX}v_{name}")) }
}

// ---- the three columns ----

/// glibc's scalar function, one call per element.
fn baseline(name: &'static str) -> Implementation {
    use glibc::{
        acosf, asinf, atan2f, atanhf, ceilf, cosf, coshf, erff, exp2f, expf, floorf, fmaf, logf,
        powf, roundf, sincosf, sinf, sqrtf, tanf,
    };
    // SAFETY (every call): a glibc math function of `f32` values, no
    // preconditions; `sincosf` writes one float through each pointer.
    let run = unsafe {
        match name {
            "sinf" => scalar(|a, _, _| sinf(a)),
            "cosf" => scalar(|a, _, _| cosf(a)),
            "sincosf" => scalar_pair(|a| {
                let (mut s, mut c) = (0.0, 0.0);
                sincosf(a, &raw mut s, &raw mut c);
                (s, c)
            }),
            "tanf" => scalar(|a, _, _| tanf(a)),
            "asinf" => scalar(|a, _, _| asinf(a)),
            "acosf" => scalar(|a, _, _| acosf(a)),
            "atan2f" => scalar(|a, b, _| atan2f(a, b)),
            "coshf" => scalar(|a, _, _| coshf(a)),
            "atanhf" => scalar(|a, _, _| atanhf(a)),
            "expf" => scalar(|a, _, _| expf(a)),
            "exp2f" => scalar(|a, _, _| exp2f(a)),
            "logf" => scalar(|a, _, _| logf(a)),
            "powf" => scalar(|a, b, _| powf(a, b)),
            "erff" => scalar(|a, _, _| erff(a)),
            "sqrtf" => scalar(|a, _, _| sqrtf(a)),
            "fmaf" => scalar(|a, b, c| fmaf(a, b, c)),
            "floorf" => scalar(|a, _, _| floorf(a)),
            "ceilf" => scalar(|a, _, _| ceilf(a)),
            "roundf" => scalar(|a, _, _| roundf(a)),
            _ => unreachable!("no glibc function {name}"),
        }
    };
    Implementation {
        column: Column::Baseline,
        note: name.to_owned(),
        run,
    }
}

/// What umbra-math offers for the function today: its own kernel where it
/// has one, the hardware instruction, or its wrapper of glibc.
fn umbra(name: &str) -> Implementation {
    use umbra_math as m;
    let (note, run) = match name {
        "sinf" => ("wrapper sinf", scalar(|a, _, _| m::sinf(a))),
        "cosf" => ("wrapper cosf", scalar(|a, _, _| m::cosf(a))),
        "sincosf" => ("own sin_cos", scalar_pair(m::sin_cos)),
        "tanf" => ("wrapper tanf", scalar(|a, _, _| m::tanf(a))),
        "asinf" => ("wrapper asinf", scalar(|a, _, _| m::asinf(a))),
        "acosf" => ("wrapper acosf", scalar(|a, _, _| m::acosf(a))),
        "atan2f" => ("own atan2", scalar(|a, b, _| m::atan2(a, b))),
        "coshf" => ("wrapper coshf", scalar(|a, _, _| m::coshf(a))),
        "atanhf" => ("wrapper atanhf", scalar(|a, _, _| m::atanhf(a))),
        "expf" => ("Vec4::exp", vec4(Vec4::exp)),
        "exp2f" => ("wrapper exp2f", scalar(|a, _, _| m::exp2f(a))),
        "logf" => ("wrapper logf", scalar(|a, _, _| m::logf(a))),
        "powf" => ("Vec4::pow", vec4(|v| v.pow(GAMMA))),
        "erff" => ("own erf", scalar(|a, _, _| m::erf(a))),
        "sqrtf" => ("sqrt, the instruction", scalar(|a, _, _| m::sqrt(a))),
        "fmaf" => ("fma, the instruction", scalar(m::fma)),
        "floorf" => ("floor, the instruction", scalar(|a, _, _| m::floor(a))),
        "ceilf" => ("ceil, the instruction", scalar(|a, _, _| m::ceil(a))),
        "roundf" => ("wrapper roundf", scalar(|a, _, _| m::roundf(a))),
        _ => unreachable!("no umbra-math function {name}"),
    };
    Implementation {
        column: Column::Umbra,
        note: note.to_owned(),
        run,
    }
}

/// The fastest SIMD form: `libmvec` for the transcendentals, the vector
/// instruction for the exact operations.
fn best(name: &str) -> Option<Implementation> {
    let instruction = || format!("vector instruction ×{LANES}");
    let (note, run) = match name {
        "sincosf" => libmvec_sincos(),
        "atan2f" | "powf" => libmvec_binary(name),
        "sqrtf" => Some((instruction(), lanes(|a, _, _| a.sqrt()))),
        "fmaf" => Some((instruction(), lanes(f32::mul_add))),
        "floorf" => Some((instruction(), lanes(|a, _, _| a.floor()))),
        "ceilf" => Some((instruction(), lanes(|a, _, _| a.ceil()))),
        "roundf" => Some((instruction(), lanes(|a, _, _| a.round()))),
        _ => libmvec_unary(name),
    }?;
    Some(Implementation {
        column: Column::Best,
        note,
        run,
    })
}

/// The implementations of `name` on this CPU.
pub fn implementations(name: &'static str) -> Vec<Implementation> {
    [Some(baseline(name)), Some(umbra(name)), best(name)]
        .into_iter()
        .flatten()
        .collect()
}
