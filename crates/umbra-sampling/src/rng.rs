//! pbrt-v4's random number generator, O'Neill's PCG32 (pbrt-v4
//! `src/pbrt/util/rng.h`).

use core::ops::Sub;

use umbra_math::ONE_MINUS_EPSILON;

use crate::hash::{TWO_TO_MINUS_32, mix_bits};

const PCG32_DEFAULT_STATE: u64 = 0x853c_49e6_748f_ea9b;
const PCG32_DEFAULT_STREAM: u64 = 0xda3e_39cb_94b9_5bdb;
const PCG32_MULT: u64 = 0x5851_f42d_4c95_7f2d;

/// PCG32: a 64-bit linear congruential state and a 32-bit output
/// permutation, with 2^63 independent streams (sequences) of period 2^64.
///
/// It is `Clone` but not `Copy`, so a stream is never duplicated by
/// accident.
///
/// PBR 4e §B.2.8, pbrt-v4 `RNG`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Rng {
    state: u64,
    inc: u64,
}

impl Default for Rng {
    /// pbrt-v4 `RNG()`: PCG32's default state and stream.
    fn default() -> Self {
        Self {
            state: PCG32_DEFAULT_STATE,
            inc: PCG32_DEFAULT_STREAM,
        }
    }
}

impl Rng {
    /// The start of sequence `sequence_index`, offset by
    /// [`mix_bits`]`(sequence_index)`.
    ///
    /// pbrt-v4 `RNG(seqIndex)`.
    #[must_use]
    pub fn new(sequence_index: u64) -> Self {
        Self::with_offset(sequence_index, mix_bits(sequence_index))
    }

    /// Sequence `sequence_index`, started at `offset`.
    ///
    /// pbrt-v4 `RNG(seqIndex, offset)`.
    #[must_use]
    pub fn with_offset(sequence_index: u64, offset: u64) -> Self {
        let mut rng = Self { state: 0, inc: 0 };
        rng.set_sequence_with_offset(sequence_index, offset);
        rng
    }

    /// Restarts at sequence `sequence_index`, as [`Rng::new`].
    ///
    /// pbrt-v4 `SetSequence(sequenceIndex)`.
    pub fn set_sequence(&mut self, sequence_index: u64) {
        self.set_sequence_with_offset(sequence_index, mix_bits(sequence_index));
    }

    /// Restarts at sequence `sequence_index`, offset by `offset`, as
    /// [`Rng::with_offset`].
    ///
    /// pbrt-v4 `SetSequence(sequenceIndex, offset)`.
    pub fn set_sequence_with_offset(&mut self, sequence_index: u64, offset: u64) {
        self.state = 0;
        self.inc = (sequence_index << 1) | 1;
        self.next_u32();
        self.state = self.state.wrapping_add(offset);
        self.next_u32();
    }

    /// A uniformly distributed value of type `T`: any integer of its range,
    /// or an `f32` in `[0, 1)`.
    ///
    /// pbrt-v4 `Uniform<T>()`.
    #[inline]
    pub fn uniform<T: Uniform>(&mut self) -> T {
        T::uniform(self)
    }

    /// A uniformly distributed integer in `[0, b)`, without modulo bias.
    ///
    /// # Panics
    ///
    /// If `b` is zero.
    ///
    /// pbrt-v4 `Uniform<T>(T b)`.
    #[inline]
    pub fn uniform_below<T: UniformBelow>(&mut self, b: T) -> T {
        T::uniform_below(self, b)
    }

    /// Skips `delta` values ahead, or back if negative, in `O(log |delta|)`
    /// (Brown, "Random number generation with arbitrary strides").
    ///
    /// pbrt-v4 `Advance`.
    pub fn advance(&mut self, delta: i64) {
        self.state = jump(self.step(), delta.cast_unsigned()).apply(self.state);
    }

    /// The state transition of one draw.
    fn step(&self) -> Affine {
        Affine {
            mult: PCG32_MULT,
            plus: self.inc,
        }
    }

    /// The next 32 bits of the stream.
    ///
    /// pbrt-v4 `Uniform<uint32_t>()`.
    #[inline]
    fn next_u32(&mut self) -> u32 {
        let old = self.state;
        self.state = old.wrapping_mul(PCG32_MULT).wrapping_add(self.inc);
        #[expect(
            clippy::cast_possible_truncation,
            reason = "PCG32's output permutation"
        )]
        let (xorshifted, rot) = ((((old >> 18) ^ old) >> 27) as u32, (old >> 59) as u32);
        xorshifted.rotate_right(rot)
    }
}

impl Sub for &Rng {
    type Output = i64;

    /// How many values `rhs` must draw to reach `self`'s state.
    ///
    /// # Panics
    ///
    /// If the two are on different sequences.
    ///
    /// pbrt-v4 `RNG::operator-`.
    fn sub(self, rhs: Self) -> i64 {
        assert_eq!(self.inc, rhs.inc, "the RNGs are on different sequences");
        distance(self.step(), rhs.state, self.state).cast_signed()
    }
}

/// The affine map `state ↦ mult · state + plus` modulo 2⁶⁴ that one or more
/// PCG32 draws apply to the state.
#[derive(Clone, Copy)]
struct Affine {
    mult: u64,
    plus: u64,
}

impl Affine {
    /// The map applied to `state`.
    fn apply(self, state: u64) -> u64 {
        self.mult.wrapping_mul(state).wrapping_add(self.plus)
    }

    /// The map applied twice, with pbrt-v4's operations.
    fn squared(self) -> Self {
        Self {
            mult: self.mult.wrapping_mul(self.mult),
            plus: self.mult.wrapping_add(1).wrapping_mul(self.plus),
        }
    }
}

/// `step` applied `n` times, by squaring (pbrt-v4 `RNG::Advance`).
fn jump(step: Affine, mut n: u64) -> Affine {
    let mut cur = step;
    let mut acc = Affine { mult: 1, plus: 0 };
    while n > 0 {
        if n & 1 != 0 {
            acc = Affine {
                mult: acc.mult.wrapping_mul(cur.mult),
                plus: cur.apply(acc.plus),
            };
        }
        cur = cur.squared();
        n /= 2;
    }
    acc
}

/// How many times `step` takes `from` to `to`, found bit by bit from the
/// lowest (pbrt-v4 `RNG::operator-`).
fn distance(step: Affine, from: u64, to: u64) -> u64 {
    let (mut cur, mut state) = (step, from);
    let (mut the_bit, mut distance) = (1_u64, 0_u64);
    while state != to {
        if (state ^ to) & the_bit != 0 {
            state = cur.apply(state);
            distance |= the_bit;
        }
        the_bit <<= 1;
        cur = cur.squared();
    }
    distance
}

/// A type [`Rng::uniform`] draws.
pub trait Uniform: Sized {
    /// A uniformly distributed value drawn from `rng`.
    fn uniform(rng: &mut Rng) -> Self;
}

impl Uniform for u32 {
    #[inline]
    fn uniform(rng: &mut Rng) -> Self {
        rng.next_u32()
    }
}

impl Uniform for u64 {
    /// Two draws, the first one the high half.
    #[inline]
    fn uniform(rng: &mut Rng) -> Self {
        let high = u64::from(rng.next_u32());
        (high << 32) | u64::from(rng.next_u32())
    }
}

impl Uniform for i32 {
    /// The bits of a `u32` draw, in two's complement.
    #[inline]
    fn uniform(rng: &mut Rng) -> Self {
        rng.next_u32().cast_signed()
    }
}

impl Uniform for i64 {
    /// The bits of a `u64` draw, in two's complement.
    #[inline]
    fn uniform(rng: &mut Rng) -> Self {
        u64::uniform(rng).cast_signed()
    }
}

impl Uniform for f32 {
    /// A `u32` draw times 2^-32, in `[0, 1)`.
    #[inline]
    fn uniform(rng: &mut Rng) -> Self {
        #[expect(clippy::cast_precision_loss, reason = "as pbrt-v4: u32 * 0x1p-32f")]
        let u = rng.next_u32() as f32 * TWO_TO_MINUS_32;
        umbra_math::min(ONE_MINUS_EPSILON, u)
    }
}

/// An unsigned integer type [`Rng::uniform_below`] draws.
pub trait UniformBelow: Uniform {
    /// A uniformly distributed value in `[0, b)` drawn from `rng`.
    fn uniform_below(rng: &mut Rng, b: Self) -> Self;
}

/// [`UniformBelow`] by rejection of the values below `2^n mod b`.
macro_rules! uniform_below {
    ($($t:ty),+) => {$(
        impl UniformBelow for $t {
            #[inline]
            fn uniform_below(rng: &mut Rng, b: Self) -> Self {
                let threshold = b.wrapping_neg() % b;
                loop {
                    let r = Self::uniform(rng);
                    if r >= threshold {
                        return r % b;
                    }
                }
            }
        }
    )+};
}

uniform_below!(u32, u64);
