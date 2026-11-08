//! Hashing (pbrt-v4 `src/pbrt/util/hash.h`): the bit mixer and the byte hash
//! that seed every sampler and RNG, so a seed gives pbrt-v4's sequences
//! (ADR 0004).

use umbra_math::{Normal3f, Point2f, Point2i, Point3f, Point3i, Vector2f, Vector3f};

/// A 64-bit finaliser that spreads every input bit over the output (Stafford's
/// "Mix13").
///
/// PBR 4e §B.2.7, pbrt-v4 `MixBits`.
#[inline]
#[must_use]
pub fn mix_bits(mut v: u64) -> u64 {
    v ^= v >> 31;
    v = v.wrapping_mul(0x7fb5_d329_728e_a185);
    v ^= v >> 27;
    v = v.wrapping_mul(0x81da_def4_bc2d_d44d);
    v ^= v >> 33;
    v
}

/// Austin Appleby's 64-bit `MurmurHash2` of `key`, reading it as little-endian
/// 64-bit words as pbrt-v4 does on every target Umbra supports.
///
/// PBR 4e §B.2.7, pbrt-v4 `MurmurHash64A`.
#[must_use]
pub fn murmur_hash_64a(key: &[u8], seed: u64) -> u64 {
    const M: u64 = 0xc6a4_a793_5bd1_e995;
    const R: u32 = 47;

    let (words, tail) = key.as_chunks::<8>();
    let mut h = words
        .iter()
        .fold(seed ^ (key.len() as u64).wrapping_mul(M), |h, word| {
            let mut k = u64::from_le_bytes(*word).wrapping_mul(M);
            k ^= k >> R;
            (h ^ k.wrapping_mul(M)).wrapping_mul(M)
        });
    if !tail.is_empty() {
        h ^= tail
            .iter()
            .rev()
            .fold(0, |acc, &byte| (acc << 8) | u64::from(byte));
        h = h.wrapping_mul(M);
    }
    h ^= h >> R;
    h = h.wrapping_mul(M);
    h ^ (h >> R)
}

/// 2⁻³², which maps a `u32` to `[0, 1)` (pbrt-v4's `0x1p-32f`).
pub(crate) const TWO_TO_MINUS_32: f32 = f32::from_bits(0x2f80_0000);

/// The largest argument list [`hash`] takes, in bytes.
const MAX_HASH_BYTES: usize = 64;

/// A value, or a tuple of values, whose bytes [`hash`] hashes: the bytes
/// pbrt-v4's variadic `Hash` copies, the arguments one after the other with
/// no padding, each in its C++ layout.
///
/// The width of each argument matters, as it does in pbrt-v4: hash an `i32`
/// where pbrt-v4 hashes an `int`, a `u64` where it hashes a `uint64_t`.
pub trait Hashable: Copy {
    /// The number of bytes.
    const SIZE: usize;
    /// Writes the bytes to `out`, which is exactly [`Self::SIZE`] long.
    fn write_bytes(self, out: &mut [u8]);
}

/// [`Hashable`] for a scalar, as its little-endian bytes.
macro_rules! hashable_scalar {
    ($($t:ty),+) => {$(
        impl Hashable for $t {
            const SIZE: usize = size_of::<$t>();
            #[inline]
            fn write_bytes(self, out: &mut [u8]) {
                out.copy_from_slice(&self.to_le_bytes());
            }
        }
    )+};
}

hashable_scalar!(i32, u32, i64, u64, f32);

/// [`Hashable`] for a tuple type of pbrt-v4, as its components in order.
macro_rules! hashable_tuple {
    ($($t:ty: $n:literal * $c:ty, $($f:ident),+;)+) => {$(
        impl Hashable for $t {
            const SIZE: usize = $n * size_of::<$c>();
            #[inline]
            fn write_bytes(self, out: &mut [u8]) {
                ($(self.$f),+).write_bytes(out);
            }
        }
    )+};
}

hashable_tuple! {
    Point2i: 2 * i32, x, y;
    Point3i: 3 * i32, x, y, z;
    Point2f: 2 * f32, x, y;
    Point3f: 3 * f32, x, y, z;
    Vector2f: 2 * f32, x, y;
    Vector3f: 3 * f32, x, y, z;
    Normal3f: 3 * f32, x, y, z;
}

/// [`Hashable`] for an argument list: each argument's bytes, in order.
macro_rules! hashable_args {
    ($(($($a:ident),+))+) => {$(
        impl<$($a: Hashable),+> Hashable for ($($a,)+) {
            const SIZE: usize = 0 $(+ $a::SIZE)+;
            #[inline]
            #[expect(non_snake_case, reason = "one binding per type parameter")]
            fn write_bytes(self, out: &mut [u8]) {
                let ($($a,)+) = self;
                let rest = out;
                $(
                    let (head, rest) = rest.split_at_mut($a::SIZE);
                    $a.write_bytes(head);
                )+
                debug_assert!(rest.is_empty(), "argument bytes fill the buffer");
            }
        }
    )+};
}

hashable_args! {
    (A, B)
    (A, B, C)
    (A, B, C, D)
    (A, B, C, D, E)
}

/// The hash of `args`, a [`Hashable`] value or tuple of them: `MurmurHash64A`
/// of their bytes with seed 0.
///
/// PBR 4e §B.2.7, pbrt-v4 `Hash(Args...)`.
#[inline]
#[must_use]
pub fn hash<T: Hashable>(args: T) -> u64 {
    const { assert!(T::SIZE <= MAX_HASH_BYTES, "too many bytes to hash") };
    let mut buf = [0_u8; MAX_HASH_BYTES];
    let bytes = &mut buf[..T::SIZE];
    args.write_bytes(bytes);
    murmur_hash_64a(bytes, 0)
}

/// A float in `[0, 1)` from the low 32 bits of [`hash`] of `args`.
///
/// pbrt-v4 `HashFloat(Args...)`.
#[inline]
#[must_use]
pub fn hash_float<T: Hashable>(args: T) -> f32 {
    #[expect(
        clippy::cast_possible_truncation,
        clippy::cast_precision_loss,
        reason = "as pbrt-v4: uint32_t(Hash(...)) * 0x1p-32f"
    )]
    let low = hash(args) as u32 as f32;
    low * TWO_TO_MINUS_32
}
