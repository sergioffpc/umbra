//! What the Coordinator knows of a running Render Job's progress: what is
//! assigned and merged, how long each Work Unit took, and the rates and
//! ETA derived from them (arc42 §8.10). The status line and the Render Job
//! Report both read it.

use std::time::{Duration, Instant};

use umbra_render::{Decomposition, PassIndex, TileIndex, WorkUnit};

/// The time constant of the moving average of the Pixel Sample rate.
const RATE_WINDOW: Duration = Duration::from_secs(10);

/// The shortest step the average takes in once it has a value: a shorter
/// one, such as the last before the end, would only add noise.
const MIN_STEP: Duration = Duration::from_millis(100);

/// An estimate of the time left.
#[derive(Debug, Clone, Copy, PartialEq)]
pub(crate) enum Eta {
    /// Nothing is merged yet: no rate to estimate from.
    Unknown,
    /// During Pass 0, from the Pixel Samples left alone: shown with `~`.
    Provisional(Duration),
    /// From Pass 1, from the Tile costs measured so far.
    Measured(Duration),
}

/// A moving average of the Pixel Samples merged per second over about
/// [`RATE_WINDOW`], updated at each tick; it starts at the first rate
/// measured over a tick.
#[derive(Debug, Clone, Copy)]
struct Rate {
    per_second: Option<f64>,
    last_tick: Instant,
    last_samples: u64,
}

impl Rate {
    /// Takes the samples merged since the last tick into the average.
    fn tick(&mut self, now: Instant, samples: u64) {
        let step = now.duration_since(self.last_tick);
        let too_short = match self.per_second {
            Some(_) => step < MIN_STEP,
            None => step.is_zero(),
        };
        if too_short || samples == 0 {
            return;
        }
        let dt = step.as_secs_f64();
        let instant = to_f64(samples - self.last_samples) / dt;
        let weight = dt / (RATE_WINDOW.as_secs_f64() + dt);
        self.per_second = Some(match self.per_second {
            Some(rate) => rate + weight * (instant - rate),
            None => instant,
        });
        self.last_tick = now;
        self.last_samples = samples;
    }
}

/// The progress of one Render Job, from the start of its render phase.
#[derive(Debug)]
pub(crate) struct Progress<'d> {
    decomposition: &'d Decomposition,
    start: Instant,
    /// The Work Units merged, per Pass.
    merged_per_pass: Vec<u32>,
    merged_units: u64,
    merged_samples: u64,
    in_flight: u32,
    busy_since: Option<Instant>,
    busy: Duration,
    thread_time: Duration,
    merge_time: Duration,
    render_time: Duration,
    tile_render_times: Vec<Duration>,
    tile_samples: Vec<u64>,
    rate: Rate,
}

impl<'d> Progress<'d> {
    /// Nothing assigned or merged yet, the render phase starting `now`.
    pub(crate) fn new(decomposition: &'d Decomposition, now: Instant) -> Self {
        let tiles = decomposition.tile_count() as usize;
        Self {
            decomposition,
            start: now,
            merged_per_pass: vec![0; decomposition.pass_count() as usize],
            merged_units: 0,
            merged_samples: 0,
            in_flight: 0,
            busy_since: None,
            busy: Duration::ZERO,
            thread_time: Duration::ZERO,
            merge_time: Duration::ZERO,
            render_time: Duration::ZERO,
            tile_render_times: vec![Duration::ZERO; tiles],
            tile_samples: vec![0; tiles],
            rate: Rate {
                per_second: None,
                last_tick: now,
                last_samples: 0,
            },
        }
    }

    /// A Work Unit was assigned to a Worker at `now`.
    pub(crate) fn assigned(&mut self, now: Instant) {
        if self.in_flight == 0 {
            self.busy_since = Some(now);
        }
        self.in_flight += 1;
    }

    /// A Work Unit in flight went back to the queue at `now`: its Worker
    /// went away before its Work Result arrived.
    pub(crate) fn returned(&mut self, now: Instant) {
        self.in_flight = self.in_flight.saturating_sub(1);
        self.end_busy_if_idle(now);
    }

    /// Ends the busy interval at `now` once nothing is in flight.
    fn end_busy_if_idle(&mut self, now: Instant) {
        if self.in_flight == 0
            && let Some(since) = self.busy_since.take()
        {
            self.busy += now.duration_since(since);
        }
    }

    /// The Local Worker's threads spent `thread_time` on a Work Unit,
    /// summed over them.
    pub(crate) fn rendered(&mut self, thread_time: Duration) {
        self.thread_time += thread_time;
    }

    /// The Work Result of `unit`, rendered in `render_time`, was merged at
    /// `now`, the merge taking `merge_time`.
    pub(crate) fn merged(
        &mut self,
        unit: WorkUnit,
        render_time: Duration,
        merge_time: Duration,
        now: Instant,
    ) {
        self.in_flight = self.in_flight.saturating_sub(1);
        self.end_busy_if_idle(now);
        let samples = self.pixel_samples(unit);
        let tile = unit.tile.0 as usize;
        self.merged_units += 1;
        self.merged_per_pass[unit.pass.0 as usize] += 1;
        self.merged_samples += samples;
        self.merge_time += merge_time;
        self.render_time += render_time;
        self.tile_render_times[tile] += render_time;
        self.tile_samples[tile] += samples;
    }

    /// Updates the moving average of the rate at `now`.
    pub(crate) fn tick(&mut self, now: Instant) {
        self.rate.tick(now, self.merged_samples);
    }

    /// The current Pass: the first not wholly merged, or the last once
    /// every one is.
    pub(crate) fn pass(&self) -> PassIndex {
        let tiles = self.decomposition.tile_count();
        let pending = self.merged_per_pass.iter().position(|&n| n < tiles);
        let last = self.merged_per_pass.len().saturating_sub(1);
        PassIndex(u32::try_from(pending.unwrap_or(last)).expect("Passes are counted in a u32"))
    }

    /// The Passes of the Render Job.
    pub(crate) fn passes(&self) -> u32 {
        self.decomposition.pass_count()
    }

    /// The Work Units merged.
    pub(crate) fn merged_units(&self) -> u64 {
        self.merged_units
    }

    /// The Work Units of the Render Job.
    pub(crate) fn total_units(&self) -> u64 {
        u64::from(self.decomposition.tile_count()) * u64::from(self.decomposition.pass_count())
    }

    /// The Pixel Samples merged.
    pub(crate) fn merged_samples(&self) -> u64 {
        self.merged_samples
    }

    /// The Pixel Samples of the Render Job.
    pub(crate) fn total_samples(&self) -> u64 {
        (0..self.tile_samples.len())
            .map(|tile| self.tile_total_samples(tile))
            .sum()
    }

    /// The Work Units assigned and not merged yet.
    pub(crate) fn in_flight(&self) -> u32 {
        self.in_flight
    }

    /// The moving average of the Pixel Samples merged per second.
    pub(crate) fn rate(&self) -> f64 {
        self.rate.per_second.unwrap_or(0.0)
    }

    /// The time since the render phase started.
    pub(crate) fn elapsed(&self, now: Instant) -> Duration {
        now.duration_since(self.start)
    }

    /// The time spent merging Work Results into the Film.
    pub(crate) fn merge_time(&self) -> Duration {
        self.merge_time
    }

    /// The time the Workers had at least one Work Unit in flight, up to
    /// `now`.
    pub(crate) fn busy_time(&self, now: Instant) -> Duration {
        self.busy
            + self
                .busy_since
                .map_or(Duration::ZERO, |s| now.duration_since(s))
    }

    /// The time the Local Worker's threads spent rendering the Work Units
    /// it sent back, summed over them.
    pub(crate) fn thread_time(&self) -> Duration {
        self.thread_time
    }

    /// Each Tile's normalised cost, in raster order: its render time over
    /// the Worker's mean time per Pixel Sample, so in Pixel Samples rendered
    /// at the Worker's mean speed (arc42 §8.10).
    pub(crate) fn tile_costs(&self) -> Vec<f64> {
        let per_sample = self.time_per_sample();
        self.tile_render_times
            .iter()
            .map(|time| match per_sample {
                Some(per_sample) => time.as_secs_f64() / per_sample,
                None => 0.0,
            })
            .collect()
    }

    /// The time left: during Pass 0, the Pixel Samples left over the rate;
    /// from Pass 1, the cost of the Pixel Samples left in each Tile at its
    /// measured cost per Pixel Sample, over the rate.
    pub(crate) fn eta(&self) -> Eta {
        if self.merged_units == self.total_units() {
            return Eta::Measured(Duration::ZERO);
        }
        let rate = self.rate();
        if rate <= 0.0 {
            return Eta::Unknown;
        }
        let left = to_f64(self.total_samples() - self.merged_samples);
        match self.pass() {
            PassIndex(0) => Eta::Provisional(Duration::from_secs_f64(left / rate)),
            PassIndex(_) => Eta::Measured(Duration::from_secs_f64(self.cost_left() / rate)),
        }
    }

    /// The Pixel Samples left, each weighted by its Tile's cost per Pixel
    /// Sample relative to the mean.
    fn cost_left(&self) -> f64 {
        let per_sample = self.time_per_sample();
        (0..self.tile_samples.len())
            .map(|tile| {
                let left = self.tile_total_samples(tile) - self.tile_samples[tile];
                to_f64(left) * self.relative_cost(tile, per_sample)
            })
            .sum()
    }

    /// The cost per Pixel Sample of `tile` over the mean, `per_sample`; 1
    /// for a Tile not measured yet.
    fn relative_cost(&self, tile: usize, per_sample: Option<f64>) -> f64 {
        let measured = self.tile_samples[tile];
        match per_sample {
            Some(per_sample) if measured > 0 => {
                self.tile_render_times[tile].as_secs_f64() / to_f64(measured) / per_sample
            }
            Some(_) | None => 1.0,
        }
    }

    /// The Worker's mean render time per Pixel Sample, in seconds, once it
    /// has merged any.
    fn time_per_sample(&self) -> Option<f64> {
        let time = self.render_time.as_secs_f64();
        (self.merged_samples > 0 && time > 0.0).then(|| time / to_f64(self.merged_samples))
    }

    /// The Pixel Samples of `unit`: its Tile's pixels times its Pass's
    /// sample indices.
    fn pixel_samples(&self, unit: WorkUnit) -> u64 {
        let pixels = self.decomposition.tile_bounds(unit.tile).area() as u64;
        pixels * self.decomposition.sample_indices(unit.pass).len() as u64
    }

    /// Every Pixel Sample of `tile`, over every Pass.
    fn tile_total_samples(&self, tile: usize) -> u64 {
        let tile = TileIndex(u32::try_from(tile).expect("Tiles are counted in a u32"));
        let pixels = self.decomposition.tile_bounds(tile).area() as u64;
        pixels * u64::from(self.decomposition.samples_per_pixel())
    }
}

/// `n` as an `f64`, for rates and estimates.
#[expect(
    clippy::cast_precision_loss,
    reason = "rates and estimates, not exact counts"
)]
fn to_f64(n: u64) -> f64 {
    n as f64
}
