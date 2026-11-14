//! The GPU's Renderer: a batch of Work Units in, one Work Result per Work
//! Unit out, through the wavefront of `umbra-gpu-programs` (ADR 0026,
//! ADR 0028).

use std::ops::Range;
use std::rc::Rc;
use std::sync::Arc;

use cudarc::driver::{CudaSlice, CudaStream, DevicePtr, DeviceRepr, ValidAsZeroBits};
use umbra_gpu_programs::{
    DeviceSlice, FilmSampleRecord, Lanes, PixelSampleQueue, RandomizerRecord, SamplerRecord,
    SceneRecord, WavefrontParams, WorkUnitRecord,
};
use umbra_integrators::FilmSample;
use umbra_render::{Cancellation, Decomposition, PixelSum, Rendered, WorkResult, WorkUnit};
use umbra_sampling::{DigitPermutations, RandomizeStrategy};
use umbra_scene::{RenderJobOptions, Scene};
use umbra_spectrum::{N_SPECTRUM_SAMPLES, RGB};

use crate::Error;
use crate::device::{self, Device, record, stride};
use crate::optix::{Module, Pipeline, ProgramGroup, Programs, ShaderBindingTable};
use crate::records::{DeviceCopy, SbtRecord};

/// Renders the Work Units of a Render Job of one Scene into Work Results on
/// one GPU, a batch at a time: each stage of the wavefront runs once over
/// every Pixel Sample of the batch.
///
/// Building it uploads what the stages read of the Scene and the sampler,
/// once for the Render Job; dropping it frees the GPU's memory and OptiX's
/// objects.
#[derive(Debug)]
pub struct Renderer {
    // Declared first, so that it drops before the CUDA memory below and the
    // context the stream holds.
    pipeline: Pipeline,
    camera: CudaSlice<SbtRecord<()>>,
    film: CudaSlice<SbtRecord<()>>,
    miss: CudaSlice<SbtRecord<()>>,
    hitgroup: CudaSlice<SbtRecord<()>>,
    scene: Uploaded<DeviceCopy<SceneRecord>>,
    sampler: Uploaded<DeviceCopy<SamplerRecord>>,
    // The tables `scene` and `sampler` point into: kept for as long as they
    // are read.
    _tables: Uploaded<f32>,
    _permutations: Option<Uploaded<u16>>,
    decomposition: Decomposition,
    stream: Arc<CudaStream>,
}

/// The program groups of the pipeline.
struct Groups {
    camera: Rc<ProgramGroup>,
    film: Rc<ProgramGroup>,
    miss: Rc<ProgramGroup>,
    hitgroup: Rc<ProgramGroup>,
}

impl Groups {
    /// The stages' programs, and the miss and hit group programs every
    /// shader binding table needs, though no stage traces a ray yet.
    fn new(module: &Rc<Module>) -> Result<Self, Error> {
        Ok(Self {
            camera: ProgramGroup::new(module, Programs::Raygen(c"__raygen__camera"))?,
            film: ProgramGroup::new(module, Programs::Raygen(c"__raygen__film"))?,
            miss: ProgramGroup::new(module, Programs::Miss(c"__miss__trace"))?,
            hitgroup: ProgramGroup::new(
                module,
                Programs::Hitgroup {
                    closest_hit: c"__closesthit__trace",
                    intersection: None,
                },
            )?,
        })
    }
}

impl Renderer {
    /// The Renderer on GPU `ordinal` of a Render Job of `scene` with
    /// `options`: builds the pipeline and uploads the Scene's view and the
    /// sampler's digit permutations, if it permutes digits.
    ///
    /// # Errors
    ///
    /// [`Error::Render`] if the Render Job cannot be cut into Work Units,
    /// and the errors of a GPU that cannot run the programs:
    /// [`Error::Cuda`] without a CUDA driver or device, [`Error::OptixLibrary`]
    /// or [`Error::OptixFunction`] without a driver new enough for OptiX 9.0,
    /// [`Error::UnlinkedPtx`] when the crate was built without `libdevice`,
    /// and [`Error::Optix`] when OptiX rejects the programs.
    pub fn new(ordinal: usize, scene: &Scene, options: &RenderJobOptions) -> Result<Self, Error> {
        let decomposition = Decomposition::new(scene.full_resolution(), options)?;
        let Device {
            stream,
            context,
            module,
        } = Device::open(ordinal)?;
        let groups = Groups::new(&module)?;
        let pipeline = Pipeline::new(
            &context,
            &[&groups.camera, &groups.film, &groups.miss, &groups.hitgroup],
        )?;

        let (scene_record, tables) = upload_scene(&stream, scene)?;
        let (sampler_record, permutations) = upload_sampler(&stream, scene, options)?;
        Ok(Self {
            camera: record(&stream, &groups.camera, ())?,
            film: record(&stream, &groups.film, ())?,
            miss: record(&stream, &groups.miss, ())?,
            hitgroup: record(&stream, &groups.hitgroup, ())?,
            pipeline,
            scene: Uploaded::new(&stream, &[DeviceCopy(scene_record)])?,
            sampler: Uploaded::new(&stream, &[DeviceCopy(sampler_record)])?,
            _tables: tables,
            _permutations: permutations,
            decomposition,
            stream,
        })
    }

    /// How the Render Job is cut into Work Units.
    #[must_use]
    pub fn decomposition(&self) -> &Decomposition {
        &self.decomposition
    }

    /// The Work Units a GPU Worker keeps in flight and renders as one
    /// batch: about 16 to start with, to be tuned by benchmark (ADR 0026).
    pub const CAPACITY: u32 = 16;

    /// Renders `units` as one batch into their Work Results, in the order of
    /// `units`, unless `cancel` stops it first: then every one is
    /// [`Rendered::Cancelled`]. Each pixel's Pixel Samples are summed on the
    /// host in ascending sample index, as on the CPU, but the GPU rounds
    /// them its own way, so a Work Result is equal to the CPU's up to noise
    /// only (ADR 0026). The batch may hold more than [`Self::CAPACITY`] Work
    /// Units: the capacity is what a GPU Worker keeps in flight, and a
    /// batch's memory grows with its Pixel Samples.
    ///
    /// # Errors
    ///
    /// [`Error::TooMany`] beyond `u32::MAX` Pixel Samples in the batch, and
    /// [`Error::Cuda`] or [`Error::Optix`] when the GPU fails.
    ///
    /// # Panics
    ///
    /// If a Work Unit of `units` is not one of this Render Job's.
    pub fn render(
        &self,
        units: &[WorkUnit],
        cancel: &Cancellation,
    ) -> Result<Vec<Rendered>, Error> {
        let cancelled = || vec![Rendered::Cancelled; units.len()];
        if units.is_empty() || cancel.is_cancelled() {
            return Ok(cancelled());
        }
        let batch = Batch::new(&self.decomposition, units)?;
        let queue = Queue::new(&self.stream, batch.pixel_samples)?;
        let film_samples = Uploaded::new(
            &self.stream,
            &vec![DeviceCopy(FilmSampleRecord::default()); batch.pixel_samples],
        )?;
        let units_uploaded = Uploaded::new(&self.stream, &batch.records)?;
        let params = WavefrontParams {
            scene: self.scene.whole(),
            sampler: self.sampler.whole(),
            units: units_uploaded.whole(),
            queue: queue.record(),
            film_samples: film_samples.whole(),
        };
        let width =
            u32::try_from(batch.pixel_samples).map_err(|_| Error::TooMany("Pixel Samples"))?;
        self.launch(&self.camera, params, width)?;
        if cancel.is_cancelled() {
            return Ok(cancelled());
        }
        self.launch(&self.film, params, width)?;
        let film_samples = self.stream.clone_dtoh(&film_samples.buffer)?;
        Ok(batch.results(units, &film_samples))
    }

    /// Runs the stage whose ray-generation record is `raygen` over the
    /// `width` Pixel Samples of the batch of `params`, and waits for it.
    fn launch(
        &self,
        raygen: &CudaSlice<SbtRecord<()>>,
        params: WavefrontParams,
        width: u32,
    ) -> Result<(), Error> {
        let sbt = self.binding_table(raygen);
        device::launch(
            &self.pipeline,
            &self.stream,
            DeviceCopy(params),
            width,
            &sbt,
        )
    }

    /// The shader binding table of launches of the stage whose
    /// ray-generation record is `raygen`.
    fn binding_table(&self, raygen: &CudaSlice<SbtRecord<()>>) -> ShaderBindingTable {
        let record_stride = stride(size_of::<SbtRecord<()>>());
        ShaderBindingTable {
            raygen: raygen.device_ptr(&self.stream).0,
            miss: self.miss.device_ptr(&self.stream).0,
            miss_stride: record_stride,
            hitgroup: self.hitgroup.device_ptr(&self.stream).0,
            hitgroup_stride: record_stride,
        }
    }
}

/// The Scene's view, its tables copied to the device, and its integrator:
/// the record the stages read, and the tables it points into.
fn upload_scene(
    stream: &Arc<CudaStream>,
    scene: &Scene,
) -> Result<(SceneRecord, Uploaded<f32>), Error> {
    let view = scene.view();
    let mut tables = Staging::default();
    let filter = view
        .filter()
        .clone()
        .map_storage(|values| tables.place(values));
    let sensor = view
        .sensor()
        .clone()
        .map_storage(|values| tables.place(values));
    let tables = tables.upload(stream)?;
    let record = SceneRecord {
        camera: *view.camera(),
        filter: filter.map_storage(|range| tables.slice(range)),
        sensor: sensor.map_storage(|range| tables.slice(range)),
        integrator: scene.integrator(),
    };
    Ok((record, tables))
}

/// The Render Job's sampler, its digit permutations copied to the device if
/// it permutes digits: the record the camera stage reads, and the
/// permutations it points into.
fn upload_sampler(
    stream: &Arc<CudaStream>,
    scene: &Scene,
    options: &RenderJobOptions,
) -> Result<(SamplerRecord, Option<Uploaded<u16>>), Error> {
    let (randomizer, permutations) = match scene.randomize_strategy() {
        RandomizeStrategy::None => (RandomizerRecord::None, None),
        RandomizeStrategy::Owen => (RandomizerRecord::Owen, None),
        RandomizeStrategy::PermuteDigits => {
            let permutations = DigitPermutations::new_in(options.seed.into(), |n| vec![0; n]);
            let mut values = Staging::default();
            let placed = permutations.view().map_storage(|v| values.place(v));
            let values = values.upload(stream)?;
            let record = placed.map_storage(|range| values.slice(range));
            (RandomizerRecord::PermuteDigits(record), Some(values))
        }
    };
    let resolution = scene.full_resolution();
    let record = SamplerRecord {
        randomizer,
        resolution,
        samples_per_pixel: options.spp.get(),
    };
    Ok((record, permutations))
}

/// Values gathered into one host buffer, to be uploaded as one device
/// buffer: the Scene's tables, or the digit permutations.
struct Staging<T> {
    values: Vec<T>,
}

impl<T> Default for Staging<T> {
    fn default() -> Self {
        Self { values: Vec::new() }
    }
}

impl<T: Copy + DeviceRepr> Staging<T> {
    /// Appends `values`, and returns where they are in the buffer.
    fn place(&mut self, values: &[T]) -> Range<usize> {
        let start = self.values.len();
        self.values.extend_from_slice(values);
        start..self.values.len()
    }

    /// The buffer, copied to the device.
    fn upload(self, stream: &Arc<CudaStream>) -> Result<Uploaded<T>, Error> {
        Uploaded::new(stream, &self.values)
    }
}

/// Values copied to device memory, and their device address.
#[derive(Debug)]
struct Uploaded<T> {
    buffer: CudaSlice<T>,
    address: u64,
}

impl<T: DeviceRepr> Uploaded<T> {
    /// `values`, copied to the device.
    fn new(stream: &Arc<CudaStream>, values: &[T]) -> Result<Self, Error> {
        Ok(Self::from_buffer(stream, stream.clone_htod(values)?))
    }

    /// `len` zeroed values on the device.
    fn zeros(stream: &Arc<CudaStream>, len: usize) -> Result<Self, Error>
    where
        T: ValidAsZeroBits,
    {
        Ok(Self::from_buffer(stream, stream.alloc_zeros(len)?))
    }

    fn from_buffer(stream: &Arc<CudaStream>, buffer: CudaSlice<T>) -> Self {
        let (address, _) = buffer.device_ptr(stream);
        Self { buffer, address }
    }

    /// The values of `range`, as the device reads them, as values of `U`:
    /// `T` itself, or the record a `DeviceCopy` wraps.
    #[expect(
        unsafe_code,
        reason = "hands the device a slice of this buffer by address (ADR 0007, ADR 0028)"
    )]
    fn slice<U>(&self, range: Range<usize>) -> DeviceSlice<U> {
        const { assert!(size_of::<U>() == size_of::<T>(), "a `T` is a `U`") };
        assert!(range.end <= self.buffer.len(), "{range:?} past the buffer");
        let offset = u64::try_from(range.start * size_of::<T>()).expect("an offset fits u64");
        let len = u64::try_from(range.len()).expect("a length fits u64");
        // SAFETY: `range` is within the buffer, whose values are
        // initialised and aligned, and whose `T`s are `U`s, byte for byte.
        // The buffer lives as long as the Renderer, or the batch, whose
        // launches read it, and no launch writes what a stage reads as a
        // slice.
        unsafe { DeviceSlice::from_raw_parts(self.address + offset, len) }
    }

    /// Every value, as [`Self::slice`] hands them over.
    fn whole<U>(&self) -> DeviceSlice<U> {
        self.slice(0..self.buffer.len())
    }
}

/// The queue between the stages, one entry per Pixel Sample of a batch:
/// each field an array of its own.
struct Queue {
    lambda: Uploaded<f32>,
    pdf: Uploaded<f32>,
    radiance: Uploaded<f32>,
    filter_weight: Uploaded<f32>,
}

impl Queue {
    /// The queue of `len` Pixel Samples, zeroed.
    fn new(stream: &Arc<CudaStream>, len: usize) -> Result<Self, Error> {
        Ok(Self {
            lambda: Uploaded::zeros(stream, N_SPECTRUM_SAMPLES * len)?,
            pdf: Uploaded::zeros(stream, N_SPECTRUM_SAMPLES * len)?,
            radiance: Uploaded::zeros(stream, N_SPECTRUM_SAMPLES * len)?,
            filter_weight: Uploaded::zeros(stream, len)?,
        })
    }

    /// The queue as the stages read and write it.
    fn record(&self) -> PixelSampleQueue {
        PixelSampleQueue {
            lambda: lanes(&self.lambda),
            pdf: lanes(&self.pdf),
            radiance: lanes(&self.radiance),
            filter_weight: self.filter_weight.whole(),
        }
    }
}

/// An array of `N_SPECTRUM_SAMPLES · n` `f32`s as `n` values of a lane per
/// wavelength.
#[expect(
    unsafe_code,
    reason = "hands the device a slice of this buffer by address (ADR 0007, ADR 0028)"
)]
fn lanes(values: &Uploaded<f32>) -> DeviceSlice<Lanes> {
    let len = u64::try_from(values.buffer.len() / N_SPECTRUM_SAMPLES).expect("a length fits u64");
    // SAFETY: the buffer holds `N_SPECTRUM_SAMPLES · len` initialised `f32`s,
    // whose alignment is that of `Lanes`; it lives as long as the batch.
    unsafe { DeviceSlice::from_raw_parts(values.address, len) }
}

/// A batch of Work Units, laid out for the stages: each Work Unit's Pixel
/// Samples after the previous one's.
struct Batch {
    records: Vec<DeviceCopy<WorkUnitRecord>>,
    pixel_samples: usize,
}

impl Batch {
    /// The batch of `units`.
    fn new(decomposition: &Decomposition, units: &[WorkUnit]) -> Result<Self, Error> {
        let mut pixel_samples = 0;
        let records = units
            .iter()
            .map(|&unit| {
                assert!(
                    decomposition.contains(unit),
                    "{unit:?} is not in this Render Job"
                );
                let bounds = decomposition.tile_bounds(unit.tile);
                let samples = decomposition.sample_indices(unit.pass);
                let record = WorkUnitRecord {
                    tile_min: bounds.min,
                    tile_width: u32::try_from(bounds.width())
                        .map_err(|_| Error::TooMany("pixels"))?,
                    first_sample_index: samples.start,
                    samples_per_pixel: samples.end - samples.start,
                    first_pixel_sample: u32::try_from(pixel_samples)
                        .map_err(|_| Error::TooMany("Pixel Samples"))?,
                };
                pixel_samples += bounds.area() * samples.len();
                Ok(DeviceCopy(record))
            })
            .collect::<Result<_, Error>>()?;
        Ok(Self {
            records,
            pixel_samples,
        })
    }

    /// The Work Result of each Work Unit of `units`, from the film samples
    /// of every Pixel Sample of the batch.
    fn results(
        &self,
        units: &[WorkUnit],
        film_samples: &[DeviceCopy<FilmSampleRecord>],
    ) -> Vec<Rendered> {
        let ends = self
            .records
            .iter()
            .skip(1)
            .map(|DeviceCopy(record)| record.first_pixel_sample as usize)
            .chain([self.pixel_samples]);
        units
            .iter()
            .zip(&self.records)
            .zip(ends)
            .map(|((&unit, DeviceCopy(record)), end)| {
                let samples = &film_samples[record.first_pixel_sample as usize..end];
                let pixels = samples
                    .chunks_exact(record.samples_per_pixel as usize)
                    .map(pixel_sum)
                    .collect();
                Rendered::Done(WorkResult::new(unit, pixels))
            })
            .collect()
    }
}

/// The sum of one pixel's film samples, as the CPU sums them (`PixelSum`).
fn pixel_sum(samples: &[DeviceCopy<FilmSampleRecord>]) -> PixelSum {
    samples
        .iter()
        .fold(PixelSum::default(), |mut sum, DeviceCopy(sample)| {
            let [r, g, b] = sample.rgb;
            sum += FilmSample {
                rgb: RGB::new(r, g, b),
                weight: sample.weight,
            };
            sum
        })
}
