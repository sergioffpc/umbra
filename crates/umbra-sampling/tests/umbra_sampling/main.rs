//! Tests of `umbra-sampling` through its public interface: the pbrt-v4 unit
//! tests it ports, its results against the values pbrt-v4 itself computes,
//! and the statistical sampling tests of arc42 §8.11.

mod chi2;
mod generators;
mod pbrt_v4;
mod sampling;
mod storage;
