//! Low-discrepancy sequences (pbrt-v4 `src/pbrt/util/lowdiscrepancy.h`,
//! `lowdiscrepancy.cpp`, `primes.cpp`): radical inverses in prime bases,
//! their randomisation by random digit permutations and by Owen scrambling,
//! which the `HaltonSampler` draws from.
//!
//! The core has no allocator (ADR 0014), so the digit permutations live in
//! storage their caller provides, as the piecewise-constant distributions
//! do: [`DigitPermutations::new_in`] takes an `alloc` that returns a buffer
//! of the length asked for.

use core::fmt;

use umbra_math::ONE_MINUS_EPSILON;

use crate::hash::{hash, mix_bits};

/// The number of primes in [`PRIMES`], and so of dimensions a radical
/// inverse can be computed in.
///
/// pbrt-v4 `PrimeTableSize`.
pub const PRIME_TABLE_SIZE: usize = 1000;

/// The first [`PRIME_TABLE_SIZE`] primes, the bases of the radical inverses.
///
/// pbrt-v4 `Primes`.
pub static PRIMES: [u32; PRIME_TABLE_SIZE] = primes();

/// The first [`PRIME_TABLE_SIZE`] primes, by trial division.
const fn primes() -> [u32; PRIME_TABLE_SIZE] {
    let mut primes = [0; PRIME_TABLE_SIZE];
    let mut count = 0;
    let mut candidate = 2;
    while count < PRIME_TABLE_SIZE {
        if is_prime(candidate, &primes, count) {
            primes[count] = candidate;
            count += 1;
        }
        candidate += 1;
    }
    primes
}

/// Whether no prime of the first `count` of `primes`, all those up to
/// `candidate`'s square root, divides `candidate`.
const fn is_prime(candidate: u32, primes: &[u32; PRIME_TABLE_SIZE], count: usize) -> bool {
    let mut i = 0;
    while i < count && primes[i] * primes[i] <= candidate {
        if candidate.is_multiple_of(primes[i]) {
            return false;
        }
        i += 1;
    }
    true
}

/// `n` as a float, as C++ converts an integer to `Float`: exact below 2²⁴.
#[expect(
    clippy::cast_precision_loss,
    reason = "as pbrt-v4: (Float)base, exact for the primes of the table"
)]
const fn float(n: u32) -> f32 {
    n as f32
}

/// `n` as a float, as C++ converts a `uint64_t` to `Float`: rounded to
/// nearest.
#[expect(
    clippy::cast_precision_loss,
    reason = "as pbrt-v4: reversedDigits * invBaseM converts the uint64_t to Float"
)]
const fn float_u64(n: u64) -> f32 {
    n as f32
}

/// The number of base-`base` digits a permutation randomises: those that
/// still change an `f32` in `[0, 1)`.
///
/// pbrt-v4 `DigitPermutation`'s constructor.
const fn n_digits(base: u32) -> usize {
    let inv_base = 1.0 / float(base);
    let mut inv_base_m = 1.0;
    let mut n = 0;
    while 1.0 - float(base - 1) * inv_base_m < 1.0 {
        n += 1;
        inv_base_m *= inv_base;
    }
    n
}

/// Where the permutations of each base start in the storage of
/// [`DigitPermutations`], and, last, its length.
static DIGIT_OFFSETS: [usize; PRIME_TABLE_SIZE + 1] = digit_offsets();

/// The prefix sums of `base · n_digits(base)` over the primes.
const fn digit_offsets() -> [usize; PRIME_TABLE_SIZE + 1] {
    let primes = primes();
    let mut offsets = [0; PRIME_TABLE_SIZE + 1];
    let mut i = 0;
    while i < PRIME_TABLE_SIZE {
        offsets[i + 1] = offsets[i] + primes[i] as usize * n_digits(primes[i]);
        i += 1;
    }
    offsets
}

/// The `i`th element of a random permutation of `0..l` chosen by `p`,
/// computed without storing the permutation (Kensler's hashing).
///
/// PBR 4e §B.2.8, pbrt-v4 `PermutationElement`.
#[must_use]
pub fn permutation_element(mut i: u32, l: u32, p: u32) -> u32 {
    debug_assert!(i < l, "permutation_element: {i} is not below {l}");
    let mut w = l - 1;
    w |= w >> 1;
    w |= w >> 2;
    w |= w >> 4;
    w |= w >> 8;
    w |= w >> 16;
    loop {
        i ^= p;
        i = i.wrapping_mul(0xe170_893d);
        i ^= p >> 16;
        i ^= (i & w) >> 4;
        i ^= p >> 8;
        i = i.wrapping_mul(0x0929_eb3f);
        i ^= p >> 23;
        i ^= (i & w) >> 1;
        i = i.wrapping_mul(1 | p >> 27);
        i = i.wrapping_mul(0x6935_fa69);
        i ^= (i & w) >> 11;
        i = i.wrapping_mul(0x74dc_b303);
        i ^= (i & w) >> 2;
        i = i.wrapping_mul(0x9e50_1cc3);
        i ^= (i & w) >> 2;
        i = i.wrapping_mul(0xc860_a3df);
        i &= w;
        i ^= i >> 5;
        if i < l {
            break;
        }
    }
    i.wrapping_add(p) % l
}

/// The prime of `base_index` and the bound on the reversed digits beyond
/// which one more digit could overflow them.
fn base_and_limit(base_index: usize) -> (u64, u64) {
    let base = u64::from(PRIMES[base_index]);
    (base, u64::MAX / base - base)
}

/// The base-`PRIMES[base_index]` digits of `a` mirrored about the radix
/// point: the `a`th point of the van der Corput sequence in that base.
///
/// # Panics
///
/// If `base_index` is not below [`PRIME_TABLE_SIZE`].
///
/// PBR 4e §8.6.1, pbrt-v4 `RadicalInverse`.
#[must_use]
pub fn radical_inverse(base_index: usize, mut a: u64) -> f32 {
    let (base, limit) = base_and_limit(base_index);
    let inv_base = 1.0 / float_u64(base);
    let mut inv_base_m = 1.0;
    let mut reversed_digits = 0_u64;
    while a != 0 && reversed_digits < limit {
        let next = a / base;
        let digit = a - next * base;
        reversed_digits = reversed_digits * base + digit;
        inv_base_m *= inv_base;
        a = next;
    }
    (float_u64(reversed_digits) * inv_base_m).min(ONE_MINUS_EPSILON)
}

/// The index whose `n_digits`-digit radical inverse in `base` is
/// `inverse / base^n_digits`: the inverse of [`radical_inverse`] on the
/// integer digits.
///
/// PBR 4e §8.6.1, pbrt-v4 `InverseRadicalInverse`.
#[must_use]
pub fn inverse_radical_inverse(mut inverse: u64, base: u32, n_digits: u32) -> u64 {
    let base = u64::from(base);
    let mut index = 0;
    for _ in 0..n_digits {
        let digit = inverse % base;
        inverse /= base;
        index = index * base + digit;
    }
    index
}

/// [`radical_inverse`] with each digit replaced by its image under `perm`,
/// the permutations of the same base.
///
/// # Panics
///
/// If `base_index` is not below [`PRIME_TABLE_SIZE`].
///
/// PBR 4e §8.6.2, pbrt-v4 `ScrambledRadicalInverse`.
#[must_use]
pub fn scrambled_radical_inverse(base_index: usize, mut a: u64, perm: DigitPermutation<'_>) -> f32 {
    let (base, limit) = base_and_limit(base_index);
    debug_assert_eq!(base, u64::from(perm.base), "a permutation of another base");
    let inv_base = 1.0 / float_u64(base);
    let mut inv_base_m = 1.0;
    let mut reversed_digits = 0_u64;
    let mut digit_index = 0;
    while 1.0 - float_u64(base - 1) * inv_base_m < 1.0 && reversed_digits < limit {
        let next = a / base;
        let digit_value = usize::try_from(a - next * base).expect("a digit is below its base");
        reversed_digits =
            reversed_digits * base + u64::from(perm.permute(digit_index, digit_value));
        inv_base_m *= inv_base;
        digit_index += 1;
        a = next;
    }
    (inv_base_m * float_u64(reversed_digits)).min(ONE_MINUS_EPSILON)
}

/// [`radical_inverse`] Owen-scrambled: each digit permuted by a permutation
/// chosen by `hash` and the digits above it.
///
/// # Panics
///
/// If `base_index` is not below [`PRIME_TABLE_SIZE`].
///
/// PBR 4e §8.6.3, pbrt-v4 `OwenScrambledRadicalInverse`.
#[must_use]
pub fn owen_scrambled_radical_inverse(base_index: usize, mut a: u64, hash: u32) -> f32 {
    let (base, limit) = base_and_limit(base_index);
    let base32 = PRIMES[base_index];
    let inv_base = 1.0 / float_u64(base);
    let mut inv_base_m = 1.0;
    let mut reversed_digits = 0_u64;
    while 1.0 - inv_base_m < 1.0 && reversed_digits < limit {
        let next = a / base;
        let digit_value = u32::try_from(a - next * base).expect("a digit is below its base");
        #[expect(
            clippy::cast_possible_truncation,
            reason = "as pbrt-v4: uint32_t digitHash = MixBits(...)"
        )]
        let digit_hash = mix_bits(u64::from(hash) ^ reversed_digits) as u32;
        let digit_value = permutation_element(digit_value, base32, digit_hash);
        reversed_digits = reversed_digits * base + u64::from(digit_value);
        inv_base_m *= inv_base;
        a = next;
    }
    (inv_base_m * float_u64(reversed_digits)).min(ONE_MINUS_EPSILON)
}

/// The random permutations of the digits of one base, one per digit
/// position, borrowed from [`DigitPermutations`].
///
/// PBR 4e §8.6.2, pbrt-v4 `DigitPermutation`.
#[derive(Debug, Clone, Copy)]
pub struct DigitPermutation<'a> {
    base: u32,
    permutations: &'a [u16],
}

impl DigitPermutation<'_> {
    /// The image of `digit_value` under the permutation of position
    /// `digit_index`.
    ///
    /// # Panics
    ///
    /// If `digit_index` is past the last digit position of the base.
    ///
    /// pbrt-v4 `DigitPermutation::Permute`.
    #[inline]
    #[must_use]
    pub fn permute(self, digit_index: usize, digit_value: usize) -> u16 {
        let base = self.base as usize;
        debug_assert!(digit_value < base, "digit {digit_value} of base {base}");
        self.permutations[digit_index * base + digit_value]
    }
}

/// The digit permutations of every base of [`PRIMES`], chosen by a seed,
/// in one storage `S` of `u16`s, every base's permutations one after the
/// other.
///
/// PBR 4e §8.6.2, pbrt-v4 `ComputeRadicalInversePermutations`.
// `repr(C)` for every `S`: the GPU Worker copies it to the device byte for
// byte, with its storage in device memory (ADR 0028, arc42 §8.12).
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(C)]
pub struct DigitPermutations<S> {
    storage: S,
}

// The GPU Worker copies the permutations' handle to the device byte for
// byte, their storage in device memory (ADR 0028, arc42 §8.12).
const _: () = {
    // The storage as the GPU Worker's: a device address and a length, 16
    // bytes aligned to 8 (`umbra-gpu-programs`' `DeviceSlice`).
    type DeviceStorage = [u64; 2];

    assert!(
        size_of::<DigitPermutations<DeviceStorage>>() == 16,
        "`DigitPermutations` is its storage"
    );
};

impl<S: AsRef<[u16]>> fmt::Debug for DigitPermutations<S> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("DigitPermutations")
            .field("len", &self.storage.as_ref().len())
            .finish_non_exhaustive()
    }
}

impl<S> DigitPermutations<S> {
    /// The number of `u16`s the permutations take.
    pub const LEN: usize = DIGIT_OFFSETS[PRIME_TABLE_SIZE];

    /// The same permutations, their storage moved by `f`: to device memory,
    /// for the GPU Worker (ADR 0028).
    #[must_use]
    pub fn map_storage<T>(self, f: impl FnOnce(S) -> T) -> DigitPermutations<T> {
        DigitPermutations {
            storage: f(self.storage),
        }
    }
}

impl<S: AsRef<[u16]> + AsMut<[u16]>> DigitPermutations<S> {
    /// The permutations seeded by `seed`, in storage of [`Self::LEN`]
    /// values that `alloc` returns.
    ///
    /// # Panics
    ///
    /// If `alloc` returns storage of another length.
    ///
    /// pbrt-v4 `ComputeRadicalInversePermutations(seed)`.
    pub fn new_in(seed: u32, alloc: impl FnOnce(usize) -> S) -> Self {
        let mut storage = alloc(Self::LEN);
        let values = storage.as_mut();
        assert_eq!(values.len(), Self::LEN, "storage of the wrong length");
        for (base_index, &base) in PRIMES.iter().enumerate() {
            let range = DIGIT_OFFSETS[base_index]..DIGIT_OFFSETS[base_index + 1];
            fill_permutations(&mut values[range], base, seed);
        }
        Self { storage }
    }
}

/// Fills `out` with the permutations of `base`'s digits, one row of `base`
/// values per digit position.
///
/// pbrt-v4 `DigitPermutation(base, seed)`.
fn fill_permutations(out: &mut [u16], base: u32, seed: u32) {
    let base_len = usize::try_from(base).expect("a prime fits usize");
    let rows = out.chunks_exact_mut(base_len).zip(0_i32..);
    for (row, digit_index) in rows {
        let base_int = i32::try_from(base).expect("a prime of the table fits i32");
        let digit_seed = hash((base_int, digit_index, seed));
        for (value, digit_value) in row.iter_mut().zip(0..) {
            #[expect(
                clippy::cast_possible_truncation,
                reason = "as pbrt-v4: PermutationElement's uint32_t seed from the uint64_t hash"
            )]
            let element = permutation_element(digit_value, base, digit_seed as u32);
            *value = u16::try_from(element).expect("a prime of the table is below 2^16");
        }
    }
}

impl<S: AsRef<[u16]>> DigitPermutations<S> {
    /// The permutations of the digits of base `PRIMES[base_index]`.
    ///
    /// # Panics
    ///
    /// If `base_index` is not below [`PRIME_TABLE_SIZE`].
    #[inline]
    #[must_use]
    pub fn get(&self, base_index: usize) -> DigitPermutation<'_> {
        let range = DIGIT_OFFSETS[base_index]..DIGIT_OFFSETS[base_index + 1];
        DigitPermutation {
            base: PRIMES[base_index],
            permutations: &self.storage.as_ref()[range],
        }
    }

    /// The same permutations, borrowed: what a sampler holds.
    #[inline]
    #[must_use]
    pub fn view(&self) -> DigitPermutations<&[u16]> {
        DigitPermutations {
            storage: self.storage.as_ref(),
        }
    }
}
