//! `samplers_test.cpp`: `TEST(Sampler, ConsistentValues)` for the Halton
//! sampler, the only one ported so far. The other tests there check
//! elementary intervals of the Sobol and PMJ samplers; pbrt-v4 has none for
//! Halton. Then the `known_values` tests of the low-discrepancy routines and
//! of `HaltonSampler` against `halton_values.rs`.

use umbra_math::{Point2f, Point2i};
use umbra_sampling::{
    DigitPermutations, HaltonRandomizer, HaltonSampler, PRIMES, Sampler, SamplerKind,
    inverse_radical_inverse, owen_scrambled_radical_inverse, permutation_element, radical_inverse,
    scrambled_radical_inverse,
};

use super::halton_values::{
    HALTON_DIMENSION_WRAP, HALTON_NONE, HALTON_OWEN, HALTON_PERMUTE_DIGITS,
    HALTON_PERMUTE_DIGITS_SEED_7, INVERSE_RADICAL_INVERSE, OWEN_SCRAMBLED_RADICAL_INVERSE,
    PERMUTATION_ELEMENT, PRIMES as PBRT_PRIMES, RADICAL_INVERSE, SCRAMBLED_RADICAL_INVERSE,
};

/// pbrt-v4's digit permutations for `seed`, in a `Vec`.
fn permutations(seed: u32) -> DigitPermutations<Vec<u16>> {
    DigitPermutations::new_in(seed, |n| vec![0; n])
}

#[test]
fn consistent_values() {
    const SPP: u32 = 16;
    let permutations = permutations(0);
    let mut sampler = SamplerKind::from(HaltonSampler::new(
        SPP,
        Point2i::new(100, 101),
        HaltonRandomizer::PermuteDigits(permutations.view()),
    ));
    let draw = |sampler: &mut SamplerKind<'_>, s: u32| {
        sampler.start_pixel_sample(Point2i::new(1, 5), s, 0);
        (0..10)
            .map(|_| (sampler.get_2d(), sampler.get_1d()))
            .collect::<Vec<(Point2f, f32)>>()
    };
    let first: Vec<_> = (0..SPP).map(|s| draw(&mut sampler, s)).collect();

    sampler.start_pixel_sample(Point2i::new(0, 6), 10, 0);
    let _ = (sampler.get_2d(), sampler.get_2d(), sampler.get_1d());

    for s in (0..SPP).rev() {
        assert_eq!(
            draw(&mut sampler, s),
            first[s as usize],
            "Pixel Sample {s} again"
        );
    }
}

#[test]
fn known_values_primes() {
    for &(i, prime) in PBRT_PRIMES {
        assert_eq!(PRIMES[i], prime, "Primes[{i}]");
    }
}

#[test]
fn known_values_radical_inverse() {
    for &(base_index, a, bits) in RADICAL_INVERSE {
        assert_eq!(
            radical_inverse(base_index, a).to_bits(),
            bits,
            "RadicalInverse({base_index}, {a:#x})"
        );
    }
    for &(inverse, base, n_digits, index) in INVERSE_RADICAL_INVERSE {
        assert_eq!(
            inverse_radical_inverse(inverse, base, n_digits),
            index,
            "InverseRadicalInverse({inverse}, {base}, {n_digits})"
        );
    }
}

#[test]
fn known_values_permutation_element() {
    for &(i, l, p, element) in PERMUTATION_ELEMENT {
        assert_eq!(
            permutation_element(i, l, p),
            element,
            "PermutationElement({i}, {l}, {p:#x})"
        );
    }
}

#[test]
fn known_values_owen_scrambled_radical_inverse() {
    for &(base_index, a, hash, bits) in OWEN_SCRAMBLED_RADICAL_INVERSE {
        assert_eq!(
            owen_scrambled_radical_inverse(base_index, a, hash).to_bits(),
            bits,
            "OwenScrambledRadicalInverse({base_index}, {a:#x}, {hash:#x})"
        );
    }
}

#[test]
fn known_values_scrambled_radical_inverse() {
    let mut cached: Option<(u32, DigitPermutations<Vec<u16>>)> = None;
    for &(seed, base_index, a, bits) in SCRAMBLED_RADICAL_INVERSE {
        if cached.as_ref().is_none_or(|(s, _)| *s != seed) {
            cached = Some((seed, permutations(seed)));
        }
        let (_, perms) = cached.as_ref().expect("computed above");
        assert_eq!(
            scrambled_radical_inverse(base_index, a, perms.get(base_index)).to_bits(),
            bits,
            "ScrambledRadicalInverse({base_index}, {a:#x}) with seed {seed:#x}"
        );
    }
}

/// One Pixel Sample's draws, as `halton_values.cpp` makes them.
fn draws(sampler: &mut SamplerKind<'_>, pixel: Point2i, sample_index: u32) -> [u32; 10] {
    sampler.start_pixel_sample(pixel, sample_index, 0);
    let wavelengths = sampler.get_1d();
    let filter = sampler.get_pixel_2d();
    let time = sampler.get_1d();
    let lens = sampler.get_2d();
    let a = sampler.get_1d();
    let b = sampler.get_2d();
    let c = sampler.get_1d();
    [
        wavelengths,
        filter.x,
        filter.y,
        time,
        lens.x,
        lens.y,
        a,
        b.x,
        b.y,
        c,
    ]
    .map(f32::to_bits)
}

/// Checks `sampler` against the table pbrt-v4 computed with the same
/// arguments.
#[track_caller]
fn check_halton(mut sampler: SamplerKind<'_>, expected: &[(i32, i32, u32, [u32; 10])]) {
    assert_eq!(sampler.samples_per_pixel(), 16, "samples per pixel");
    for &(x, y, sample_index, bits) in expected {
        assert_eq!(
            draws(&mut sampler, Point2i::new(x, y), sample_index),
            bits,
            "pixel ({x}, {y}), Pixel Sample {sample_index}"
        );
    }
}

#[test]
fn known_values_halton_none() {
    let sampler = HaltonSampler::new(16, Point2i::new(64, 48), HaltonRandomizer::None);
    check_halton(sampler.into(), HALTON_NONE);
}

#[test]
fn known_values_halton_permute_digits() {
    let perms = permutations(0);
    let randomizer = HaltonRandomizer::PermuteDigits(perms.view());
    let sampler = HaltonSampler::new(16, Point2i::new(1280, 720), randomizer);
    check_halton(sampler.into(), HALTON_PERMUTE_DIGITS);

    let mut sampler = SamplerKind::from(HaltonSampler::new(16, Point2i::new(8, 8), randomizer));
    sampler.start_pixel_sample(Point2i::new(3, 4), 2, 0);
    let wrapped: Vec<u32> = (0..1100)
        .map(|_| sampler.get_1d())
        .skip(99)
        .step_by(100)
        .map(f32::to_bits)
        .collect();
    assert_eq!(wrapped, HALTON_DIMENSION_WRAP, "draws past the last prime");
}

#[test]
fn known_values_halton_permute_digits_seed() {
    let perms = permutations(7);
    let randomizer = HaltonRandomizer::PermuteDigits(perms.view());
    let sampler = HaltonSampler::new(16, Point2i::new(16, 16), randomizer);
    check_halton(sampler.into(), HALTON_PERMUTE_DIGITS_SEED_7);
}

#[test]
fn known_values_halton_owen() {
    let sampler = HaltonSampler::new(16, Point2i::new(200, 1), HaltonRandomizer::Owen);
    check_halton(sampler.into(), HALTON_OWEN);
}
