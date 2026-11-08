//! Tests of `umbra-spectrum` through its public interface: the pbrt-v4 unit
//! tests it ports, its results against the values pbrt-v4 itself computes,
//! and the hash of the generated RGB-to-spectrum table (ADR 0002).

mod generators;
mod pbrt_v4;
mod sampled;
mod table;
