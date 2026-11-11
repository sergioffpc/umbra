//! OptiX on the host: the driver's `libnvoptix.so.1` loaded at run time, and
//! one safe wrapper per call Umbra makes (ADR 0007, as amended by #126).
//!
//! The NVIDIA driver ships OptiX; Umbra links nothing from NVIDIA at build
//! time and ships nothing from it (ADR 0026). In a container, OptiX needs the
//! NVIDIA Container Toolkit's `graphics` driver capability (arc42 §7).

mod ffi;

use std::ffi::{CStr, c_char, c_uint, c_void};
use std::ptr;
use std::rc::Rc;

use libloading::Library;

use crate::Error;

pub(crate) use ffi::{DevicePointer, SBT_RECORD_HEADER_SIZE, TraversableHandle};

/// The size of the log buffer handed to the calls that compile.
const LOG_SIZE: usize = 4096;

/// The pipeline's launch-parameter variable, `params` in `umbra-kernels`.
const LAUNCH_PARAMS_VARIABLE: &CStr = c"params";

/// The loaded OptiX library and its function table.
pub(crate) struct Api {
    table: ffi::FunctionTable,
    // Kept loaded for as long as the table's pointers are used.
    _library: Library,
}

impl std::fmt::Debug for Api {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Api").finish_non_exhaustive()
    }
}

impl Api {
    /// Loads the driver's OptiX and queries its function table for ABI 105.
    #[expect(
        unsafe_code,
        reason = "loading OptiX from the driver at run time (ADR 0007, ADR 0028)"
    )]
    pub(crate) fn load() -> Result<Rc<Self>, Error> {
        // SAFETY: `libnvoptix.so.1` is the NVIDIA driver's OptiX library,
        // whose initialisers have no precondition on the process.
        let library = unsafe { Library::new("libnvoptix.so.1") }.map_err(Error::OptixLibrary)?;
        // SAFETY: the symbol has the type `ffi::QueryFunctionTable`, which
        // OptiX's ABI fixes.
        let query = unsafe { library.get::<ffi::QueryFunctionTable>(b"optixQueryFunctionTable\0") }
            .map_err(Error::OptixLibrary)?;
        let mut table = ffi::FunctionTable::default();
        // SAFETY: `table` is a writable `FunctionTable` of the size passed,
        // the layout of ABI 105 that `ABI_VERSION` asks for; no options.
        let result = unsafe {
            query(
                ffi::ABI_VERSION,
                0,
                ptr::null(),
                ptr::null(),
                ptr::from_mut(&mut table).cast(),
                size_of::<ffi::FunctionTable>(),
            )
        };
        if result != ffi::SUCCESS {
            return Err(Error::Optix {
                call: "optixQueryFunctionTable",
                result: format!("code {result}"),
                log: String::new(),
            });
        }
        Ok(Rc::new(Self {
            table,
            _library: library,
        }))
    }

    /// `Ok` on success, otherwise the error of `call`, with the name OptiX
    /// gives `result` and the log of a compiling call.
    fn check(
        &self,
        call: &'static str,
        result: ffi::OptixResult,
        log: &[c_char],
    ) -> Result<(), Error> {
        if result == ffi::SUCCESS {
            return Ok(());
        }
        Err(Error::Optix {
            call,
            result: self.error_name(result),
            log: log_text(log),
        })
    }

    /// The name OptiX gives `result`, such as `OPTIX_ERROR_INVALID_VALUE`.
    #[expect(
        unsafe_code,
        reason = "OptiX's host API through its function table (ADR 0007, ADR 0028)"
    )]
    fn error_name(&self, result: ffi::OptixResult) -> String {
        let Some(get_error_name) = self.table.get_error_name else {
            return format!("code {result}");
        };
        // SAFETY: `optixGetErrorName` takes any result code and returns a
        // static, NUL-terminated string.
        let name = unsafe { get_error_name(result) };
        if name.is_null() {
            return format!("code {result}");
        }
        // SAFETY: a non-null name is a static C string, as above.
        unsafe { CStr::from_ptr(name) }
            .to_string_lossy()
            .into_owned()
    }
}

/// The text OptiX wrote to a log buffer, up to its NUL.
fn log_text(log: &[c_char]) -> String {
    let bytes: Vec<u8> = log
        .iter()
        .take_while(|&&c| c != 0)
        .map(|&c| c.to_ne_bytes()[0])
        .collect();
    String::from_utf8_lossy(&bytes).into_owned()
}

/// The function in `slot` of the table, or the error that the driver's
/// OptiX lacks `name`.
fn function<F>(slot: Option<F>, name: &'static str) -> Result<F, Error> {
    slot.ok_or(Error::OptixFunction(name))
}

/// Whether OptiX checks every call and launch: in builds with debug
/// assertions, where speed matters less than finding misuse.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum Validation {
    /// No checks.
    Off,
    /// OptiX's validation mode: every check, slowly.
    All,
}

/// An `OptixDeviceContext` on a CUDA context, destroyed on drop.
#[derive(Debug)]
pub(crate) struct DeviceContext {
    api: Rc<Api>,
    handle: ffi::Handle,
}

impl DeviceContext {
    /// The OptiX context of the CUDA context `cu_context`.
    #[expect(
        unsafe_code,
        reason = "OptiX's host API through its function table (ADR 0007, ADR 0028)"
    )]
    pub(crate) fn new(
        api: &Rc<Api>,
        cu_context: *mut c_void,
        validation: Validation,
    ) -> Result<Rc<Self>, Error> {
        let create = function(api.table.device_context_create, "optixDeviceContextCreate")?;
        let options = ffi::DeviceContextOptions {
            log_callback: [0; 2],
            log_callback_level: 0,
            validation_mode: match validation {
                Validation::Off => ffi::VALIDATION_MODE_OFF,
                Validation::All => ffi::VALIDATION_MODE_ALL,
            },
        };
        let mut handle = ptr::null_mut();
        // SAFETY: `cu_context` is a live CUDA context the caller keeps alive
        // for as long as `Self`; `options` and `handle` are valid for the
        // call.
        let result = unsafe { create(cu_context, &raw const options, &raw mut handle) };
        api.check("optixDeviceContextCreate", result, &[])?;
        Ok(Rc::new(Self {
            api: Rc::clone(api),
            handle,
        }))
    }

    /// The function table this context's calls go through.
    pub(crate) fn api(&self) -> &Api {
        &self.api
    }
}

impl Drop for DeviceContext {
    #[expect(
        unsafe_code,
        reason = "OptiX's host API through its function table (ADR 0007, ADR 0028)"
    )]
    fn drop(&mut self) {
        if let Some(destroy) = self.api.table.device_context_destroy {
            // SAFETY: `handle` is a context `new` created, destroyed once,
            // after every object created from it, each of which holds an
            // `Rc` of this context.
            unsafe { destroy(self.handle) };
        }
    }
}

/// The compile options shared by the module and the pipeline, which OptiX
/// requires to be the same: one GAS traced directly, two payload values (the
/// hit's distance and kind), triangles and custom primitives.
fn pipeline_compile_options() -> ffi::PipelineCompileOptions {
    ffi::PipelineCompileOptions {
        uses_motion_blur: 0,
        traversable_graph_flags: ffi::TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS,
        num_payload_values: 2,
        // OptiX's minimum, which triangles' barycentrics use.
        num_attribute_values: 2,
        exception_flags: 0,
        pipeline_launch_params_variable_name: LAUNCH_PARAMS_VARIABLE.as_ptr(),
        uses_primitive_type_flags: ffi::PRIMITIVE_TYPE_FLAGS_TRIANGLE
            | ffi::PRIMITIVE_TYPE_FLAGS_CUSTOM,
        allow_opacity_micromaps: 0,
        allow_clustered_geometry: 0,
    }
}

/// An `OptixModule` compiled from PTX, destroyed on drop.
#[derive(Debug)]
pub(crate) struct Module {
    context: Rc<DeviceContext>,
    handle: ffi::Handle,
}

impl Module {
    /// Compiles `ptx` into a module of `context`.
    #[expect(
        unsafe_code,
        reason = "OptiX's host API through its function table (ADR 0007, ADR 0028)"
    )]
    pub(crate) fn new(context: &Rc<DeviceContext>, ptx: &str) -> Result<Rc<Self>, Error> {
        let api = context.api();
        let create = function(api.table.module_create, "optixModuleCreate")?;
        let module_options = ffi::ModuleCompileOptions {
            max_register_count: 0,
            opt_level: 0,
            debug_level: 0,
            unused: [0; 3],
        };
        let pipeline_options = pipeline_compile_options();
        let mut log = [0; LOG_SIZE];
        let mut log_size = log.len();
        let mut handle = ptr::null_mut();
        // SAFETY: every pointer is valid for the call: the options, `ptx`
        // with its length, the log with its size, and `handle`.
        let result = unsafe {
            create(
                context.handle,
                &raw const module_options,
                &raw const pipeline_options,
                ptx.as_ptr().cast(),
                ptx.len(),
                log.as_mut_ptr(),
                &raw mut log_size,
                &raw mut handle,
            )
        };
        api.check("optixModuleCreate", result, &log)?;
        Ok(Rc::new(Self {
            context: Rc::clone(context),
            handle,
        }))
    }
}

impl Drop for Module {
    #[expect(
        unsafe_code,
        reason = "OptiX's host API through its function table (ADR 0007, ADR 0028)"
    )]
    fn drop(&mut self) {
        if let Some(destroy) = self.context.api().table.module_destroy {
            // SAFETY: `handle` is a module `new` created, destroyed once,
            // after the program groups that hold an `Rc` of it.
            unsafe { destroy(self.handle) };
        }
    }
}

/// The programs of one program group, by their entry names in its module.
#[derive(Debug, Clone, Copy)]
pub(crate) enum Programs {
    /// A ray-generation program.
    Raygen(&'static CStr),
    /// A miss program.
    Miss(&'static CStr),
    /// A hit group: a closest-hit program and, for a custom primitive, an
    /// intersection program.
    Hitgroup {
        /// The closest-hit program.
        closest_hit: &'static CStr,
        /// The intersection program, or `None` for built-in triangles.
        intersection: Option<&'static CStr>,
    },
}

impl Programs {
    /// The descriptor OptiX takes, for programs in `module`.
    fn desc(self, module: ffi::Handle) -> ffi::ProgramGroupDesc {
        let single = |name: &CStr| ffi::ProgramGroupSingleModule {
            module,
            entry_function_name: name.as_ptr(),
        };
        let none = ffi::ProgramGroupSingleModule {
            module: ptr::null_mut(),
            entry_function_name: ptr::null(),
        };
        let (kind, programs) = match self {
            Self::Raygen(name) => (
                ffi::PROGRAM_GROUP_KIND_RAYGEN,
                ffi::ProgramGroupPrograms {
                    single: single(name),
                },
            ),
            Self::Miss(name) => (
                ffi::PROGRAM_GROUP_KIND_MISS,
                ffi::ProgramGroupPrograms {
                    single: single(name),
                },
            ),
            Self::Hitgroup {
                closest_hit,
                intersection,
            } => (
                ffi::PROGRAM_GROUP_KIND_HITGROUP,
                ffi::ProgramGroupPrograms {
                    hitgroup: ffi::ProgramGroupHitgroup {
                        closest_hit: single(closest_hit),
                        any_hit: none,
                        intersection: intersection.map_or(none, single),
                    },
                },
            ),
        };
        ffi::ProgramGroupDesc {
            kind,
            flags: 0,
            programs,
        }
    }
}

/// An `OptixProgramGroup`, destroyed on drop.
#[derive(Debug)]
pub(crate) struct ProgramGroup {
    module: Rc<Module>,
    handle: ffi::Handle,
}

impl ProgramGroup {
    /// The program group of `programs`, in `module`.
    #[expect(
        unsafe_code,
        reason = "OptiX's host API through its function table (ADR 0007, ADR 0028)"
    )]
    pub(crate) fn new(module: &Rc<Module>, programs: Programs) -> Result<Rc<Self>, Error> {
        let api = module.context.api();
        let create = function(api.table.program_group_create, "optixProgramGroupCreate")?;
        let desc = programs.desc(module.handle);
        let options = ffi::ProgramGroupOptions {
            payload_type: ptr::null(),
        };
        let mut log = [0; LOG_SIZE];
        let mut log_size = log.len();
        let mut handle = ptr::null_mut();
        // SAFETY: one descriptor whose module and entry names outlive the
        // call, the options, the log with its size, and one output handle.
        let result = unsafe {
            create(
                module.context.handle,
                &raw const desc,
                1,
                &raw const options,
                log.as_mut_ptr(),
                &raw mut log_size,
                &raw mut handle,
            )
        };
        api.check("optixProgramGroupCreate", result, &log)?;
        Ok(Rc::new(Self {
            module: Rc::clone(module),
            handle,
        }))
    }

    /// The 32-byte header of a shader binding table record for this group.
    #[expect(
        unsafe_code,
        reason = "OptiX's host API through its function table (ADR 0007, ADR 0028)"
    )]
    pub(crate) fn record_header(&self) -> Result<[u8; SBT_RECORD_HEADER_SIZE], Error> {
        let api = self.module.context.api();
        let pack = function(api.table.sbt_record_pack_header, "optixSbtRecordPackHeader")?;
        let mut header = [0; SBT_RECORD_HEADER_SIZE];
        // SAFETY: `header` is `OPTIX_SBT_RECORD_HEADER_SIZE` writable bytes.
        let result = unsafe { pack(self.handle, header.as_mut_ptr().cast()) };
        api.check("optixSbtRecordPackHeader", result, &[])?;
        Ok(header)
    }
}

impl Drop for ProgramGroup {
    #[expect(
        unsafe_code,
        reason = "OptiX's host API through its function table (ADR 0007, ADR 0028)"
    )]
    fn drop(&mut self) {
        if let Some(destroy) = self.module.context.api().table.program_group_destroy {
            // SAFETY: `handle` is a group `new` created, destroyed once,
            // after the pipelines that hold an `Rc` of it.
            unsafe { destroy(self.handle) };
        }
    }
}

/// An `OptixPipeline`, destroyed on drop.
#[derive(Debug)]
pub(crate) struct Pipeline {
    context: Rc<DeviceContext>,
    // Destroyed after the pipeline, which `drop` destroys first.
    _groups: Vec<Rc<ProgramGroup>>,
    handle: ffi::Handle,
}

impl Pipeline {
    /// The pipeline of `groups`, which trace no further than one level:
    /// the closest-hit and miss programs trace no rays of their own.
    #[expect(
        unsafe_code,
        reason = "OptiX's host API through its function table (ADR 0007, ADR 0028)"
    )]
    pub(crate) fn new(
        context: &Rc<DeviceContext>,
        groups: &[&Rc<ProgramGroup>],
    ) -> Result<Self, Error> {
        let api = context.api();
        let create = function(api.table.pipeline_create, "optixPipelineCreate")?;
        let compile_options = pipeline_compile_options();
        let link_options = ffi::PipelineLinkOptions { max_trace_depth: 1 };
        let handles: Vec<ffi::Handle> = groups.iter().map(|group| group.handle).collect();
        let count =
            c_uint::try_from(handles.len()).map_err(|_| Error::TooMany("program groups"))?;
        let mut log = [0; LOG_SIZE];
        let mut log_size = log.len();
        let mut handle = ptr::null_mut();
        // SAFETY: the options, `count` program groups of this context, the
        // log with its size, and one output handle are valid for the call.
        let result = unsafe {
            create(
                context.handle,
                &raw const compile_options,
                &raw const link_options,
                handles.as_ptr(),
                count,
                log.as_mut_ptr(),
                &raw mut log_size,
                &raw mut handle,
            )
        };
        api.check("optixPipelineCreate", result, &log)?;
        Ok(Self {
            context: Rc::clone(context),
            _groups: groups.iter().map(|&group| Rc::clone(group)).collect(),
            handle,
        })
    }

    /// Launches `width` threads of the ray-generation program on `stream`,
    /// with the `LaunchParams` at device address `params`, and the records of
    /// `sbt`.
    #[expect(
        unsafe_code,
        reason = "OptiX's host API through its function table (ADR 0007, ADR 0028)"
    )]
    pub(crate) fn launch(
        &self,
        stream: *mut c_void,
        params: Launch,
        sbt: &ShaderBindingTable,
    ) -> Result<(), Error> {
        let api = self.context.api();
        let launch = function(api.table.launch, "optixLaunch")?;
        let table = sbt.ffi();
        // SAFETY: `params` points at `params.size` bytes of device memory and
        // `sbt` at records built for this pipeline, all kept alive by the
        // caller until the stream is synchronised.
        let result = unsafe {
            launch(
                self.handle,
                stream,
                params.address,
                params.size,
                &raw const table,
                params.width,
                1,
                1,
            )
        };
        api.check("optixLaunch", result, &[])
    }
}

impl Drop for Pipeline {
    #[expect(
        unsafe_code,
        reason = "OptiX's host API through its function table (ADR 0007, ADR 0028)"
    )]
    fn drop(&mut self) {
        if let Some(destroy) = self.context.api().table.pipeline_destroy {
            // SAFETY: `handle` is a pipeline `new` created, destroyed once.
            unsafe { destroy(self.handle) };
        }
    }
}

/// The launch parameters and size of one launch.
#[derive(Debug, Clone, Copy)]
pub(crate) struct Launch {
    /// The device address of the launch parameters.
    pub(crate) address: DevicePointer,
    /// Their size in bytes.
    pub(crate) size: usize,
    /// The number of threads, along `x`.
    pub(crate) width: c_uint,
}

/// The records a launch uses: one ray-generation record, one miss record
/// and the hit group records, each by device address and stride.
#[derive(Debug, Clone, Copy)]
pub(crate) struct ShaderBindingTable {
    /// The ray-generation record.
    pub(crate) raygen: DevicePointer,
    /// The miss record.
    pub(crate) miss: DevicePointer,
    /// The stride of a miss record.
    pub(crate) miss_stride: c_uint,
    /// The hit group record.
    pub(crate) hitgroup: DevicePointer,
    /// The stride of a hit group record.
    pub(crate) hitgroup_stride: c_uint,
}

impl ShaderBindingTable {
    /// The table OptiX takes: one miss and one hit group record.
    fn ffi(self) -> ffi::ShaderBindingTable {
        ffi::ShaderBindingTable {
            raygen_record: self.raygen,
            exception_record: 0,
            miss_record_base: self.miss,
            miss_record_stride_in_bytes: self.miss_stride,
            miss_record_count: 1,
            hitgroup_record_base: self.hitgroup,
            hitgroup_record_stride_in_bytes: self.hitgroup_stride,
            hitgroup_record_count: 1,
            callables: [0; 2],
        }
    }
}

/// The geometry of one geometry acceleration structure (GAS): one build
/// input, one SBT record, any-hit programs off.
#[derive(Debug, Clone, Copy)]
pub(crate) enum GasInput {
    /// Triangles: `count` vertices, three `f32` each, at a device address.
    Triangles {
        /// The vertices' device address.
        vertices: DevicePointer,
        /// The number of vertices, three per triangle.
        count: c_uint,
    },
    /// Custom primitives: `count` boxes, six `f32` each (`OptixAabb`), at a
    /// device address.
    CustomPrimitives {
        /// The boxes' device address.
        aabbs: DevicePointer,
        /// The number of primitives.
        count: c_uint,
    },
}

/// The sizes of the buffers a GAS build needs.
#[derive(Debug, Clone, Copy)]
pub(crate) struct GasSizes {
    /// The size of the GAS itself.
    pub(crate) output: usize,
    /// The size of the scratch buffer of the build.
    pub(crate) temp: usize,
}

impl DeviceContext {
    /// The sizes of the buffers the GAS of `input` needs.
    #[expect(
        unsafe_code,
        reason = "OptiX's host API through its function table (ADR 0007, ADR 0028)"
    )]
    pub(crate) fn gas_sizes(&self, input: GasInput) -> Result<GasSizes, Error> {
        let compute = function(
            self.api.table.accel_compute_memory_usage,
            "optixAccelComputeMemoryUsage",
        )?;
        let options = accel_build_options();
        let mut sizes = ffi::AccelBufferSizes::default();
        let (build_input, _keep) = build_input(&input);
        // SAFETY: one build input whose host arrays (`_keep`) outlive the
        // call, the options and the output sizes are valid.
        let result = unsafe {
            compute(
                self.handle,
                &raw const options,
                &raw const build_input,
                1,
                &raw mut sizes,
            )
        };
        self.api
            .check("optixAccelComputeMemoryUsage", result, &[])?;
        Ok(GasSizes {
            output: sizes.output_size_in_bytes,
            temp: sizes.temp_size_in_bytes,
        })
    }

    /// Builds the GAS of `input` on `stream` into `output`, with `temp` as
    /// scratch space, both of the sizes [`Self::gas_sizes`] gave and
    /// 128-byte aligned; returns its traversable handle. The build is
    /// complete once the stream is synchronised.
    #[expect(
        unsafe_code,
        reason = "OptiX's host API through its function table (ADR 0007, ADR 0028)"
    )]
    pub(crate) fn build_gas(
        &self,
        stream: *mut c_void,
        input: GasInput,
        temp: (DevicePointer, usize),
        output: (DevicePointer, usize),
    ) -> Result<TraversableHandle, Error> {
        let build = function(self.api.table.accel_build, "optixAccelBuild")?;
        let options = accel_build_options();
        let (build_input, _keep) = build_input(&input);
        let mut handle = 0;
        // SAFETY: one build input whose host arrays (`_keep`) outlive the
        // call, device buffers of the sizes OptiX asked for, kept alive by
        // the caller until the stream is synchronised, and one output handle.
        let result = unsafe {
            build(
                self.handle,
                stream,
                &raw const options,
                &raw const build_input,
                1,
                temp.0,
                temp.1,
                output.0,
                output.1,
                &raw mut handle,
                ptr::null(),
                0,
            )
        };
        self.api.check("optixAccelBuild", result, &[])?;
        Ok(handle)
    }
}

/// Build options for a static GAS traced many times.
fn accel_build_options() -> ffi::AccelBuildOptions {
    ffi::AccelBuildOptions {
        build_flags: ffi::BUILD_FLAG_PREFER_FAST_TRACE,
        operation: ffi::BUILD_OPERATION_BUILD,
        motion_options: [0; 3],
    }
}

/// The host arrays a build input points into: its one buffer address and
/// its one geometry flag. Boxed, so the pointers stay valid as it moves.
type BuildInputArrays = Box<(DevicePointer, c_uint)>;

/// The build input of `input`, and the host arrays it points into, which
/// the caller keeps alive for as long as it uses the input.
fn build_input(input: &GasInput) -> (ffi::BuildInput, BuildInputArrays) {
    let (buffer, count) = match *input {
        GasInput::Triangles { vertices, count } => (vertices, count),
        GasInput::CustomPrimitives { aabbs, count } => (aabbs, count),
    };
    let arrays = Box::new((buffer, ffi::GEOMETRY_FLAG_DISABLE_ANYHIT));
    let buffers = &raw const arrays.0;
    let flags = &raw const arrays.1;
    let (kind, array) = match *input {
        GasInput::Triangles { .. } => (
            ffi::BUILD_INPUT_TYPE_TRIANGLES,
            ffi::BuildInputArray {
                triangles: ffi::BuildInputTriangleArray {
                    vertex_buffers: buffers,
                    num_vertices: count,
                    vertex_format: ffi::VERTEX_FORMAT_FLOAT3,
                    vertex_stride_in_bytes: 0,
                    indices: [0; 4],
                    flags,
                    num_sbt_records: 1,
                    unused: [0; 21],
                },
            },
        ),
        GasInput::CustomPrimitives { .. } => (
            ffi::BUILD_INPUT_TYPE_CUSTOM_PRIMITIVES,
            ffi::BuildInputArray {
                custom_primitives: ffi::BuildInputCustomPrimitiveArray {
                    aabb_buffers: buffers,
                    num_primitives: count,
                    stride_in_bytes: 0,
                    flags,
                    num_sbt_records: 1,
                    unused: [0; 3],
                },
            },
        ),
    };
    (ffi::BuildInput { kind, array }, arrays)
}
