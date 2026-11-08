//! pbrt-v4's sample generators (`src/pbrt/util/sampling.h`), which its tests
//! draw their inputs from. Test support only: the renderer draws through its
//! samplers.

use umbra_sampling::Rng;
use umbra_spectrum::RGB;

/// pbrt-v4 `Stratified1D(n)`: stratum `i` of `n`, jittered by `RNG(0)`.
pub(crate) fn stratified_1d(n: u16) -> impl Iterator<Item = f32> {
    let mut rng = Rng::new(0);
    (0..n).map(move |i| (f32::from(i) + rng.uniform::<f32>()) / f32::from(n))
}

/// `n` colours from pbrt-v4's default `RNG`, red drawn first, each
/// component scaled by `scale` and offset by `offset`.
pub(crate) fn colors(n: usize, scale: f32, offset: f32) -> impl Iterator<Item = RGB> {
    let mut rng = Rng::default();
    let mut component = move || offset + scale * rng.uniform::<f32>();
    (0..n).map(move |_| {
        let r = component();
        let g = component();
        RGB::new(r, g, component())
    })
}
