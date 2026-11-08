//! Choosing among a few discrete outcomes, and weighting the strategies of
//! multiple importance sampling.

use umbra_math::{ONE_MINUS_EPSILON, next_float_down, sqr};

/// An outcome chosen by [`sample_discrete`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct DiscreteSample {
    /// The index of the chosen weight.
    pub index: usize,
    /// Its probability: its weight over the sum of the weights.
    pub pmf: f32,
    /// The sample `u` remapped to `[0, 1)` within the chosen outcome, so it
    /// can be used again.
    pub u_remapped: f32,
}

/// An index into `weights` chosen with probability proportional to its
/// weight, by a linear scan; `None` if `weights` is empty.
///
/// # Panics
///
/// If the weights do not have a positive, finite sum.
///
/// PBR 4e §A.1, pbrt-v4 `SampleDiscrete`.
#[must_use]
pub fn sample_discrete(weights: &[f32], u: f32) -> Option<DiscreteSample> {
    if weights.is_empty() {
        return None;
    }
    let sum_weights = weights.iter().fold(0.0, |sum, &w| sum + w);

    let mut up = u * sum_weights;
    #[expect(clippy::float_cmp, reason = "pbrt-v4 keeps u·sum strictly below sum")]
    let at_end = up == sum_weights;
    if at_end {
        up = next_float_down(up);
    }

    let mut index = 0;
    let mut sum = 0.0;
    while sum + weights[index] <= up {
        sum += weights[index];
        index += 1;
    }

    let weight = weights[index];
    Some(DiscreteSample {
        index,
        pmf: weight / sum_weights,
        u_remapped: umbra_math::min((up - sum) / weight, ONE_MINUS_EPSILON),
    })
}

/// The balance heuristic weight of a sample from strategy `f`, taken `nf`
/// times with PDF `f_pdf`, against strategy `g`, taken `ng` times.
///
/// PBR 4e §2.2.3, pbrt-v4 `BalanceHeuristic`.
#[must_use]
#[expect(clippy::cast_precision_loss, reason = "as pbrt-v4: int * Float")]
pub fn balance_heuristic(nf: i32, f_pdf: f32, ng: i32, g_pdf: f32) -> f32 {
    (nf as f32 * f_pdf) / (nf as f32 * f_pdf + ng as f32 * g_pdf)
}

/// The power heuristic weight (exponent 2) of a sample from strategy `f`,
/// taken `nf` times with PDF `f_pdf`, against strategy `g`, taken `ng`
/// times; 1 when `f`'s square overflows.
///
/// PBR 4e §2.2.3, pbrt-v4 `PowerHeuristic`.
#[must_use]
#[expect(clippy::cast_precision_loss, reason = "as pbrt-v4: int * Float")]
pub fn power_heuristic(nf: i32, f_pdf: f32, ng: i32, g_pdf: f32) -> f32 {
    let (f, g) = (nf as f32 * f_pdf, ng as f32 * g_pdf);
    if sqr(f).is_infinite() {
        return 1.0;
    }
    sqr(f) / (sqr(f) + sqr(g))
}
