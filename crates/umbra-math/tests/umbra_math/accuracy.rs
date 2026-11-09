//! Umbra's own transcendentals against `libm`'s `f64` routines (ADR 0022):
//! each result must be within its documented bound, [`BOUND`] ULPs, of the
//! `f64` reference rounded to `f32`, with the reference's sign, and a NaN
//! exactly where the reference has one.
//!
//! The unary functions are swept over every [`SWEEP`]-th bit pattern on
//! every run and over all 2³² on demand:
//! `cargo nextest run -p umbra-math --run-ignored all -E 'test(exhaustive)'`.
//! The binary ones (`atan2`, `pow`) take random pairs, dense sweeps of one
//! argument and every pair of special values.

mod scalar;
mod simd;

use std::num::NonZero;
use std::ops::Range;
use std::thread;

pub(crate) use crate::equivalence::{COUNT, SPECIAL, STRIDE, samples};

/// The stride of the sweeps over every `f32`: [`STRIDE`] natively; under
/// Miri, which is there for the `unsafe` code rather than the accuracy and
/// interprets `libm`'s software floats too, a few hundred values.
pub(crate) const SWEEP: u32 = if cfg!(miri) { 1 << 24 } else { STRIDE };

/// The error bound of every kernel here, in ULPs.
pub(crate) const BOUND: u32 = 1;

/// `v` on a line where neighbouring `f32`s of one sign are one apart.
fn ordinal(v: f32) -> i64 {
    i64::from(v.to_bits() & 0x7fff_ffff)
}

/// The error of `ours` against `reference` in ULPs: `u32::MAX` where one is
/// a NaN and the other is not, or where their signs differ.
pub(crate) fn ulps(ours: f32, reference: f32) -> u32 {
    match (ours.is_nan(), reference.is_nan()) {
        (true, true) => 0,
        (false, false) if ours.is_sign_negative() == reference.is_sign_negative() => {
            u32::try_from((ordinal(ours) - ordinal(reference)).unsigned_abs()).unwrap_or(u32::MAX)
        }
        _ => u32::MAX,
    }
}

/// The `f64` value rounded to the nearest `f32`.
#[expect(clippy::cast_possible_truncation, reason = "the reference rounding")]
pub(crate) fn narrow(x: f64) -> f32 {
    x as f32
}

/// The largest `error(x)` over the `f32`s whose bits are `i · stride` for
/// `i` in `range`, with an argument where it occurs.
fn worst_in(
    range: Range<u64>,
    stride: u32,
    error: &(impl Fn(f32) -> u32 + Sync),
) -> Option<(u32, f32)> {
    range
        .map(|i| f32::from_bits(u32::try_from(i * u64::from(stride)).expect("below 2³²")))
        .map(|x| (error(x), x))
        .max_by_key(|&(e, _)| e)
}

/// The largest `error(x)` over the `f32`s whose bits are multiples of
/// `stride`, with an argument where it occurs; on every thread.
pub(crate) fn worst_over_f32(stride: u32, error: impl Fn(f32) -> u32 + Sync) -> (u32, f32) {
    let count = u64::from(u32::MAX / stride) + 1;
    let threads = thread::available_parallelism().map_or(1, NonZero::get) as u64;
    let chunk = count.div_ceil(threads);
    let error = &error;
    thread::scope(|s| {
        #[expect(
            clippy::needless_collect,
            reason = "every thread is spawned before any is joined"
        )]
        let handles: Vec<_> = (0..threads)
            .map(|t| {
                s.spawn(move || worst_in(t * chunk..((t + 1) * chunk).min(count), stride, error))
            })
            .collect();
        handles
            .into_iter()
            .filter_map(|h| h.join().expect("a sweep thread panicked"))
            .max_by_key(|&(e, _)| e)
            .unwrap_or((0, 0.0))
    })
}

/// Asserts that a sweep found no error above [`BOUND`].
#[track_caller]
pub(crate) fn assert_within((worst, x): (u32, f32), what: &str) {
    assert!(
        worst <= BOUND,
        "{what}: {worst} ULPs at {x:e} ({:#010x})",
        x.to_bits()
    );
}
