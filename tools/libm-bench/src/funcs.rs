//! The functions benchmarked, their inputs, and their exact results.

use std::fmt;

/// Inputs per batch: a multiple of every vector width.
pub const BATCH: usize = 1024;

/// The exponent of `powf`: a gamma curve, as `SampledSpectrum::pow` takes it.
pub const GAMMA: f32 = 2.2;

/// Up to three arguments per element; the unused ones are zero.
pub struct Inputs {
    pub a: Vec<f32>,
    pub b: Vec<f32>,
    pub c: Vec<f32>,
}

/// How many results one evaluation gives.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Outputs {
    One,
    /// `sincosf`: the sine, then the cosine.
    Two,
}

/// One function of the table.
pub struct Func {
    pub name: &'static str,
    pub inputs: fn() -> Inputs,
    /// The exact result: glibc's `f64` routine, or an exact `f32` operation.
    /// The second is the cosine for `sincosf`, unused otherwise.
    pub exact: fn(f32, f32, f32) -> [f64; 2],
    pub outputs: Outputs,
}

/// glibc's `f64` routines, the reference the `f32` results are measured
/// against: within 1 ULP of `f64`, far below an `f32` ULP.
mod glibc64 {
    #[link(name = "m")]
    unsafe extern "C" {
        pub fn sin(x: f64) -> f64;
        pub fn cos(x: f64) -> f64;
        pub fn tan(x: f64) -> f64;
        pub fn asin(x: f64) -> f64;
        pub fn acos(x: f64) -> f64;
        pub fn atan2(y: f64, x: f64) -> f64;
        pub fn cosh(x: f64) -> f64;
        pub fn atanh(x: f64) -> f64;
        pub fn exp(x: f64) -> f64;
        pub fn exp2(x: f64) -> f64;
        pub fn log(x: f64) -> f64;
        pub fn pow(x: f64, y: f64) -> f64;
        pub fn erf(x: f64) -> f64;
    }
}

/// The exact result of a unary `f64` routine.
macro_rules! exact1 {
    ($f:ident) => {
        // SAFETY: a glibc math function of `f64` values, no preconditions.
        |a, _, _| [unsafe { glibc64::$f(f64::from(a)) }, 0.0]
    };
}

/// The exact result of a binary `f64` routine.
macro_rules! exact2 {
    ($f:ident) => {
        // SAFETY: a glibc math function of `f64` values, no preconditions.
        |a, b, _| [unsafe { glibc64::$f(f64::from(a), f64::from(b)) }, 0.0]
    };
}

/// `SplitMix64`: the inputs are the same on every machine.
struct Rng(u64);

impl Rng {
    fn next_u32(&mut self) -> u32 {
        self.0 = self.0.wrapping_add(0x9E37_79B9_7F4A_7C15);
        let mut z = self.0;
        z = (z ^ (z >> 30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94D0_49BB_1331_11EB);
        u32::try_from((z ^ (z >> 31)) >> 32).unwrap_or_default()
    }

    /// Uniform in `[0, 1)`, 24 bits.
    fn unit(&mut self) -> f32 {
        #[expect(clippy::cast_precision_loss, reason = "24 bits are exact in f32")]
        let bits = (self.next_u32() >> 8) as f32;
        bits * (1.0 / 16_777_216.0)
    }
}

/// [`BATCH`] values uniform in `[low, high)`.
fn uniform(seed: u64, low: f32, high: f32) -> Vec<f32> {
    let mut rng = Rng(seed);
    (0..BATCH)
        .map(|_| low + (high - low) * rng.unit())
        .collect()
}

/// [`BATCH`] values of renderer magnitude: uniform in `[−1, 1)` times a
/// power of two in `[2⁻¹⁰, 2¹⁰]`, never zero (as umbra-math's benches).
fn values(seed: u64) -> Vec<f32> {
    let mut rng = Rng(seed);
    (0..BATCH)
        .map(|_| {
            let magnitude = f32::from_bits((rng.next_u32() % 21 + 117) << 23);
            let u = (rng.unit() * 2.0 - 1.0) * magnitude;
            if u == 0.0 { magnitude } else { u }
        })
        .collect()
}

/// [`values`], made positive.
fn magnitudes(seed: u64) -> Vec<f32> {
    values(seed).into_iter().map(f32::abs).collect()
}

fn one(a: Vec<f32>) -> Inputs {
    two(a, vec![0.0; BATCH])
}

fn two(a: Vec<f32>, b: Vec<f32>) -> Inputs {
    Inputs {
        a,
        b,
        c: vec![0.0; BATCH],
    }
}

/// The 19 `f32` functions umbra-math exports from `libm`, in the order of the
/// table.
pub const FUNCS: &[Func] = &[
    Func {
        name: "sinf",
        inputs: || one(uniform(1, -7.0, 7.0)),
        exact: exact1!(sin),
        outputs: Outputs::One,
    },
    Func {
        name: "cosf",
        inputs: || one(uniform(2, -7.0, 7.0)),
        exact: exact1!(cos),
        outputs: Outputs::One,
    },
    Func {
        name: "sincosf",
        inputs: || one(uniform(3, -7.0, 7.0)),
        // SAFETY: glibc math functions of `f64` values, no preconditions.
        exact: |a, _, _| unsafe { [glibc64::sin(f64::from(a)), glibc64::cos(f64::from(a))] },
        outputs: Outputs::Two,
    },
    Func {
        name: "tanf",
        inputs: || one(uniform(4, -1.5, 1.5)),
        exact: exact1!(tan),
        outputs: Outputs::One,
    },
    Func {
        name: "asinf",
        inputs: || one(uniform(5, -1.0, 1.0)),
        exact: exact1!(asin),
        outputs: Outputs::One,
    },
    Func {
        name: "acosf",
        inputs: || one(uniform(6, -1.0, 1.0)),
        exact: exact1!(acos),
        outputs: Outputs::One,
    },
    Func {
        name: "atan2f",
        inputs: || two(values(7), values(8)),
        exact: exact2!(atan2),
        outputs: Outputs::One,
    },
    Func {
        name: "coshf",
        inputs: || one(uniform(9, -10.0, 10.0)),
        exact: exact1!(cosh),
        outputs: Outputs::One,
    },
    Func {
        name: "atanhf",
        inputs: || one(uniform(10, -0.99, 0.99)),
        exact: exact1!(atanh),
        outputs: Outputs::One,
    },
    Func {
        name: "expf",
        inputs: || one(uniform(11, -20.0, 2.0)),
        exact: exact1!(exp),
        outputs: Outputs::One,
    },
    Func {
        name: "exp2f",
        inputs: || one(uniform(12, -20.0, 20.0)),
        exact: exact1!(exp2),
        outputs: Outputs::One,
    },
    Func {
        name: "logf",
        inputs: || one(magnitudes(13)),
        exact: exact1!(log),
        outputs: Outputs::One,
    },
    Func {
        name: "powf",
        inputs: || two(uniform(14, 0.0, 4.0), vec![GAMMA; BATCH]),
        exact: exact2!(pow),
        outputs: Outputs::One,
    },
    Func {
        name: "erff",
        inputs: || one(uniform(15, -4.0, 4.0)),
        exact: exact1!(erf),
        outputs: Outputs::One,
    },
    Func {
        name: "sqrtf",
        inputs: || one(magnitudes(16)),
        exact: |a, _, _| [f64::from(a).sqrt(), 0.0],
        outputs: Outputs::One,
    },
    Func {
        name: "fmaf",
        inputs: || Inputs {
            a: values(17),
            b: values(18),
            c: values(19),
        },
        exact: |a, b, c| [f64::from(a.mul_add(b, c)), 0.0],
        outputs: Outputs::One,
    },
    Func {
        name: "floorf",
        inputs: || one(uniform(20, -1000.0, 1000.0)),
        exact: |a, _, _| [f64::from(a.floor()), 0.0],
        outputs: Outputs::One,
    },
    Func {
        name: "ceilf",
        inputs: || one(uniform(21, -1000.0, 1000.0)),
        exact: |a, _, _| [f64::from(a.ceil()), 0.0],
        outputs: Outputs::One,
    },
    Func {
        name: "roundf",
        inputs: || one(uniform(22, -1000.0, 1000.0)),
        exact: |a, _, _| [f64::from(a.round()), 0.0],
        outputs: Outputs::One,
    },
];

/// The largest error of an implementation over a batch.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum UlpError {
    /// The distance to the correctly rounded result, in `f32` ULPs.
    Ulps(u64),
    /// A NaN where the exact result is a number, or the other way round.
    Nan,
}

impl UlpError {
    /// The error of `got` against the exact result.
    fn of(got: f32, exact: f64) -> Self {
        #[expect(
            clippy::cast_possible_truncation,
            reason = "the rounding to f32 is the point"
        )]
        let want = exact as f32;
        match (got.is_nan(), want.is_nan()) {
            (true, true) => Self::Ulps(0),
            (false, false) => Self::Ulps(ordered(got).abs_diff(ordered(want))),
            _ => Self::Nan,
        }
    }

    /// The largest error of `out1` (and `out2` for two outputs) on `inputs`.
    pub fn max(func: &Func, inputs: &Inputs, out1: &[f32], out2: &[f32]) -> Self {
        (0..BATCH)
            .map(|i| {
                let exact = (func.exact)(inputs.a[i], inputs.b[i], inputs.c[i]);
                let first = Self::of(out1[i], exact[0]);
                match func.outputs {
                    Outputs::One => first,
                    Outputs::Two => first.max(Self::of(out2[i], exact[1])),
                }
            })
            .max()
            .unwrap_or(Self::Ulps(0))
    }
}

impl fmt::Display for UlpError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Ulps(n) => write!(f, "{n}"),
            Self::Nan => f.write_str("NaN"),
        }
    }
}

/// The `f32`s in order as integers: adjacent floats differ by one.
fn ordered(x: f32) -> i64 {
    let bits = x.to_bits();
    let magnitude = i64::from(bits & 0x7fff_ffff);
    if x.is_sign_negative() {
        -magnitude
    } else {
        magnitude
    }
}
