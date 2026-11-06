//! pbrt-v4's `RNG` (PCG32, `src/pbrt/util/rng.h`), so ported tests draw the
//! same values as the originals. Test support only: `umbra-sampling` owns the
//! renderer's RNG.

const PCG32_DEFAULT_STATE: u64 = 0x853c_49e6_748f_ea9b;
const PCG32_DEFAULT_STREAM: u64 = 0xda3e_39cb_94b9_5bdb;
const PCG32_MULT: u64 = 0x5851_f42d_4c95_7f2d;

/// pbrt-v4 `RNG`.
#[derive(Debug, Clone)]
pub(crate) struct Rng {
    state: u64,
    inc: u64,
}

impl Default for Rng {
    fn default() -> Self {
        Self {
            state: PCG32_DEFAULT_STATE,
            inc: PCG32_DEFAULT_STREAM,
        }
    }
}

/// pbrt-v4 `MixBits`.
fn mix_bits(mut v: u64) -> u64 {
    v ^= v >> 31;
    v = v.wrapping_mul(0x7fb5_d329_728e_a185);
    v ^= v >> 27;
    v = v.wrapping_mul(0x81da_def4_bc2d_d44d);
    v ^= v >> 33;
    v
}

impl Rng {
    /// pbrt-v4 `RNG(seqIndex)`.
    pub(crate) fn new(sequence_index: u64) -> Self {
        let mut rng = Self {
            state: 0,
            inc: (sequence_index << 1) | 1,
        };
        rng.uniform_u32();
        rng.state = rng.state.wrapping_add(mix_bits(sequence_index));
        rng.uniform_u32();
        rng
    }

    /// pbrt-v4 `Uniform<uint32_t>()`.
    pub(crate) fn uniform_u32(&mut self) -> u32 {
        let old = self.state;
        self.state = old.wrapping_mul(PCG32_MULT).wrapping_add(self.inc);
        #[expect(clippy::cast_possible_truncation, reason = "PCG32 output permutation")]
        let xorshifted = (((old >> 18) ^ old) >> 27) as u32;
        let rot = (old >> 59) as u32;
        xorshifted.rotate_right(rot)
    }

    /// pbrt-v4 `Uniform<uint32_t>(b)`: uniform in `[0, b)`.
    pub(crate) fn uniform_u32_below(&mut self, b: u32) -> u32 {
        let threshold = b.wrapping_neg() % b;
        core::iter::repeat_with(|| self.uniform_u32())
            .find(|&r| r >= threshold)
            .map_or(0, |r| r % b)
    }

    /// pbrt-v4 `Uniform<float>()`: uniform in `[0, 1)`.
    pub(crate) fn uniform_f32(&mut self) -> f32 {
        #[expect(clippy::cast_precision_loss, reason = "as pbrt-v4: u32 * 2^-32")]
        let u = self.uniform_u32() as f32 * f32::from_bits(0x2f80_0000);
        umbra_math::ONE_MINUS_EPSILON.min(u)
    }
}
