//! The GPU's math agrees with glibc's on the CPU (#146): each `libdevice`
//! function that `umbra-math`'s `nvptx64` backend calls, evaluated on the
//! GPU by `Tracer::evaluate`, is within the error bound that `umbra-math`'s
//! `src/libdevice.rs` documents of the x86-64 backend's result, glibc's.
//!
//! The inputs sweep the whole `f32` range by bit pattern, the subnormals
//! more densely, so both normal and subnormal inputs and results are
//! covered; each check asserts that its arguments reach subnormal results
//! where it means to. No nonzero result may come back as zero:
//! `libdevice.rs` documents none, and the primitives keeping subnormals is
//! what shows that `__nvvm_reflect("__CUDA_FTZ")` resolved to 0.
//!
//! Each test prints what it measured: `cargo nextest run --profile gpu
//! --no-capture`.

use std::f32::consts::PI;
use std::fmt;

use umbra_geometry::Sphere;
use umbra_gpu::{Function as MathFunction, Tracer};
use umbra_math::{Point3f, Vector3f, translate};

/// The tracer on the first GPU; its geometry is not used here.
fn tracer() -> Tracer {
    let triangle = [
        Point3f::new(-1.0, -1.0, 5.0),
        Point3f::new(1.0, -1.0, 5.0),
        Point3f::new(0.0, 1.0, 5.0),
    ];
    let sphere = Sphere::new(translate(Vector3f::new(0.0, 0.0, 10.0)), 2.0);
    Tracer::new(0, triangle, &sphere).unwrap_or_else(|error| panic!("no GPU tracer: {error}"))
}

/// The smallest positive subnormal, 2⁻¹⁴⁹.
const TINIEST: f32 = f32::from_bits(1);

/// 2ᵉ, exactly, for `e` in `[−149, 127]`: a power of two built from its
/// bits, the subnormal ones included.
fn pow2(e: i32) -> f32 {
    let bits = if e >= -126 {
        u32::try_from(e + 127).expect("e ≤ 127") << 23
    } else {
        1 << u32::try_from(e + 149).expect("e ≥ −149")
    };
    f32::from_bits(bits)
}

/// 2ˣ in `f64`, by glibc.
fn pow2_f64(x: f64) -> f64 {
    umbra_math::double::pow(2.0, x)
}

/// `sin_cos` reduces its argument itself up to this magnitude, 2²⁰.
const SIN_COS_LIMIT: f32 = 1_048_576.0;

/// Values a sweep by bit pattern may step over.
const SPECIALS: [f32; 10] = [
    0.0,
    -0.0,
    1.0,
    -1.0,
    f32::INFINITY,
    f32::NEG_INFINITY,
    f32::NAN,
    f32::MIN_POSITIVE,
    TINIEST,
    f32::MAX,
];

/// Whether `bits` is a signaling NaN, which the sweeps leave out: glibc
/// follows IEEE 754 for them (`powf(sNaN, 0)` is NaN, not 1), and the GPU
/// has no signaling NaNs.
fn is_signaling_nan(bits: u32) -> bool {
    f32::from_bits(bits).is_nan() && bits & 0x0040_0000 == 0
}

/// About four million `f32`s: every 1021st bit pattern, both signs and
/// infinities included; every 97th subnormal of either sign; and
/// [`SPECIALS`].
fn sweep() -> Vec<f32> {
    let all = (0..=u32::MAX).step_by(1021);
    let subnormals = (1..0x0080_0000_u32)
        .step_by(97)
        .flat_map(|bits| [bits, bits | 0x8000_0000]);
    all.chain(subnormals)
        .filter(|&bits| !is_signaling_nan(bits))
        .map(f32::from_bits)
        .chain(SPECIALS)
        .collect()
}

/// About `n` `f32`s evenly spread over the bit patterns, both signs.
fn coarse(n: u32) -> impl Iterator<Item = f32> + Clone {
    let step = u32::MAX / n;
    (0..n)
        .map(move |i| i * step)
        .filter(|&bits| !is_signaling_nan(bits))
        .map(f32::from_bits)
}

/// The argument records of a function of one argument.
fn unary(xs: impl IntoIterator<Item = f32>) -> Vec<[f32; 3]> {
    xs.into_iter().map(|x| [x, 0.0, 0.0]).collect()
}

/// Every pair of `xs` and `ys`, as the argument records of a function of
/// two arguments.
fn pairs(
    xs: impl IntoIterator<Item = f32>,
    ys: impl IntoIterator<Item = f32> + Clone,
) -> Vec<[f32; 3]> {
    xs.into_iter()
        .flat_map(move |x| ys.clone().into_iter().map(move |y| [x, y, 0.0]))
        .collect()
}

/// A distance between two `f32`s in ULPs: their difference in the order of
/// the `f32`s.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
struct Ulps(u64);

impl fmt::Display for Ulps {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} ULP", self.0)
    }
}

/// How far the GPU's result is from glibc's, in order: any distance in ULPs
/// is nearer than a NaN against a number.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum Distance {
    /// Both are numbers, or both NaN, at no distance.
    Ulps(Ulps),
    /// One is NaN and the other a number: beyond every bound.
    NanVersusNumber,
}

impl Distance {
    /// The distance of `a` and `b`. A subnormal flushed to zero is as far
    /// from zero as it is in ULPs of 2⁻¹⁴⁹.
    fn between(a: f32, b: f32) -> Self {
        match (a.is_nan(), b.is_nan()) {
            (true, true) => Self::Ulps(Ulps(0)),
            (false, false) => Self::Ulps(Ulps(ordinal(a).abs_diff(ordinal(b)))),
            (true, false) | (false, true) => Self::NanVersusNumber,
        }
    }
}

impl fmt::Display for Distance {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Ulps(ulps) => ulps.fmt(f),
            Self::NanVersusNumber => f.write_str("NaN against a number"),
        }
    }
}

/// The position of `x` among the `f32`s in order, both zeros at 0, so that
/// the difference of two is their distance in ULPs.
fn ordinal(x: f32) -> i64 {
    let magnitude = i64::from(x.to_bits() & 0x7FFF_FFFF);
    if x.is_sign_negative() {
        -magnitude
    } else {
        magnitude
    }
}

/// One record: its arguments, and the results of the GPU and of glibc.
#[derive(Debug, Clone, Copy)]
struct Sample {
    args: [f32; 3],
    gpu: f32,
    glibc: f32,
}

impl Sample {
    /// How far the GPU's result is from glibc's.
    fn distance(&self) -> Distance {
        Distance::between(self.gpu, self.glibc)
    }

    /// Whether the GPU gave glibc's nonzero result as zero.
    fn is_flushed_to_zero(&self) -> bool {
        self.gpu == 0.0 && self.glibc != 0.0
    }
}

impl fmt::Display for Sample {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let Self { args, gpu, glibc } = self;
        write!(f, "{args:?} (GPU {gpu:e}, glibc {glibc:e})")
    }
}

/// Whether the arguments of a check reach glibc results in the subnormal
/// range.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum SubnormalResults {
    /// Some of glibc's results are subnormal.
    Reached,
    /// None is: the function has none on these arguments.
    NotReached,
}

/// How a function's results on the GPU compare with glibc's.
#[derive(Debug)]
struct Comparison {
    function: MathFunction,
    /// The record at the largest distance.
    worst: Sample,
    /// A record whose nonzero glibc result the GPU gave as zero, if any.
    flushed_to_zero: Option<Sample>,
    /// How many of glibc's results are subnormal.
    subnormal_results: usize,
    /// How many records were compared.
    count: usize,
}

impl Comparison {
    /// Whether some of glibc's results are subnormal.
    fn subnormal_results(&self) -> SubnormalResults {
        if self.subnormal_results > 0 {
            SubnormalResults::Reached
        } else {
            SubnormalResults::NotReached
        }
    }
}

impl fmt::Display for Comparison {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "{:?}: max {} at {}, over {} records, {} with a subnormal result",
            self.function,
            self.worst.distance(),
            self.worst,
            self.count,
            self.subnormal_results
        )
    }
}

/// Evaluates `function` on `args` on the GPU and compares each result with
/// `cpu`'s, glibc's.
#[expect(clippy::print_stdout, reason = "the measured error is the report")]
fn compare(function: MathFunction, args: &[[f32; 3]], cpu: impl Fn([f32; 3]) -> f32) -> Comparison {
    let gpu = tracer().evaluate(function, args).expect("the launch runs");
    assert_eq!(gpu.len(), args.len(), "one result per record");
    let samples: Vec<Sample> = args
        .iter()
        .zip(gpu)
        .map(|(&args, gpu)| Sample {
            args,
            gpu,
            glibc: cpu(args),
        })
        .collect();
    let comparison = Comparison {
        function,
        worst: samples
            .iter()
            .copied()
            .max_by_key(Sample::distance)
            .expect("at least one record"),
        flushed_to_zero: samples.iter().copied().find(Sample::is_flushed_to_zero),
        subnormal_results: samples.iter().filter(|s| s.glibc.is_subnormal()).count(),
        count: samples.len(),
    };
    println!("{comparison}");
    comparison
}

/// Asserts that `function` on the GPU is within `bound` of `cpu` on every
/// record of `args`, that it gives none of glibc's nonzero results as zero,
/// and that `args` reach subnormal results as `subnormals` says.
fn assert_within_ulps(
    function: MathFunction,
    args: &[[f32; 3]],
    cpu: impl Fn([f32; 3]) -> f32,
    bound: Ulps,
    subnormals: SubnormalResults,
) {
    let comparison = compare(function, args, cpu);
    assert!(
        comparison.worst.distance() <= Distance::Ulps(bound),
        "beyond {bound}: {comparison}"
    );
    if let Some(sample) = comparison.flushed_to_zero {
        panic!("{function:?}: a nonzero result flushed to zero at {sample}");
    }
    assert_eq!(
        comparison.subnormal_results(),
        subnormals,
        "subnormal results: {comparison}"
    );
}

/// `__nvvm_reflect("__CUDA_FTZ")` resolved to 0: the IEEE primitives keep
/// subnormal inputs and results, where their `.ftz` variants would give
/// zeros. The expected values are exact.
#[test]
fn the_primitives_keep_subnormals() {
    let tracer = tracer();
    let cases = [
        (MathFunction::Fma, [TINIEST, 1.0, 0.0], TINIEST),
        (MathFunction::Fma, [-TINIEST, 3.0, 0.0], -3.0 * TINIEST),
        (MathFunction::Fma, [pow2(-70), pow2(-70), 0.0], pow2(-140)),
        (MathFunction::Sqrt, [pow2(-148), 0.0, 0.0], pow2(-74)),
        (MathFunction::Floor, [-TINIEST, 0.0, 0.0], -1.0),
        (MathFunction::Ceil, [TINIEST, 0.0, 0.0], 1.0),
    ];
    for (function, args, expected) in cases {
        let gpu = tracer.evaluate(function, &[args]).expect("the launch runs")[0];
        assert_eq!(
            gpu.to_bits(),
            expected.to_bits(),
            "{function:?}{args:?}: GPU {gpu:e}, expected {expected:e}"
        );
    }
}

#[test]
fn fma_is_exact() {
    let mut args: Vec<[f32; 3]> = coarse(161)
        .flat_map(|a| coarse(161).flat_map(move |b| coarse(161).map(move |c| [a, b, c])))
        .collect();
    // Products in and around the subnormal range, with small addends.
    let small = || (0..64).map(|i| pow2(-90 + i) * 1.375);
    args.extend(
        pairs(small(), small())
            .into_iter()
            .flat_map(|[a, b, _]| [0.0, TINIEST, -1.0e-40, 1.0e-30].map(|c| [a, -b, c])),
    );
    assert_within_ulps(
        MathFunction::Fma,
        &args,
        |[a, b, c]| umbra_math::fmaf(a, b, c),
        Ulps(0),
        SubnormalResults::Reached,
    );
}

/// Defines a test that checks the function of one argument `$function` on
/// the GPU against the glibc function `$cpu` on [`sweep`], within `$bound`
/// ULPs.
macro_rules! unary_test {
    ($($name:ident: $function:ident ~ $cpu:ident, $bound:literal, $subnormals:ident;)+) => {$(
        #[test]
        fn $name() {
            assert_within_ulps(
                MathFunction::$function,
                &unary(sweep()),
                |[x, ..]| umbra_math::$cpu(x),
                Ulps($bound),
                SubnormalResults::$subnormals,
            );
        }
    )+};
}

unary_test! {
    sqrt_is_correctly_rounded: Sqrt ~ sqrtf, 0, NotReached;
    floor_is_exact: Floor ~ floorf, 0, NotReached;
    ceil_is_exact: Ceil ~ ceilf, 0, NotReached;
    roundf_is_exact: Roundf ~ roundf, 0, NotReached;
    sinf_is_within_2_ulp: Sinf ~ sinf, 2, Reached;
    tanf_is_within_4_ulp: Tanf ~ tanf, 4, Reached;
    asinf_is_within_2_ulp: Asinf ~ asinf, 2, Reached;
    acosf_is_within_2_ulp: Acosf ~ acosf, 2, NotReached;
    erf_is_within_2_ulp: Erf ~ erff, 2, Reached;
    expf_is_within_2_ulp: Expf ~ expf, 2, Reached;
    logf_is_within_1_ulp: Logf ~ logf, 1, NotReached;
    atanhf_is_within_3_ulp: Atanhf ~ atanhf, 3, Reached;
    coshf_is_within_2_ulp: Coshf ~ coshf, 2, NotReached;
}

/// Both zeros and about a thousand subnormals of either sign, from the
/// smallest to the largest.
fn tiny() -> impl Iterator<Item = f32> + Clone {
    (1..0x0080_0000_u32)
        .step_by(16_381)
        .chain([0x007F_FFFF])
        .flat_map(|bits| [bits, bits | 0x8000_0000])
        .chain([0, 0x8000_0000])
        .map(f32::from_bits)
}

/// Within 3 ULP everywhere, both arguments subnormal or zero included,
/// where the GPU's `__nv_atan2f` alone returns the angle of the zeros.
#[test]
fn atan2_is_within_3_ulp() {
    let mut args = pairs(coarse(2048), coarse(2048));
    // `y` tiny against `x`: results in the subnormal range.
    args.extend(pairs(
        (0..2048).map(|i| f32::from_bits(i * 4093 + 1)),
        [1.0, -1.0, 3.0, 1.0e3, -1.0e6],
    ));
    // Both subnormal or zero: every angle, the axes included.
    args.extend(pairs(tiny(), tiny()));
    assert_within_ulps(
        MathFunction::Atan2,
        &args,
        |[y, x, _]| umbra_math::atan2f(y, x),
        Ulps(3),
        SubnormalResults::Reached,
    );
}

/// Within 4 ULP everywhere, subnormal arguments included, where the GPU's
/// `__nv_powf` alone takes them for zeros.
#[test]
fn powf_is_within_4_ulp() {
    let mut args = pairs(coarse(2048), coarse(2048));
    // Positive bases at exponents that reach the subnormal range.
    let bases = sweep().into_iter().filter(|x| *x > 0.0).step_by(7);
    args.extend(pairs(
        bases,
        [-150.5, -20.25, -2.5, -1.0, 0.5, 1.5, 3.0, 25.75, 150.5],
    ));
    // Subnormal bases of either sign, at exponents from −1.05 to 6.5 that
    // keep the result finite and nonzero, integers among them for the
    // negative bases.
    let exponents = (-1050_i16..=6500)
        .step_by(7)
        .map(|i| f32::from(i) / 1000.0)
        .chain([-1.0, 1.0, 2.0, 3.0]);
    args.extend(pairs(tiny(), exponents));
    // Subnormal exponents of either sign, on every kind of base.
    let tiny_exponents: Vec<f32> = tiny().filter(|y| *y != 0.0).step_by(8).collect();
    args.extend(pairs(coarse(2048).chain(SPECIALS), tiny_exponents));
    assert_within_ulps(
        MathFunction::Powf,
        &args,
        |[x, y, _]| umbra_math::powf(x, y),
        Ulps(4),
        SubnormalResults::Reached,
    );
}

/// Beyond `|x| = 2²⁰`, and at infinities and NaN, `sin_cos` is `libdevice`'s
/// `sinf` and `cosf`, within 2 ULP.
#[test]
fn sin_cos_beyond_2_pow_20_is_within_2_ulp() {
    let args = unary(
        sweep()
            .into_iter()
            .filter(|x| x.is_nan() || x.abs() > SIN_COS_LIMIT),
    );
    assert_within_ulps(
        MathFunction::SinOfSinCos,
        &args,
        |[x, ..]| umbra_math::sinf(x),
        Ulps(2),
        SubnormalResults::NotReached,
    );
    assert_within_ulps(
        MathFunction::CosOfSinCos,
        &args,
        |[x, ..]| umbra_math::cosf(x),
        Ulps(2),
        SubnormalResults::NotReached,
    );
}

/// The absolute difference of the GPU's result from glibc's at an argument.
#[derive(Debug, Clone, Copy)]
struct AbsoluteError {
    /// The difference, exact in `f64`.
    error: f64,
    /// The argument.
    x: f32,
}

/// The largest absolute difference of `function` on the GPU from `cpu` on
/// `xs`, none of them NaN, printed.
#[expect(clippy::print_stdout, reason = "the measured error is the report")]
fn max_absolute_error(function: MathFunction, xs: &[f32], cpu: impl Fn(f32) -> f32) -> f64 {
    let gpu = tracer()
        .evaluate(function, &unary(xs.iter().copied()))
        .expect("the launch runs");
    let AbsoluteError { error, x } = xs
        .iter()
        .zip(gpu)
        .map(|(&x, gpu)| AbsoluteError {
            error: (f64::from(gpu) - f64::from(cpu(x))).abs(),
            x,
        })
        .max_by(|a, b| a.error.total_cmp(&b.error))
        .expect("at least one argument");
    println!(
        "{function:?}: max absolute error 2^{:.2} at {x:e}, over {} records",
        umbra_math::double::log(error) / std::f64::consts::LN_2,
        xs.len()
    );
    error
}

/// On `[−π, π]`, where it needs no reduction, `sin_cos` is `sin.approx.f32`
/// and `cos.approx.f32` (`__nv_fast_sinf`, `__nv_fast_cosf`), within the
/// absolute errors `libdevice.rs` documents, 2⁻²¹·⁴¹ and 2⁻²¹·¹⁹; below
/// 2⁻¹², subnormals included, the sine is `x` itself.
#[test]
fn sin_cos_on_minus_pi_to_pi_is_within_its_absolute_bounds() {
    let xs: Vec<f32> = sweep().into_iter().filter(|x| x.abs() <= PI).collect();
    let sin = max_absolute_error(MathFunction::SinOfSinCos, &xs, umbra_math::sinf);
    assert!(sin <= pow2_f64(-21.41), "sine: {sin:e}");
    let cos = max_absolute_error(MathFunction::CosOfSinCos, &xs, umbra_math::cosf);
    assert!(cos <= pow2_f64(-21.19), "cosine: {cos:e}");
    let small = unary(xs.into_iter().filter(|x| x.abs() < 1.0 / 4096.0));
    assert_within_ulps(
        MathFunction::SinOfSinCos,
        &small,
        |[x, ..]| x,
        Ulps(0),
        SubnormalResults::Reached,
    );
}

/// Up to `|x| = 2²⁰`, Umbra's argument reduction keeps `sin_cos` within its
/// documented absolute error, 2⁻²⁰.
#[test]
fn sin_cos_up_to_2_pow_20_is_within_2_pow_minus_20() {
    let xs: Vec<f32> = sweep()
        .into_iter()
        .filter(|x| x.abs() <= SIN_COS_LIMIT)
        .collect();
    let sin = max_absolute_error(MathFunction::SinOfSinCos, &xs, umbra_math::sinf);
    assert!(sin < f64::from(pow2(-20)), "sine: {sin:e}");
    let cos = max_absolute_error(MathFunction::CosOfSinCos, &xs, umbra_math::cosf);
    assert!(cos < f64::from(pow2(-20)), "cosine: {cos:e}");
}
