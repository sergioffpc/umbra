//! Moving the digit permutations to other storage, as the GPU Worker moves
//! them into device memory (ADR 0028): a sampler over the moved
//! permutations draws what it draws over the original.

use umbra_math::Point2i;
use umbra_sampling::{DigitPermutations, HaltonRandomizer, HaltonSampler, Sampler};

/// The first two dimensions of sample `sample_index` of `pixel`, then the
/// next eight.
fn draws(randomizer: HaltonRandomizer<'_>, pixel: Point2i, sample_index: u32) -> Vec<f32> {
    let mut sampler = HaltonSampler::new(64, Point2i::new(40, 24), randomizer);
    sampler.start_pixel_sample(pixel, sample_index, 0);
    let first = sampler.get_pixel_2d();
    let rest: Vec<f32> = std::iter::repeat_with(|| sampler.get_1d())
        .take(8)
        .collect();
    [first.x, first.y].into_iter().chain(rest).collect()
}

#[test]
fn digit_permutations_moved_to_other_storage_permute_as_before() {
    let permutations = DigitPermutations::new_in(7, |n| vec![0; n]);
    let moved: DigitPermutations<Box<[u16]>> = permutations.view().map_storage(Box::from);
    for (pixel, sample_index) in [
        (Point2i::new(0, 0), 0),
        (Point2i::new(17, 5), 3),
        (Point2i::new(39, 23), 63),
    ] {
        assert_eq!(
            draws(
                HaltonRandomizer::PermuteDigits(moved.view()),
                pixel,
                sample_index
            ),
            draws(
                HaltonRandomizer::PermuteDigits(permutations.view()),
                pixel,
                sample_index
            ),
            "sample {sample_index} of {pixel:?}"
        );
    }
}
