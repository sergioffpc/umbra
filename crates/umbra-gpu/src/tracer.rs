//! The tracer bullet of the GPU path (ADR 0026): one triangle and one
//! sphere, each in a geometry acceleration structure of its own, traced
//! through OptiX by the programs of `umbra-kernels`.

use std::ffi::c_void;
use std::rc::Rc;
use std::sync::Arc;

use cudarc::driver::{CudaContext, CudaSlice, CudaStream, DevicePtr, DevicePtrMut, DeviceRepr};
use umbra_geometry::{Ray, Sphere};
use umbra_math::Point3f;

use crate::launch::{HitRecord, LaunchParams, RayRecord};
use crate::optix::{
    Api, DeviceContext, GasInput, Launch, Module, Pipeline, ProgramGroup, Programs,
    ShaderBindingTable, TraversableHandle, Validation,
};
use crate::records::SbtRecord;
use crate::{Error, Geometry, Hit};

#[cfg(feature = "math-program")]
mod math;

/// The PTX of `umbra-kernels`, which the build script compiles.
const PTX: &str = include_str!(concat!(env!("OUT_DIR"), "/umbra_kernels.ptx"));

/// One triangle and one sphere on a GPU, ready to be traced.
///
/// Built once, then traced as often as needed; dropping it frees the GPU's
/// memory and OptiX's objects.
#[derive(Debug)]
pub struct Tracer {
    // Declared first, so that it drops before the CUDA memory below and the
    // context the stream holds.
    pipeline: Pipeline,
    raygen: CudaSlice<SbtRecord<()>>,
    #[cfg(feature = "math-program")]
    math_raygen: CudaSlice<SbtRecord<()>>,
    miss: CudaSlice<SbtRecord<()>>,
    triangle: Gas,
    triangle_hitgroup: CudaSlice<SbtRecord<()>>,
    sphere: Gas,
    sphere_hitgroup: CudaSlice<SbtRecord<Sphere>>,
    stream: Arc<CudaStream>,
}

/// A geometry acceleration structure and the device memory it lives in.
#[derive(Debug)]
struct Gas {
    handle: TraversableHandle,
    // The GAS and the vertices or boxes it was built from: kept for as long
    // as `handle` is traced.
    _buffer: CudaSlice<u8>,
    _geometry: CudaSlice<f32>,
}

/// The program groups of the pipeline.
struct Groups {
    raygen: Rc<ProgramGroup>,
    #[cfg(feature = "math-program")]
    math_raygen: Rc<ProgramGroup>,
    miss: Rc<ProgramGroup>,
    triangle: Rc<ProgramGroup>,
    sphere: Rc<ProgramGroup>,
}

impl Groups {
    /// Every group, for `Pipeline::new`.
    fn all(&self) -> Vec<&Rc<ProgramGroup>> {
        #[cfg(feature = "math-program")]
        let math = Some(&self.math_raygen);
        #[cfg(not(feature = "math-program"))]
        let math = None;
        [&self.raygen, &self.miss, &self.triangle, &self.sphere]
            .into_iter()
            .chain(math)
            .collect()
    }
}

impl Tracer {
    /// Builds the pipeline and the two acceleration structures on GPU
    /// `ordinal`: the triangle of `vertices` and `sphere`.
    ///
    /// # Errors
    ///
    /// [`Error::Cuda`] without a CUDA driver or device, [`Error::OptixLibrary`]
    /// or [`Error::OptixFunction`] without a driver new enough for OptiX 9.0,
    /// [`Error::UnlinkedPtx`] when the crate was built without `libdevice`,
    /// and [`Error::Optix`] when OptiX rejects the programs or a build.
    pub fn new(ordinal: usize, vertices: [Point3f; 3], sphere: &Sphere) -> Result<Self, Error> {
        if PTX.contains(".extern") {
            return Err(Error::UnlinkedPtx);
        }
        let cuda = CudaContext::new(ordinal)?;
        let stream = cuda.default_stream();
        let api = Api::load()?;
        let validation = if cfg!(debug_assertions) {
            Validation::All
        } else {
            Validation::Off
        };
        let context = DeviceContext::new(&api, cuda.cu_ctx().cast(), validation)?;
        let groups = program_groups(&context)?;
        let pipeline = Pipeline::new(&context, &groups.all())?;
        let triangle_vertices: Vec<f32> = vertices.iter().flat_map(|p| [p.x, p.y, p.z]).collect();
        let bounds = sphere.bounds();
        let sphere_box: Vec<f32> = [bounds.p_min, bounds.p_max]
            .iter()
            .flat_map(|p| [p.x, p.y, p.z])
            .collect();
        Ok(Self {
            pipeline,
            raygen: record(&stream, &groups.raygen, ())?,
            #[cfg(feature = "math-program")]
            math_raygen: record(&stream, &groups.math_raygen, ())?,
            miss: record(&stream, &groups.miss, ())?,
            triangle: build_gas(&context, &stream, &triangle_vertices, Geometry::Triangle)?,
            triangle_hitgroup: record(&stream, &groups.triangle, ())?,
            sphere: build_gas(&context, &stream, &sphere_box, Geometry::Sphere)?,
            sphere_hitgroup: record(&stream, &groups.sphere, *sphere)?,
            stream,
        })
    }

    /// Traces `rays` against `geometry` alone and returns, for each, its
    /// closest hit, or `None` where it misses.
    ///
    /// # Errors
    ///
    /// [`Error::TooMany`] beyond `u32::MAX` rays, [`Error::Cuda`] or
    /// [`Error::Optix`] when the launch fails, and [`Error::HitKind`] if the
    /// device writes a hit it should not.
    pub fn trace(&self, geometry: Geometry, rays: &[Ray]) -> Result<Vec<Option<Hit>>, Error> {
        if rays.is_empty() {
            return Ok(Vec::new());
        }
        let count = u32::try_from(rays.len()).map_err(|_| Error::TooMany("rays"))?;
        let records: Vec<RayRecord> = rays.iter().map(RayRecord::from).collect();
        let ray_buffer = self.stream.clone_htod(&records)?;
        let mut hit_buffer = self
            .stream
            .clone_htod(&vec![HitRecord::default(); rays.len()])?;
        self.trace_launch(geometry, count, &ray_buffer, &mut hit_buffer)?;
        self.stream
            .clone_dtoh(&hit_buffer)?
            .into_iter()
            .map(HitRecord::hit)
            .collect()
    }

    /// Launches one thread per ray of `rays` against `geometry`, writing
    /// `hits`, and waits for it.
    fn trace_launch(
        &self,
        geometry: Geometry,
        count: u32,
        rays: &CudaSlice<RayRecord>,
        hits: &mut CudaSlice<HitRecord>,
    ) -> Result<(), Error> {
        let (handle, sbt) = self.target(geometry);
        let (rays, _rays_read) = rays.device_ptr(&self.stream);
        let (hits, _hits_written) = hits.device_ptr_mut(&self.stream);
        let params = LaunchParams {
            handle,
            rays,
            hits,
            count,
        };
        self.launch(params, count, &sbt)
    }

    /// Launches `width` threads with the launch parameters `params` and the
    /// records of `sbt`, and waits for them.
    fn launch<P: DeviceRepr>(
        &self,
        params: P,
        width: u32,
        sbt: &ShaderBindingTable,
    ) -> Result<(), Error> {
        let params = self.stream.clone_htod(&[params])?;
        let (address, _params_read) = params.device_ptr(&self.stream);
        let launch = Launch {
            address,
            size: size_of::<P>(),
            width,
        };
        self.pipeline
            .launch(stream_handle(&self.stream), launch, sbt)?;
        self.stream.synchronize()?;
        Ok(())
    }

    /// The traversable of `geometry` and the shader binding table that
    /// traces it.
    fn target(&self, geometry: Geometry) -> (TraversableHandle, ShaderBindingTable) {
        let handle = match geometry {
            Geometry::Triangle => self.triangle.handle,
            Geometry::Sphere => self.sphere.handle,
        };
        (handle, self.binding_table(&self.raygen, geometry))
    }

    /// The shader binding table of launches by the ray-generation record
    /// `raygen`, with the miss record and the hit group record of
    /// `geometry`.
    fn binding_table(
        &self,
        raygen: &CudaSlice<SbtRecord<()>>,
        geometry: Geometry,
    ) -> ShaderBindingTable {
        let (raygen, _) = raygen.device_ptr(&self.stream);
        let (miss, _) = self.miss.device_ptr(&self.stream);
        let (hitgroup, hitgroup_stride) = match geometry {
            Geometry::Triangle => (
                self.triangle_hitgroup.device_ptr(&self.stream).0,
                size_of::<SbtRecord<()>>(),
            ),
            Geometry::Sphere => (
                self.sphere_hitgroup.device_ptr(&self.stream).0,
                size_of::<SbtRecord<Sphere>>(),
            ),
        };
        ShaderBindingTable {
            raygen,
            miss,
            miss_stride: stride(size_of::<SbtRecord<()>>()),
            hitgroup,
            hitgroup_stride: stride(hitgroup_stride),
        }
    }
}

/// A record's size as the `u32` stride OptiX takes; records are a few
/// hundred bytes.
fn stride(size: usize) -> u32 {
    u32::try_from(size).expect("an SBT record is far smaller than 4 GiB")
}

/// The stream as the `CUstream` OptiX takes.
fn stream_handle(stream: &CudaStream) -> *mut c_void {
    stream.cu_stream().cast()
}

/// The module of `umbra-kernels` and its program groups: four, and the math
/// program's with the `math-program` feature.
fn program_groups(context: &Rc<DeviceContext>) -> Result<Groups, Error> {
    let module = Module::new(context, PTX)?;
    Ok(Groups {
        raygen: ProgramGroup::new(&module, Programs::Raygen(c"__raygen__trace"))?,
        #[cfg(feature = "math-program")]
        math_raygen: ProgramGroup::new(&module, Programs::Raygen(c"__raygen__math"))?,
        miss: ProgramGroup::new(&module, Programs::Miss(c"__miss__trace"))?,
        triangle: ProgramGroup::new(
            &module,
            Programs::Hitgroup {
                closest_hit: c"__closesthit__trace",
                intersection: None,
            },
        )?,
        sphere: ProgramGroup::new(
            &module,
            Programs::Hitgroup {
                closest_hit: c"__closesthit__trace",
                intersection: Some(c"__intersection__sphere"),
            },
        )?,
    })
}

/// The shader binding table record of `group` with `data`, on the device.
fn record<T>(
    stream: &Arc<CudaStream>,
    group: &ProgramGroup,
    data: T,
) -> Result<CudaSlice<SbtRecord<T>>, Error>
where
    SbtRecord<T>: DeviceRepr,
{
    let record = SbtRecord {
        header: group.record_header()?,
        data,
    };
    Ok(stream.clone_htod(&[record])?)
}

/// Builds the GAS of `geometry`: a triangle's three vertices, or a custom
/// primitive's box (`OptixAabb`: minimum, then maximum), as `f32`s.
fn build_gas(
    context: &DeviceContext,
    stream: &Arc<CudaStream>,
    data: &[f32],
    geometry: Geometry,
) -> Result<Gas, Error> {
    let data = stream.clone_htod(data)?;
    let (handle, buffer) = {
        let (address, _data_read) = data.device_ptr(stream);
        let input = match geometry {
            Geometry::Triangle => GasInput::Triangles {
                vertices: address,
                count: count(data.len() / 3)?,
            },
            Geometry::Sphere => GasInput::CustomPrimitives {
                aabbs: address,
                count: count(data.len() / 6)?,
            },
        };
        build_gas_from_input(context, stream, input)?
    };
    Ok(Gas {
        handle,
        _buffer: buffer,
        _geometry: data,
    })
}

/// Builds the GAS of `input` into a buffer of its own, and waits for it.
fn build_gas_from_input(
    context: &DeviceContext,
    stream: &Arc<CudaStream>,
    input: GasInput,
) -> Result<(TraversableHandle, CudaSlice<u8>), Error> {
    let sizes = context.gas_sizes(input)?;
    let temp = stream.alloc_zeros::<u8>(sizes.temp)?;
    let mut buffer = stream.alloc_zeros::<u8>(sizes.output)?;
    let handle = {
        let (temp_address, _temp_read) = temp.device_ptr(stream);
        let (output_address, _output_written) = buffer.device_ptr_mut(stream);
        let handle = context.build_gas(
            stream_handle(stream),
            input,
            (temp_address, sizes.temp),
            (output_address, sizes.output),
        )?;
        stream.synchronize()?;
        handle
    };
    Ok((handle, buffer))
}

/// `n` as the `u32` count OptiX takes.
fn count(n: usize) -> Result<u32, Error> {
    u32::try_from(n).map_err(|_| Error::TooMany("primitives"))
}
