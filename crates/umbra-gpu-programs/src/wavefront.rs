//! The wavefront's layout, shared by the host and the device: what a batch
//! of Work Units is rendered from, the structure-of-arrays queue between
//! its stages, and what it writes back (ADR 0026, ADR 0028).
//!
//! As `launch.rs`, it compiles on both targets, and `umbra-gpu` uses it
//! through an ordinary dependency (ADR 0033). Every type is `#[repr(C)]`
//! with layout assertions, which hold on `x86_64` and `nvptx64` alike. The
//! Scene's tables are in [`DeviceSlice`]s: the host moves them to device
//! memory with the core's `map_storage`, and the device reads them through
//! `AsRef<[T]>`, so the core samples them as it samples a `Vec` on the CPU.

use core::fmt;
use core::marker::PhantomData;

use umbra_camera::{CameraKind, FilterKind, PixelSensor};
use umbra_integrators::{PixelSampleId, VolPathIntegrator};
use umbra_math::Point2i;
use umbra_sampling::DigitPermutations;
use umbra_spectrum::N_SPECTRUM_SAMPLES;

/// `len` values of type `T` in device memory, from device address
/// `address`. On the device, it is the storage of the Scene's tables and
/// of the queues.
#[repr(C)]
pub struct DeviceSlice<T> {
    address: u64,
    len: u64,
    element: PhantomData<T>,
}

// `derive` would bound these on `T`: the slice is an address and a length,
// whatever it holds.
impl<T> Clone for DeviceSlice<T> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<T> Copy for DeviceSlice<T> {}

impl<T> PartialEq for DeviceSlice<T> {
    fn eq(&self, other: &Self) -> bool {
        (self.address, self.len) == (other.address, other.len)
    }
}

impl<T> Eq for DeviceSlice<T> {}

impl<T> fmt::Debug for DeviceSlice<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("DeviceSlice")
            .field("address", &format_args!("{:#x}", self.address))
            .field("len", &self.len)
            .finish()
    }
}

impl<T> DeviceSlice<T> {
    /// The slice of `len` values from device address `address`.
    ///
    /// # Safety
    ///
    /// `address` is the start of a device allocation of at least `len`
    /// initialised values of `T`, aligned for `T`, that outlives every
    /// launch given the slice. The device reads it as a `&[T]`, and a
    /// launch writes no value it reads that way.
    #[expect(
        unsafe_code,
        reason = "the promise behind the device's reads of the slice (ADR 0007, ADR 0028)"
    )]
    #[must_use]
    pub const unsafe fn from_raw_parts(address: u64, len: u64) -> Self {
        Self {
            address,
            len,
            element: PhantomData,
        }
    }
}

/// The device's access to a [`DeviceSlice`]: as a `&[T]`, the storage of
/// the core's tables, or value by value, as the stages read and write their
/// queues. A panic is a bug, and traps (ADR 0014). Not in the libm bench's
/// PTX, which has no stages.
#[cfg(all(target_arch = "nvptx64", not(umbra_libm_bench)))]
mod device {
    use super::DeviceSlice;

    impl<T> AsRef<[T]> for DeviceSlice<T> {
        #[expect(
            unsafe_code,
            reason = "reads device memory by its address (ADR 0007, ADR 0028)"
        )]
        fn as_ref(&self) -> &[T] {
            let len = usize::try_from(self.len).expect("a device slice fits `usize`");
            // SAFETY: `from_raw_parts`' caller promised `len` initialised,
            // aligned values at `address`, which no launch writes while it
            // reads them this way.
            unsafe { core::slice::from_raw_parts(self.address as *const T, len) }
        }
    }

    impl<T> DeviceSlice<T> {
        /// The address of value `i`.
        ///
        /// # Panics
        ///
        /// If `i` is not below the length.
        fn element(self, i: u32) -> *mut T {
            assert!(u64::from(i) < self.len, "index {i} past a device slice");
            (self.address as *mut T).wrapping_add(i as usize)
        }

        /// Value `i`.
        ///
        /// # Panics
        ///
        /// If `i` is not below the length.
        #[expect(
            unsafe_code,
            reason = "reads device memory by its address (ADR 0007, ADR 0028)"
        )]
        pub(crate) fn read(self, i: u32) -> T {
            // SAFETY: `element` checked that value `i` is one of the
            // allocation's initialised, aligned values.
            unsafe { self.element(i).read() }
        }

        /// Writes value `i`: only the thread whose Pixel Sample it is
        /// writes it.
        ///
        /// # Panics
        ///
        /// If `i` is not below the length.
        #[expect(
            unsafe_code,
            reason = "writes device memory by its address (ADR 0007, ADR 0028)"
        )]
        pub(crate) fn write(self, i: u32, value: T) {
            // SAFETY: `element` checked that value `i` is in the
            // allocation, aligned; only this thread accesses it in the
            // launch.
            unsafe { self.element(i).write(value) }
        }
    }
}

const _: () = {
    use core::mem::offset_of;

    assert!(
        size_of::<DeviceSlice<f32>>() == 16,
        "`DeviceSlice` is 16 bytes"
    );
    assert!(
        offset_of!(DeviceSlice<f32>, len) == 8,
        "`DeviceSlice::len` is at byte 8"
    );
};

/// The `SceneView` of a Render Job, its tables in device memory, and its
/// integrator: what every stage reads of the Scene.
#[derive(Debug, Clone, PartialEq)]
#[repr(C)]
pub struct SceneRecord {
    /// The camera.
    pub camera: CameraKind,
    /// The pixel filter, its sampling tables in device memory.
    pub filter: FilterKind<DeviceSlice<f32>>,
    /// The sensor, its response curves in device memory.
    pub sensor: PixelSensor<DeviceSlice<f32>>,
    /// The integrator.
    pub integrator: VolPathIntegrator,
}

const _: () = {
    use core::mem::offset_of;

    assert!(
        size_of::<SceneRecord>() == 1056,
        "`SceneRecord` is 1056 bytes: 816 of camera, 112 of filter, 112 of sensor, 4 of integrator, 12 of tail padding"
    );
    assert!(
        offset_of!(SceneRecord, filter) == 816,
        "`SceneRecord::filter` is at byte 816"
    );
    assert!(
        offset_of!(SceneRecord, sensor) == 928,
        "`SceneRecord::sensor` is at byte 928"
    );
    assert!(
        offset_of!(SceneRecord, integrator) == 1040,
        "`SceneRecord::integrator` is at byte 1040"
    );
};

/// How the Render Job's sampler randomises its sequence, with the digit
/// permutations in device memory when it permutes digits: the device's
/// `HaltonRandomizer`.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(C, u32)]
pub enum RandomizerRecord {
    /// No randomisation.
    None,
    /// Digit permutations of the Render Job's seed.
    PermuteDigits(DigitPermutations<DeviceSlice<u16>>),
    /// Owen scrambling.
    Owen,
}

// By hand: `DigitPermutations`' `Debug` reads its storage, which the host
// cannot.
impl fmt::Debug for RandomizerRecord {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::None => f.write_str("None"),
            Self::PermuteDigits(_) => f.write_str("PermuteDigits"),
            Self::Owen => f.write_str("Owen"),
        }
    }
}

/// The Render Job's sampler, before any Pixel Sample starts: what each
/// thread of the camera stage builds its own from.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(C)]
pub struct SamplerRecord {
    /// How it randomises.
    pub randomizer: RandomizerRecord,
    /// The image's full resolution.
    pub resolution: Point2i,
    /// The Pixel Samples of every pixel, over every Pass.
    pub samples_per_pixel: u32,
}

const _: () = {
    use core::mem::offset_of;

    assert!(
        size_of::<RandomizerRecord>() == 24,
        "`RandomizerRecord` is its `u32` tag, padded to 8, then the permutations"
    );
    assert!(
        size_of::<SamplerRecord>() == 40,
        "`SamplerRecord` is 40 bytes, the last 4 padding"
    );
    assert!(
        offset_of!(SamplerRecord, resolution) == 24,
        "`SamplerRecord::resolution` is at byte 24"
    );
    assert!(
        offset_of!(SamplerRecord, samples_per_pixel) == 32,
        "`SamplerRecord::samples_per_pixel` is at byte 32"
    );
};

/// One Work Unit of a batch: its Tile, its Pass's sample indices, and
/// where its Pixel Samples start in the batch. A batch's Pixel Samples are
/// its Work Units' in order, each Work Unit's pixel by pixel in raster
/// order, each pixel's in ascending sample index.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(C)]
pub struct WorkUnitRecord {
    /// The Tile's top-left pixel.
    pub tile_min: Point2i,
    /// The Tile's width, in pixels.
    pub tile_width: u32,
    /// The first sample index of the Pass.
    pub first_sample_index: u32,
    /// The Pass's number of Pixel Samples in each pixel.
    pub samples_per_pixel: u32,
    /// The index of the Work Unit's first Pixel Sample in the batch.
    pub first_pixel_sample: u32,
}

const _: () = {
    use core::mem::offset_of;

    assert!(
        size_of::<WorkUnitRecord>() == 24,
        "`WorkUnitRecord` is 24 bytes"
    );
    assert!(
        offset_of!(WorkUnitRecord, tile_width) == 8,
        "`WorkUnitRecord::tile_width` is at byte 8"
    );
    assert!(
        offset_of!(WorkUnitRecord, first_sample_index) == 12,
        "`WorkUnitRecord::first_sample_index` is at byte 12"
    );
    assert!(
        offset_of!(WorkUnitRecord, samples_per_pixel) == 16,
        "`WorkUnitRecord::samples_per_pixel` is at byte 16"
    );
    assert!(
        offset_of!(WorkUnitRecord, first_pixel_sample) == 20,
        "`WorkUnitRecord::first_pixel_sample` is at byte 20"
    );
};

impl WorkUnitRecord {
    /// The Pixel Sample `i` of the batch, which is one of this Work Unit's.
    ///
    /// # Panics
    ///
    /// If `i` is before the Work Unit's first Pixel Sample, or its Tile is
    /// wider than `i32::MAX` pixels.
    #[must_use]
    pub fn pixel_sample(&self, i: u32) -> PixelSampleId {
        let local = i - self.first_pixel_sample;
        let pixel = local / self.samples_per_pixel;
        let [x, y] = [pixel % self.tile_width, pixel / self.tile_width]
            .map(|offset| i32::try_from(offset).expect("a Tile fits `i32`"));
        PixelSampleId {
            pixel: Point2i::new(self.tile_min.x + x, self.tile_min.y + y),
            sample_index: self.first_sample_index + local % self.samples_per_pixel,
        }
    }
}

/// One value per wavelength a Pixel Sample carries: a `SampledSpectrum`'s
/// values, or a `SampledWavelengths`' wavelengths or PDFs.
pub type Lanes = [f32; N_SPECTRUM_SAMPLES];

/// The queue between the camera stage and the film stage, one entry per
/// Pixel Sample of the batch, each field an array of its own.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(C)]
pub struct PixelSampleQueue {
    /// The wavelengths, in nm.
    pub lambda: DeviceSlice<Lanes>,
    /// The PDFs of the wavelengths.
    pub pdf: DeviceSlice<Lanes>,
    /// The radiance arriving along the camera ray, weighted by it.
    pub radiance: DeviceSlice<Lanes>,
    /// The filter weight of the camera sample.
    pub filter_weight: DeviceSlice<f32>,
}

/// What one Pixel Sample adds to its pixel: the sensor's RGB and the filter
/// weight, the core's `FilmSample`.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
#[repr(C)]
pub struct FilmSampleRecord {
    /// The sensor's RGB.
    pub rgb: [f32; 3],
    /// The filter weight.
    pub weight: f32,
}

const _: () = {
    use core::mem::offset_of;

    assert!(
        size_of::<PixelSampleQueue>() == 64,
        "`PixelSampleQueue` is four `DeviceSlice`s"
    );
    assert!(
        offset_of!(PixelSampleQueue, pdf) == 16,
        "`PixelSampleQueue::pdf` is at byte 16"
    );
    assert!(
        offset_of!(PixelSampleQueue, radiance) == 32,
        "`PixelSampleQueue::radiance` is at byte 32"
    );
    assert!(
        offset_of!(PixelSampleQueue, filter_weight) == 48,
        "`PixelSampleQueue::filter_weight` is at byte 48"
    );
    assert!(
        size_of::<FilmSampleRecord>() == 16,
        "`FilmSampleRecord` is 16 bytes"
    );
    assert!(
        offset_of!(FilmSampleRecord, weight) == 12,
        "`FilmSampleRecord::weight` is at byte 12"
    );
};

/// What the host passes to each stage of a batch, in the same `.const`
/// variable `params` as `LaunchParams`. Each stage runs one thread per
/// Pixel Sample of the batch.
#[derive(Debug, Clone, Copy, PartialEq)]
#[repr(C)]
pub struct WavefrontParams {
    /// The Scene, one record.
    pub scene: DeviceSlice<SceneRecord>,
    /// The sampler, one record.
    pub sampler: DeviceSlice<SamplerRecord>,
    /// The batch's Work Units, in the order of their Pixel Samples.
    pub units: DeviceSlice<WorkUnitRecord>,
    /// The queue between the stages.
    pub queue: PixelSampleQueue,
    /// What each Pixel Sample adds to its pixel, written by the film stage.
    pub film_samples: DeviceSlice<FilmSampleRecord>,
}

const _: () = {
    use core::mem::offset_of;

    assert!(
        size_of::<WavefrontParams>() == 128,
        "`WavefrontParams` is 128 bytes"
    );
    assert!(
        offset_of!(WavefrontParams, sampler) == 16,
        "`WavefrontParams::sampler` is at byte 16"
    );
    assert!(
        offset_of!(WavefrontParams, units) == 32,
        "`WavefrontParams::units` is at byte 32"
    );
    assert!(
        offset_of!(WavefrontParams, queue) == 48,
        "`WavefrontParams::queue` is at byte 48"
    );
    assert!(
        offset_of!(WavefrontParams, film_samples) == 112,
        "`WavefrontParams::film_samples` is at byte 112"
    );
};
