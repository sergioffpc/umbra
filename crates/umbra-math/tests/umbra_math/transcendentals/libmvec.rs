//! glibc's vector transcendentals, through FFI: the variants of `libmvec`,
//! 4 lanes for SSE (`_ZGVbN4…`) and 8 for AVX2 (`_ZGVdN8…`). Only on
//! x86-64, and never under Miri, which cannot pass `__m128` arguments.
//! glibc's scalar functions come through `umbra_math`'s wrappers instead.
//!
//! The `unsafe` follows ADR 0007: one FFI call or one lane conversion per
//! function, each under its own `#[expect(unsafe_code)]`. The safe wrappers
//! are the only way the rest of the module reaches glibc.

use core::arch::x86_64::{__m128, __m256};
use core::mem::transmute;

use umbra_math::Vec4;

use super::{Transcendentals, map, map_pair, map_vec4, map2};

// The vector functions are declared without their signatures: SIMD types in
// an `extern` block need the unstable `simd_ffi` (ADR 0027 keeps unstable
// features in `umbra-kernels`). `libmvec!` gives each address its type, a
// function pointer, where `__m128` and `__m256` are allowed; with AVX2
// enabled, the System V ABI passes them in `xmm`/`ymm` registers, as
// `libmvec` takes them.
#[link(name = "mvec")]
#[expect(
    unsafe_code,
    reason = "glibc's vector functions, for the benchmark (ADR 0007)"
)]
unsafe extern "C" {
    fn _ZGVbN4v_sinf();
    fn _ZGVbN4v_cosf();
    fn _ZGVbN4vv_atan2f();
    fn _ZGVbN4v_erff();
    fn _ZGVbN4v_expf();
    fn _ZGVbN4vv_powf();
    fn _ZGVdN8v_sinf();
    fn _ZGVdN8v_cosf();
    fn _ZGVdN8vv_atan2f();
    fn _ZGVdN8v_erff();
    fn _ZGVdN8v_expf();
    fn _ZGVdN8vv_powf();
}

/// A safe wrapper of a `libmvec` function of `[f32; N]` arguments, called
/// as a function pointer of its vector type.
macro_rules! libmvec {
    ($name:ident = $symbol:ident($($arg:ident),+): [f32; $n:literal] as $vector:ty, $to:ident, $from:ident) => {
        #[expect(unsafe_code, reason = "a call into libmvec, for the benchmark (ADR 0007)")]
        fn $name($($arg: [f32; $n]),+) -> [f32; $n] {
            let symbol: unsafe extern "C" fn() = $symbol;
            // SAFETY: `$symbol` is the `libmvec` function of that many
            // vectors of `$n` lanes, by its vector-ABI name, and that is its
            // C signature.
            let f = unsafe {
                transmute::<
                    unsafe extern "C" fn(),
                    unsafe extern "C" fn($(libmvec!(@vector $arg $vector)),+) -> $vector,
                >(symbol)
            };
            // SAFETY: a pure function of its vector arguments, whose
            // instruction set `umbra-math` requires (x86-64-v3).
            $from(unsafe { f($($to($arg)),+) })
        }
    };
    (@vector $arg:ident $vector:ty) => { $vector };
}

libmvec!(sin_4 = _ZGVbN4v_sinf(x): [f32; 4] as __m128, to_m128, from_m128);
libmvec!(cos_4 = _ZGVbN4v_cosf(x): [f32; 4] as __m128, to_m128, from_m128);
libmvec!(atan2_4 = _ZGVbN4vv_atan2f(y, x): [f32; 4] as __m128, to_m128, from_m128);
libmvec!(erf_4 = _ZGVbN4v_erff(x): [f32; 4] as __m128, to_m128, from_m128);
libmvec!(exp_4 = _ZGVbN4v_expf(x): [f32; 4] as __m128, to_m128, from_m128);
libmvec!(pow_4 = _ZGVbN4vv_powf(x, y): [f32; 4] as __m128, to_m128, from_m128);
libmvec!(sin_8 = _ZGVdN8v_sinf(x): [f32; 8] as __m256, to_m256, from_m256);
libmvec!(cos_8 = _ZGVdN8v_cosf(x): [f32; 8] as __m256, to_m256, from_m256);
libmvec!(atan2_8 = _ZGVdN8vv_atan2f(y, x): [f32; 8] as __m256, to_m256, from_m256);
libmvec!(erf_8 = _ZGVdN8v_erff(x): [f32; 8] as __m256, to_m256, from_m256);
libmvec!(exp_8 = _ZGVdN8v_expf(x): [f32; 8] as __m256, to_m256, from_m256);
libmvec!(pow_8 = _ZGVdN8vv_powf(x, y): [f32; 8] as __m256, to_m256, from_m256);

/// glibc's `libmvec` for SSE, four lanes per call: `sinf` and `cosf`
/// (`sincosf`'s vector variant writes through vectors of pointers),
/// `atan2f`, `erff`, `expf`, `powf`.
#[derive(Debug)]
pub(crate) struct Libmvec4;

/// glibc's `libmvec` for AVX2, eight lanes per call, so two `Vec4`s.
#[derive(Debug)]
pub(crate) struct Libmvec8;

impl Transcendentals for Libmvec4 {
    fn sin_cos(x: &[f32], sin: &mut [f32], cos: &mut [f32]) {
        map_pair(x, sin, cos, |x| (sin_4(x), cos_4(x)));
    }
    fn atan2(y: &[f32], x: &[f32], out: &mut [f32]) {
        map2(y, x, out, atan2_4);
    }
    fn erf(x: &[f32], out: &mut [f32]) {
        map(x, out, erf_4);
    }
    fn exp4(v: &[Vec4], out: &mut [Vec4]) {
        map_vec4(v, out, |[v]| [Vec4::from(exp_4(v.to_array()))]);
    }
    fn pow4(v: &[Vec4], e: f32, out: &mut [Vec4]) {
        map_vec4(v, out, |[v]| [Vec4::from(pow_4(v.to_array(), [e; 4]))]);
    }
}

impl Transcendentals for Libmvec8 {
    fn sin_cos(x: &[f32], sin: &mut [f32], cos: &mut [f32]) {
        map_pair(x, sin, cos, |x| (sin_8(x), cos_8(x)));
    }
    fn atan2(y: &[f32], x: &[f32], out: &mut [f32]) {
        map2(y, x, out, atan2_8);
    }
    fn erf(x: &[f32], out: &mut [f32]) {
        map(x, out, erf_8);
    }
    fn exp4(v: &[Vec4], out: &mut [Vec4]) {
        map_vec4(v, out, |v| split(exp_8(join(v))));
    }
    fn pow4(v: &[Vec4], e: f32, out: &mut [Vec4]) {
        map_vec4(v, out, |v| split(pow_8(join(v), [e; 8])));
    }
}

/// The lanes of two `Vec4`s, in order.
fn join([a, b]: [Vec4; 2]) -> [f32; 8] {
    let ([a0, a1, a2, a3], [b0, b1, b2, b3]) = (a.to_array(), b.to_array());
    [a0, a1, a2, a3, b0, b1, b2, b3]
}

/// Eight lanes as two `Vec4`s, in order.
fn split([a0, a1, a2, a3, b0, b1, b2, b3]: [f32; 8]) -> [Vec4; 2] {
    [Vec4::new(a0, a1, a2, a3), Vec4::new(b0, b1, b2, b3)]
}

#[expect(unsafe_code, reason = "a lane conversion for an FFI call (ADR 0007)")]
fn to_m128(x: [f32; 4]) -> __m128 {
    // SAFETY: `__m128` is four `f32` lanes in order, 16 bytes like
    // `[f32; 4]`, and every bit pattern is valid for both.
    unsafe { transmute::<[f32; 4], __m128>(x) }
}

#[expect(unsafe_code, reason = "a lane conversion for an FFI call (ADR 0007)")]
fn from_m128(x: __m128) -> [f32; 4] {
    // SAFETY: as in `to_m128`, the other way round.
    unsafe { transmute::<__m128, [f32; 4]>(x) }
}

#[expect(unsafe_code, reason = "a lane conversion for an FFI call (ADR 0007)")]
fn to_m256(x: [f32; 8]) -> __m256 {
    // SAFETY: `__m256` is eight `f32` lanes in order, 32 bytes like
    // `[f32; 8]`, and every bit pattern is valid for both.
    unsafe { transmute::<[f32; 8], __m256>(x) }
}

#[expect(unsafe_code, reason = "a lane conversion for an FFI call (ADR 0007)")]
fn from_m256(x: __m256) -> [f32; 8] {
    // SAFETY: as in `to_m256`, the other way round.
    unsafe { transmute::<__m256, [f32; 8]>(x) }
}
