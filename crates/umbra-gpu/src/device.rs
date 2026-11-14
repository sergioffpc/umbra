//! What the Tracer and the Renderer share on a GPU: the programs' PTX, the
//! CUDA stream and OptiX context they run on, their shader binding table
//! records and their launches.

use std::ffi::c_void;
use std::rc::Rc;
use std::sync::Arc;

use cudarc::driver::{CudaContext, CudaSlice, CudaStream, DevicePtr, DeviceRepr};

use crate::Error;
use crate::optix::{
    Api, DeviceContext, Launch, Module, Pipeline, ProgramGroup, ShaderBindingTable, Validation,
};
use crate::records::SbtRecord;

/// The PTX of `umbra-gpu-programs`, which the build script compiles.
const PTX: &str = include_str!(concat!(env!("OUT_DIR"), "/umbra_gpu_programs.ptx"));

/// A GPU, ready to run the programs: its default stream, an OptiX context
/// on it, and the module of `umbra-gpu-programs` in that context.
pub(crate) struct Device {
    pub(crate) stream: Arc<CudaStream>,
    pub(crate) context: Rc<DeviceContext>,
    pub(crate) module: Rc<Module>,
}

impl Device {
    /// GPU `ordinal`, ready to run the programs.
    ///
    /// # Errors
    ///
    /// [`Error::UnlinkedPtx`] when the crate was built without `libdevice`,
    /// [`Error::Cuda`] without a CUDA driver or device,
    /// [`Error::OptixLibrary`] or [`Error::OptixFunction`] without a driver
    /// new enough for OptiX 9.0, and [`Error::Optix`] when OptiX rejects the
    /// programs.
    pub(crate) fn open(ordinal: usize) -> Result<Self, Error> {
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
        let module = Module::new(&context, PTX)?;
        Ok(Self {
            stream,
            context,
            module,
        })
    }
}

/// The shader binding table record of `group` with `data`, on the device.
pub(crate) fn record<T>(
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

/// Launches `width` threads of `pipeline` on `stream`, with the launch
/// parameters `params` and the records of `sbt`, and waits for them.
pub(crate) fn launch<P: DeviceRepr>(
    pipeline: &Pipeline,
    stream: &Arc<CudaStream>,
    params: P,
    width: u32,
    sbt: &ShaderBindingTable,
) -> Result<(), Error> {
    let params = stream.clone_htod(&[params])?;
    let (address, _params_read) = params.device_ptr(stream);
    let launch = Launch {
        address,
        size: size_of::<P>(),
        width,
    };
    pipeline.launch(stream_handle(stream), launch, sbt)?;
    stream.synchronize()?;
    Ok(())
}

/// A record's size as the `u32` stride OptiX takes; records are a few
/// hundred bytes.
pub(crate) fn stride(size: usize) -> u32 {
    u32::try_from(size).expect("an SBT record is far smaller than 4 GiB")
}

/// The stream as the `CUstream` OptiX takes.
pub(crate) fn stream_handle(stream: &CudaStream) -> *mut c_void {
    stream.cu_stream().cast()
}
