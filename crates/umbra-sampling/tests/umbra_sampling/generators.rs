//! pbrt-v4's sample generators (`src/pbrt/util/sampling.h`), which its tests
//! draw their inputs from. Test support only: the renderer draws through its
//! samplers.

use umbra_math::Point2f;
use umbra_sampling::Rng;

/// pbrt-v4 `Uniform1D(n, sequenceIndex)`: `n` floats from
/// `RNG(sequenceIndex)`. pbrt-v4's `seed` argument is unused, so it is
/// not here.
pub(crate) fn uniform_1d(n: usize, sequence_index: u64) -> impl Iterator<Item = f32> {
    let mut rng = Rng::new(sequence_index);
    (0..n).map(move |_| rng.uniform())
}

/// pbrt-v4 `Uniform2D(n, sequenceIndex)`: `n` points from
/// `RNG(sequenceIndex)`, x drawn before y.
pub(crate) fn uniform_2d(n: usize, sequence_index: u64) -> impl Iterator<Item = Point2f> {
    let mut rng = Rng::new(sequence_index);
    (0..n).map(move |_| {
        let x = rng.uniform();
        Point2f::new(x, rng.uniform())
    })
}

/// pbrt-v4 `RadicalInverse(baseIndex, a)` for the first two primes, 2 and 3.
pub(crate) fn radical_inverse(base_index: usize, mut a: u64) -> f32 {
    let base: u64 = [2, 3][base_index];
    let limit = !0 / base - base;
    #[expect(clippy::cast_precision_loss, reason = "a small prime")]
    let inv_base = 1.0 / base as f32;
    let mut inv_base_m = 1.0_f32;
    let mut reversed_digits = 0_u64;
    while a != 0 && reversed_digits < limit {
        let next = a / base;
        reversed_digits = reversed_digits * base + (a - next * base);
        inv_base_m *= inv_base;
        a = next;
    }
    #[expect(clippy::cast_precision_loss, reason = "as pbrt-v4: uint64_t * Float")]
    umbra_math::min(
        reversed_digits as f32 * inv_base_m,
        umbra_math::ONE_MINUS_EPSILON,
    )
}
