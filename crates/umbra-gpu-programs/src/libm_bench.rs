//! The kernels of `umbra-gpu`'s libm bench: each function as CUDA's
//! `libdevice` computes it (`*_libdevice`), as `umbra-math`'s `nvptx64`
//! backend does (`*_umbra`), and as the approximate intrinsics do
//! (`*_fast`), where there are such forms. Plain CUDA kernels, without
//! OptiX: built only into the bench's own PTX, under `--cfg
//! umbra_libm_bench`, which leaves the OptiX programs out.
//!
//! - `bench_<kernel>(a, b, c, acc, iters)`: thread `t` evaluates `iters`
//!   inputs, entries `t, t + 1, …` of the 1024-entry inputs (which stay in
//!   L1), and writes the XOR of the results' bits to `acc[t]`, so no
//!   evaluation is dead code.
//! - `eval_<kernel>(a, b, c, out1, out2)`: thread `t < 1024` writes the
//!   results for entry `t`, for the error check.

use core::arch::asm;
use core::panic::PanicInfo;

use umbra_math::Vec4;

/// The entries of each input.
const N: usize = 1024;
/// The exponent of `powf`, as the host's.
const GAMMA: f32 = 2.2;

/// A panic is a bug: it aborts the launch, which the host reports.
#[panic_handler]
fn panic(_: &PanicInfo<'_>) -> ! {
    loop {}
}

/// Declares CUDA's `libdevice` functions of `f32` values, which
/// `umbra-gpu`'s build script links in, and a safe wrapper of each, of the
/// same name: the functions `umbra-math` does not wrap itself.
macro_rules! libdevice {
    ($($name:ident($($arg:ident),+);)*) => {
        /// The declarations: only the wrappers, and [`sin_cos`], call them.
        #[expect(unsafe_code, reason = "FFI to CUDA's libdevice (ADR 0007)")]
        mod ffi {
            unsafe extern "C" {
                $(pub(super) fn $name($($arg: f32),+) -> f32;)*
                pub(super) fn __nv_sincosf(x: f32, s: *mut f32, c: *mut f32);
                pub(super) fn __nv_fast_sincosf(x: f32, s: *mut f32, c: *mut f32);
            }
        }

        $(
            #[expect(unsafe_code, reason = "a call into CUDA's libdevice (ADR 0007)")]
            fn $name($($arg: f32),+) -> f32 {
                // SAFETY: a `libdevice` function of `f32` values, with no
                // precondition, that touches no memory.
                unsafe { ffi::$name($($arg),+) }
            }
        )*
    };
}

libdevice! {
    __nv_sinf(x);
    __nv_cosf(x);
    __nv_tanf(x);
    __nv_asinf(x);
    __nv_acosf(x);
    __nv_coshf(x);
    __nv_atanhf(x);
    __nv_expf(x);
    __nv_exp2f(x);
    __nv_logf(x);
    __nv_erff(x);
    __nv_fsqrt_rn(x);
    __nv_floorf(x);
    __nv_ceilf(x);
    __nv_roundf(x);
    __nv_atan2f(y, x);
    __nv_powf(x, y);
    __nv_fmaf(a, b, c);
    __nv_fast_sinf(x);
    __nv_fast_cosf(x);
    __nv_fast_tanf(x);
    __nv_fast_expf(x);
    __nv_fast_logf(x);
    __nv_fast_powf(x, y);
}

/// `ex2.approx.ftz.f32`. `asm!` has integer registers only on `nvptx64`, so
/// the operand moves through a `.f32` register of the block's own.
#[expect(unsafe_code, reason = "one PTX instruction in inline assembly")]
fn ex2_approx(x: f32) -> f32 {
    let r: u32;
    // SAFETY: one arithmetic instruction on registers.
    unsafe {
        asm!(
            "{{ .reg .f32 a, b; mov.b32 a, {x}; ex2.approx.ftz.f32 b, a; mov.b32 {r}, b; }}",
            x = in(reg32) x.to_bits(),
            r = out(reg32) r,
            options(pure, nomem, nostack),
        );
    }
    f32::from_bits(r)
}

/// `sqrt.approx.ftz.f32`, as [`ex2_approx`].
#[expect(unsafe_code, reason = "one PTX instruction in inline assembly")]
fn sqrt_approx(x: f32) -> f32 {
    let r: u32;
    // SAFETY: one arithmetic instruction on registers.
    unsafe {
        asm!(
            "{{ .reg .f32 a, b; mov.b32 a, {x}; sqrt.approx.ftz.f32 b, a; mov.b32 {r}, b; }}",
            x = in(reg32) x.to_bits(),
            r = out(reg32) r,
            options(pure, nomem, nostack),
        );
    }
    f32::from_bits(r)
}

/// A `sincosf` of `libdevice`, which writes through pointers.
#[expect(
    unsafe_code,
    reason = "a libdevice function that writes through pointers"
)]
fn sin_cos(f: unsafe extern "C" fn(f32, *mut f32, *mut f32), x: f32) -> (f32, f32) {
    let (mut s, mut c) = (0.0, 0.0);
    // SAFETY: writes one float through each pointer, to live locals.
    unsafe { f(x, &raw mut s, &raw mut c) };
    (s, c)
}

/// This thread's index in the launch: `%ctaid.x · %ntid.x + %tid.x`.
#[expect(unsafe_code, reason = "special registers in inline assembly")]
fn thread() -> usize {
    let t: u32;
    // SAFETY: reads special registers, readable in any kernel.
    unsafe {
        asm!(
            "{{ .reg .u32 b, d, i; mov.u32 b, %ctaid.x; mov.u32 d, %ntid.x; mov.u32 i, %tid.x; mad.lo.u32 {t}, b, d, i; }}",
            t = out(reg32) t,
            options(pure, nomem, nostack),
        );
    }
    t as usize
}

/// Entry `j` of an input.
///
/// # Safety
///
/// `p` points to [`N`] floats and `j < N`.
#[expect(unsafe_code, reason = "reads an input the host wrote")]
unsafe fn load(p: *const f32, j: usize) -> f32 {
    // SAFETY: as the caller guarantees.
    unsafe { *p.wrapping_add(j) }
}

/// The kernels of functions of up to three scalars, with two results
/// (the second is zero but for `sincosf`).
macro_rules! scalar {
    ($($kernel:ident: |$a:ident, $b:ident, $c:ident| $e:expr;)*) => {$(
        #[expect(unsafe_code, reason = "a CUDA kernel's entry points (ADR 0007)")]
        mod $kernel {
            use super::*;

            #[inline(always)]
            fn f($a: f32, $b: f32, $c: f32) -> (f32, f32) {
                // Every kernel takes three arguments; most use fewer.
                let _ = ($a, $b, $c);
                $e
            }

            /// # Safety
            ///
            /// `a`, `b`, `c` point to [`N`] floats, `acc` to one `u32` per thread.
            #[unsafe(export_name = concat!("bench_", stringify!($kernel)))]
            unsafe extern "ptx-kernel" fn bench(a: *const f32, b: *const f32, c: *const f32, acc: *mut u32, iters: u32) {
                let t = thread();
                let mut bits = 0_u32;
                for k in 0..iters as usize {
                    let j = (t + k) % N;
                    // SAFETY: `j < N`.
                    let (x, y, z) = unsafe { (load(a, j), load(b, j), load(c, j)) };
                    let (r, s) = f(x, y, z);
                    bits ^= r.to_bits() ^ s.to_bits();
                }
                // SAFETY: one `u32` per thread.
                unsafe { *acc.add(t) = bits };
            }

            /// # Safety
            ///
            /// Every pointer points to [`N`] floats.
            #[unsafe(export_name = concat!("eval_", stringify!($kernel)))]
            unsafe extern "ptx-kernel" fn eval(a: *const f32, b: *const f32, c: *const f32, out1: *mut f32, out2: *mut f32) {
                let t = thread();
                if t < N {
                    // SAFETY: `t < N`.
                    let (x, y, z) = unsafe { (load(a, t), load(b, t), load(c, t)) };
                    let (r, s) = f(x, y, z);
                    // SAFETY: `t < N`.
                    unsafe { *out1.add(t) = r };
                    // SAFETY: `t < N`.
                    unsafe { *out2.add(t) = s };
                }
            }
        }
    )*};
}

/// The kernels of `Vec4` functions: four consecutive entries per call, the
/// same number of evaluations.
macro_rules! vec4 {
    ($($kernel:ident: |$v:ident| $e:expr;)*) => {$(
        #[expect(unsafe_code, reason = "a CUDA kernel's entry points (ADR 0007)")]
        mod $kernel {
            use super::*;

            #[inline(always)]
            fn f($v: Vec4) -> Vec4 {
                $e
            }

            /// Entries `j..j + 4` of `a`.
            ///
            /// # Safety
            ///
            /// `a` points to [`N`] floats and `j + 4 <= N`.
            unsafe fn load4(a: *const f32, j: usize) -> Vec4 {
                // SAFETY: as the caller guarantees.
                unsafe { Vec4::new(load(a, j), load(a, j + 1), load(a, j + 2), load(a, j + 3)) }
            }

            /// # Safety
            ///
            /// `a` points to [`N`] floats, `acc` to one `u32` per thread.
            #[unsafe(export_name = concat!("bench_", stringify!($kernel)))]
            unsafe extern "ptx-kernel" fn bench(a: *const f32, _b: *const f32, _c: *const f32, acc: *mut u32, iters: u32) {
                let t = thread();
                let mut bits = 0_u32;
                for k in 0..iters as usize / 4 {
                    // SAFETY: a multiple of 4 below `N`.
                    let [r0, r1, r2, r3] = f(unsafe { load4(a, (4 * (t + k)) % N) }).to_array();
                    bits ^= r0.to_bits() ^ r1.to_bits() ^ r2.to_bits() ^ r3.to_bits();
                }
                // SAFETY: one `u32` per thread.
                unsafe { *acc.add(t) = bits };
            }

            /// # Safety
            ///
            /// `a` and `out1` point to [`N`] floats.
            #[unsafe(export_name = concat!("eval_", stringify!($kernel)))]
            unsafe extern "ptx-kernel" fn eval(a: *const f32, _b: *const f32, _c: *const f32, out1: *mut f32, _out2: *mut f32) {
                let t = thread();
                if t < N / 4 {
                    // SAFETY: `4t + 4 <= N`.
                    let r = f(unsafe { load4(a, 4 * t) }).to_array();
                    for (i, r) in r.into_iter().enumerate() {
                        // SAFETY: `4t + i < N`.
                        unsafe { *out1.add(4 * t + i) = r };
                    }
                }
            }
        }
    )*};
}

use umbra_math as m;

scalar! {
    sinf_libdevice: |a, b, c| (__nv_sinf(a), 0.0);
    sinf_umbra: |a, b, c| (m::sinf(a), 0.0);
    sinf_fast: |a, b, c| (__nv_fast_sinf(a), 0.0);
    cosf_libdevice: |a, b, c| (__nv_cosf(a), 0.0);
    cosf_fast: |a, b, c| (__nv_fast_cosf(a), 0.0);
    sincosf_libdevice: |a, b, c| sin_cos(ffi::__nv_sincosf, a);
    sincosf_umbra: |a, b, c| m::sin_cos(a);
    sincosf_fast: |a, b, c| sin_cos(ffi::__nv_fast_sincosf, a);
    tanf_libdevice: |a, b, c| (__nv_tanf(a), 0.0);
    tanf_umbra: |a, b, c| (m::tanf(a), 0.0);
    tanf_fast: |a, b, c| (__nv_fast_tanf(a), 0.0);
    asinf_libdevice: |a, b, c| (__nv_asinf(a), 0.0);
    asinf_umbra: |a, b, c| (m::asinf(a), 0.0);
    acosf_libdevice: |a, b, c| (__nv_acosf(a), 0.0);
    acosf_umbra: |a, b, c| (m::acosf(a), 0.0);
    atan2f_libdevice: |a, b, c| (__nv_atan2f(a, b), 0.0);
    atan2f_umbra: |a, b, c| (m::atan2(a, b), 0.0);
    coshf_libdevice: |a, b, c| (__nv_coshf(a), 0.0);
    coshf_umbra: |a, b, c| (m::coshf(a), 0.0);
    atanhf_libdevice: |a, b, c| (__nv_atanhf(a), 0.0);
    atanhf_umbra: |a, b, c| (m::atanhf(a), 0.0);
    expf_libdevice: |a, b, c| (__nv_expf(a), 0.0);
    expf_fast: |a, b, c| (__nv_fast_expf(a), 0.0);
    exp2f_libdevice: |a, b, c| (__nv_exp2f(a), 0.0);
    exp2f_fast: |a, b, c| (ex2_approx(a), 0.0);
    logf_libdevice: |a, b, c| (__nv_logf(a), 0.0);
    logf_umbra: |a, b, c| (m::logf(a), 0.0);
    logf_fast: |a, b, c| (__nv_fast_logf(a), 0.0);
    powf_libdevice: |a, b, c| (__nv_powf(a, b), 0.0);
    powf_fast: |a, b, c| (__nv_fast_powf(a, b), 0.0);
    erff_libdevice: |a, b, c| (__nv_erff(a), 0.0);
    erff_umbra: |a, b, c| (m::erf(a), 0.0);
    sqrtf_libdevice: |a, b, c| (__nv_fsqrt_rn(a), 0.0);
    sqrtf_umbra: |a, b, c| (m::sqrt(a), 0.0);
    sqrtf_fast: |a, b, c| (sqrt_approx(a), 0.0);
    fmaf_libdevice: |a, b, c| (__nv_fmaf(a, b, c), 0.0);
    fmaf_umbra: |a, b, c| (m::fma(a, b, c), 0.0);
    floorf_libdevice: |a, b, c| (__nv_floorf(a), 0.0);
    floorf_umbra: |a, b, c| (m::floor(a), 0.0);
    ceilf_libdevice: |a, b, c| (__nv_ceilf(a), 0.0);
    ceilf_umbra: |a, b, c| (m::ceil(a), 0.0);
    roundf_libdevice: |a, b, c| (__nv_roundf(a), 0.0);
    roundf_umbra: |a, b, c| (m::roundf(a), 0.0);
}

vec4! {
    expf_umbra: |v| v.exp();
    powf_umbra: |v| v.pow(GAMMA);
}
