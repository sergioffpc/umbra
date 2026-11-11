//! OptiX 9.0's host ABI (ABI version 105): the function table that
//! `optixQueryFunctionTable` fills, and the structs and constants the calls
//! Umbra makes take.
//!
//! These are interoperability facts of the ABI, written by hand from OptiX's
//! documentation and headers; no header text is copied (ADR 0026, NOTICE).
//! Only what Umbra calls is typed: the other slots of the table are
//! placeholders of the same size, and the structs' unused members are zeroed
//! arrays of the same size and alignment. Each struct's size, and the offset
//! of every member Umbra sets, is asserted against the C layout.

use core::ffi::{c_char, c_int, c_uint, c_void};
use core::mem::offset_of;

/// The ABI version Umbra's table layout is for: OptiX 9.0.
pub(super) const ABI_VERSION: c_int = 105;

/// `OptixResult`: 0 is success, anything else an error code.
pub(super) type OptixResult = c_int;
/// `OPTIX_SUCCESS`.
pub(super) const SUCCESS: OptixResult = 0;

/// `CUdeviceptr`.
pub(crate) type DevicePointer = u64;
/// `OptixTraversableHandle`.
pub(crate) type TraversableHandle = u64;
/// `CUcontext`, `CUstream` and OptiX's opaque handles.
pub(super) type Handle = *mut c_void;

/// `OptixQueryFunctionTable_t`, the one symbol `libnvoptix.so.1` exports.
pub(super) type QueryFunctionTable = unsafe extern "C" fn(
    abi_id: c_int,
    num_options: c_uint,
    option_keys: *const c_void,
    option_values: *const *const c_void,
    function_table: *mut c_void,
    size_of_table: usize,
) -> OptixResult;

/// `optixGetErrorName`.
pub(super) type GetErrorName = unsafe extern "C" fn(OptixResult) -> *const c_char;
/// `optixDeviceContextCreate`.
pub(super) type DeviceContextCreate =
    unsafe extern "C" fn(Handle, *const DeviceContextOptions, *mut Handle) -> OptixResult;
/// `optixDeviceContextDestroy`, `optixModuleDestroy`,
/// `optixProgramGroupDestroy` and `optixPipelineDestroy`.
pub(super) type Destroy = unsafe extern "C" fn(Handle) -> OptixResult;
/// `optixModuleCreate`.
pub(super) type ModuleCreate = unsafe extern "C" fn(
    Handle,
    *const ModuleCompileOptions,
    *const PipelineCompileOptions,
    *const c_char,
    usize,
    *mut c_char,
    *mut usize,
    *mut Handle,
) -> OptixResult;
/// `optixProgramGroupCreate`.
pub(super) type ProgramGroupCreate = unsafe extern "C" fn(
    Handle,
    *const ProgramGroupDesc,
    c_uint,
    *const ProgramGroupOptions,
    *mut c_char,
    *mut usize,
    *mut Handle,
) -> OptixResult;
/// `optixPipelineCreate`.
pub(super) type PipelineCreate = unsafe extern "C" fn(
    Handle,
    *const PipelineCompileOptions,
    *const PipelineLinkOptions,
    *const Handle,
    c_uint,
    *mut c_char,
    *mut usize,
    *mut Handle,
) -> OptixResult;
/// `optixAccelComputeMemoryUsage`.
pub(super) type AccelComputeMemoryUsage = unsafe extern "C" fn(
    Handle,
    *const AccelBuildOptions,
    *const BuildInput,
    c_uint,
    *mut AccelBufferSizes,
) -> OptixResult;
/// `optixAccelBuild`.
pub(super) type AccelBuild = unsafe extern "C" fn(
    Handle,
    Handle,
    *const AccelBuildOptions,
    *const BuildInput,
    c_uint,
    DevicePointer,
    usize,
    DevicePointer,
    usize,
    *mut TraversableHandle,
    *const c_void,
    c_uint,
) -> OptixResult;
/// `optixSbtRecordPackHeader`.
pub(super) type SbtRecordPackHeader = unsafe extern "C" fn(Handle, *mut c_void) -> OptixResult;
/// `optixLaunch`.
pub(super) type Launch = unsafe extern "C" fn(
    Handle,
    Handle,
    DevicePointer,
    usize,
    *const ShaderBindingTable,
    c_uint,
    c_uint,
    c_uint,
) -> OptixResult;

/// A slot of the function table Umbra does not call.
type Unused = Option<unsafe extern "C" fn()>;

/// `OptixFunctionTable` of ABI 105: 52 function pointers, in order.
#[derive(Debug, Default)]
#[repr(C)]
pub(super) struct FunctionTable {
    /// Slot 0, `optixGetErrorName`.
    pub(super) get_error_name: Option<GetErrorName>,
    slot_1: Unused,
    /// Slot 2, `optixDeviceContextCreate`.
    pub(super) device_context_create: Option<DeviceContextCreate>,
    /// Slot 3, `optixDeviceContextDestroy`.
    pub(super) device_context_destroy: Option<Destroy>,
    slots_4_to_11: [Unused; 8],
    /// Slot 12, `optixModuleCreate`.
    pub(super) module_create: Option<ModuleCreate>,
    slots_13_and_14: [Unused; 2],
    /// Slot 15, `optixModuleDestroy`.
    pub(super) module_destroy: Option<Destroy>,
    slots_16_and_17: [Unused; 2],
    /// Slot 18, `optixProgramGroupCreate`.
    pub(super) program_group_create: Option<ProgramGroupCreate>,
    /// Slot 19, `optixProgramGroupDestroy`.
    pub(super) program_group_destroy: Option<Destroy>,
    slot_20: Unused,
    /// Slot 21, `optixPipelineCreate`.
    pub(super) pipeline_create: Option<PipelineCreate>,
    /// Slot 22, `optixPipelineDestroy`.
    pub(super) pipeline_destroy: Option<Destroy>,
    slot_23: Unused,
    /// Slot 24, `optixAccelComputeMemoryUsage`.
    pub(super) accel_compute_memory_usage: Option<AccelComputeMemoryUsage>,
    /// Slot 25, `optixAccelBuild`.
    pub(super) accel_build: Option<AccelBuild>,
    slots_26_to_39: [Unused; 14],
    /// Slot 40, `optixSbtRecordPackHeader`.
    pub(super) sbt_record_pack_header: Option<SbtRecordPackHeader>,
    /// Slot 41, `optixLaunch`.
    pub(super) launch: Option<Launch>,
    slots_42_to_51: [Unused; 10],
}

const _: () = {
    let slot = size_of::<Unused>();
    assert!(
        size_of::<FunctionTable>() == 52 * slot,
        "ABI 105's table has 52 slots"
    );
    assert!(
        offset_of!(FunctionTable, device_context_create) == 2 * slot,
        "slot 2"
    );
    assert!(
        offset_of!(FunctionTable, module_create) == 12 * slot,
        "slot 12"
    );
    assert!(
        offset_of!(FunctionTable, module_destroy) == 15 * slot,
        "slot 15"
    );
    assert!(
        offset_of!(FunctionTable, program_group_create) == 18 * slot,
        "slot 18"
    );
    assert!(
        offset_of!(FunctionTable, pipeline_create) == 21 * slot,
        "slot 21"
    );
    assert!(
        offset_of!(FunctionTable, accel_compute_memory_usage) == 24 * slot,
        "slot 24"
    );
    assert!(
        offset_of!(FunctionTable, sbt_record_pack_header) == 40 * slot,
        "slot 40"
    );
    assert!(offset_of!(FunctionTable, launch) == 41 * slot, "slot 41");
};

/// `OptixDeviceContextOptions`, with no log callback.
#[derive(Debug)]
#[repr(C)]
pub(super) struct DeviceContextOptions {
    /// `logCallbackFunction`, `logCallbackData`: none.
    pub(super) log_callback: [usize; 2],
    /// `logCallbackLevel`.
    pub(super) log_callback_level: c_int,
    /// `validationMode`: [`VALIDATION_MODE_OFF`] or [`VALIDATION_MODE_ALL`].
    pub(super) validation_mode: c_uint,
}

/// `OPTIX_DEVICE_CONTEXT_VALIDATION_MODE_OFF`.
pub(super) const VALIDATION_MODE_OFF: c_uint = 0;
/// `OPTIX_DEVICE_CONTEXT_VALIDATION_MODE_ALL`.
pub(super) const VALIDATION_MODE_ALL: c_uint = 0xFFFF_FFFF;

const _: () = {
    assert!(size_of::<DeviceContextOptions>() == 24, "24 bytes");
    assert!(
        offset_of!(DeviceContextOptions, log_callback_level) == 16,
        "byte 16"
    );
    assert!(
        offset_of!(DeviceContextOptions, validation_mode) == 20,
        "byte 20"
    );
};

/// `OptixModuleCompileOptions`.
#[derive(Debug)]
#[repr(C)]
pub(super) struct ModuleCompileOptions {
    /// `maxRegisterCount`: 0 lets OptiX choose.
    pub(super) max_register_count: c_int,
    /// `optLevel`: `OPTIX_COMPILE_OPTIMIZATION_DEFAULT` is 0.
    pub(super) opt_level: c_uint,
    /// `debugLevel`: `OPTIX_COMPILE_DEBUG_LEVEL_DEFAULT` is 0.
    pub(super) debug_level: c_uint,
    /// `boundValues`, `numBoundValues`, `numPayloadTypes`, `payloadTypes`:
    /// none.
    pub(super) unused: [usize; 3],
}

const _: () = {
    assert!(size_of::<ModuleCompileOptions>() == 40, "40 bytes");
    assert!(offset_of!(ModuleCompileOptions, unused) == 16, "byte 16");
};

/// `OptixPipelineCompileOptions`.
#[derive(Debug)]
#[repr(C)]
pub(super) struct PipelineCompileOptions {
    /// `usesMotionBlur`.
    pub(super) uses_motion_blur: c_int,
    /// `traversableGraphFlags`.
    pub(super) traversable_graph_flags: c_uint,
    /// `numPayloadValues`.
    pub(super) num_payload_values: c_int,
    /// `numAttributeValues`.
    pub(super) num_attribute_values: c_int,
    /// `exceptionFlags`: `OPTIX_EXCEPTION_FLAG_NONE` is 0.
    pub(super) exception_flags: c_uint,
    /// `pipelineLaunchParamsVariableName`.
    pub(super) pipeline_launch_params_variable_name: *const c_char,
    /// `usesPrimitiveTypeFlags`.
    pub(super) uses_primitive_type_flags: c_uint,
    /// `allowOpacityMicromaps`.
    pub(super) allow_opacity_micromaps: c_int,
    /// `allowClusteredGeometry`.
    pub(super) allow_clustered_geometry: c_int,
}

/// `OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS`.
pub(super) const TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS: c_uint = 1;
/// `OPTIX_PRIMITIVE_TYPE_FLAGS_CUSTOM`.
pub(super) const PRIMITIVE_TYPE_FLAGS_CUSTOM: c_uint = 1;
/// `OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE`.
pub(super) const PRIMITIVE_TYPE_FLAGS_TRIANGLE: c_uint = 1 << 31;

const _: () = {
    assert!(size_of::<PipelineCompileOptions>() == 48, "48 bytes");
    assert!(
        offset_of!(PipelineCompileOptions, pipeline_launch_params_variable_name) == 24,
        "byte 24"
    );
    assert!(
        offset_of!(PipelineCompileOptions, uses_primitive_type_flags) == 32,
        "byte 32"
    );
    assert!(
        offset_of!(PipelineCompileOptions, allow_clustered_geometry) == 40,
        "byte 40"
    );
};

/// `OptixProgramGroupSingleModule`: a ray-generation, miss or exception
/// program.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub(super) struct ProgramGroupSingleModule {
    /// `module`.
    pub(super) module: Handle,
    /// `entryFunctionName`.
    pub(super) entry_function_name: *const c_char,
}

/// `OptixProgramGroupHitgroup`: closest-hit, any-hit and intersection
/// programs, each a module and an entry name, or two nulls.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub(super) struct ProgramGroupHitgroup {
    /// `moduleCH`, `entryFunctionNameCH`.
    pub(super) closest_hit: ProgramGroupSingleModule,
    /// `moduleAH`, `entryFunctionNameAH`.
    pub(super) any_hit: ProgramGroupSingleModule,
    /// `moduleIS`, `entryFunctionNameIS`.
    pub(super) intersection: ProgramGroupSingleModule,
}

/// The anonymous union of `OptixProgramGroupDesc`, whose largest member is
/// the hit group.
#[derive(Clone, Copy)]
#[repr(C)]
pub(super) union ProgramGroupPrograms {
    /// `raygen`, `miss` or `exception`.
    pub(super) single: ProgramGroupSingleModule,
    /// `hitgroup`.
    pub(super) hitgroup: ProgramGroupHitgroup,
}

/// `OptixProgramGroupDesc`.
#[repr(C)]
pub(super) struct ProgramGroupDesc {
    /// `kind`: one of the `PROGRAM_GROUP_KIND_*`.
    pub(super) kind: c_uint,
    /// `flags`: `OPTIX_PROGRAM_GROUP_FLAGS_NONE` is 0.
    pub(super) flags: c_uint,
    /// The programs.
    pub(super) programs: ProgramGroupPrograms,
}

/// `OPTIX_PROGRAM_GROUP_KIND_RAYGEN`.
pub(super) const PROGRAM_GROUP_KIND_RAYGEN: c_uint = 0x2421;
/// `OPTIX_PROGRAM_GROUP_KIND_MISS`.
pub(super) const PROGRAM_GROUP_KIND_MISS: c_uint = 0x2422;
/// `OPTIX_PROGRAM_GROUP_KIND_HITGROUP`.
pub(super) const PROGRAM_GROUP_KIND_HITGROUP: c_uint = 0x2424;

const _: () = {
    assert!(size_of::<ProgramGroupDesc>() == 56, "56 bytes");
    assert!(offset_of!(ProgramGroupDesc, programs) == 8, "byte 8");
    assert!(
        size_of::<ProgramGroupPrograms>() == 48,
        "the hit group's 48 bytes"
    );
};

/// `OptixProgramGroupOptions`, with the default payload type.
#[derive(Debug)]
#[repr(C)]
pub(super) struct ProgramGroupOptions {
    /// `payloadType`: null.
    pub(super) payload_type: *const c_void,
}

/// `OptixPipelineLinkOptions`.
#[derive(Debug)]
#[repr(C)]
pub(super) struct PipelineLinkOptions {
    /// `maxTraceDepth`.
    pub(super) max_trace_depth: c_uint,
}

/// `OptixBuildInputTriangleArray`, without index buffer, pre-transform,
/// per-primitive SBT offsets or micromaps.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub(super) struct BuildInputTriangleArray {
    /// `vertexBuffers`: host array of one device address per motion key.
    pub(super) vertex_buffers: *const DevicePointer,
    /// `numVertices`.
    pub(super) num_vertices: c_uint,
    /// `vertexFormat`.
    pub(super) vertex_format: c_uint,
    /// `vertexStrideInBytes`: 0 for tightly packed.
    pub(super) vertex_stride_in_bytes: c_uint,
    /// `indexBuffer`, `numIndexTriplets`, `indexFormat`,
    /// `indexStrideInBytes`, `preTransform`: zero.
    pub(super) indices: [u64; 4],
    /// `flags`: host array of one `OPTIX_GEOMETRY_FLAG_*` per SBT record.
    pub(super) flags: *const c_uint,
    /// `numSbtRecords`.
    pub(super) num_sbt_records: c_uint,
    /// `sbtIndexOffsetBuffer` to `transformFormat`, `opacityMicromap`,
    /// `displacementMicromap`: zero.
    pub(super) unused: [u64; 21],
}

/// `OPTIX_VERTEX_FORMAT_FLOAT3`.
pub(super) const VERTEX_FORMAT_FLOAT3: c_uint = 0x2121;
/// `OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT`.
pub(super) const GEOMETRY_FLAG_DISABLE_ANYHIT: c_uint = 1;

const _: () = {
    assert!(size_of::<BuildInputTriangleArray>() == 240, "240 bytes");
    assert!(
        offset_of!(BuildInputTriangleArray, num_vertices) == 8,
        "byte 8"
    );
    assert!(
        offset_of!(BuildInputTriangleArray, vertex_format) == 12,
        "byte 12"
    );
    assert!(
        offset_of!(BuildInputTriangleArray, indices) == 24,
        "byte 24"
    );
    assert!(offset_of!(BuildInputTriangleArray, flags) == 56, "byte 56");
    assert!(
        offset_of!(BuildInputTriangleArray, num_sbt_records) == 64,
        "byte 64"
    );
    assert!(offset_of!(BuildInputTriangleArray, unused) == 72, "byte 72");
};

/// `OptixBuildInputCustomPrimitiveArray`, without per-primitive SBT
/// offsets.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub(super) struct BuildInputCustomPrimitiveArray {
    /// `aabbBuffers`: host array of one device address per motion key.
    pub(super) aabb_buffers: *const DevicePointer,
    /// `numPrimitives`.
    pub(super) num_primitives: c_uint,
    /// `strideInBytes`: 0 for tightly packed `OptixAabb`s.
    pub(super) stride_in_bytes: c_uint,
    /// `flags`: host array of one `OPTIX_GEOMETRY_FLAG_*` per SBT record.
    pub(super) flags: *const c_uint,
    /// `numSbtRecords`.
    pub(super) num_sbt_records: c_uint,
    /// `sbtIndexOffsetBuffer` to `primitiveIndexOffset`: zero.
    pub(super) unused: [u64; 3],
}

const _: () = {
    assert!(
        size_of::<BuildInputCustomPrimitiveArray>() == 56,
        "56 bytes"
    );
    assert!(
        offset_of!(BuildInputCustomPrimitiveArray, flags) == 16,
        "byte 16"
    );
    assert!(
        offset_of!(BuildInputCustomPrimitiveArray, num_sbt_records) == 24,
        "byte 24"
    );
};

/// The anonymous union of `OptixBuildInput`, padded to 1024 bytes.
#[derive(Clone, Copy)]
#[repr(C)]
pub(super) union BuildInputArray {
    /// `triangleArray`.
    pub(super) triangles: BuildInputTriangleArray,
    /// `customPrimitiveArray`.
    pub(super) custom_primitives: BuildInputCustomPrimitiveArray,
    /// `pad`.
    pub(super) pad: [u8; 1024],
}

/// `OptixBuildInput`.
#[repr(C)]
pub(super) struct BuildInput {
    /// `type`: one of the `BUILD_INPUT_TYPE_*`.
    pub(super) kind: c_uint,
    /// The array of that type.
    pub(super) array: BuildInputArray,
}

/// `OPTIX_BUILD_INPUT_TYPE_TRIANGLES`.
pub(super) const BUILD_INPUT_TYPE_TRIANGLES: c_uint = 0x2141;
/// `OPTIX_BUILD_INPUT_TYPE_CUSTOM_PRIMITIVES`.
pub(super) const BUILD_INPUT_TYPE_CUSTOM_PRIMITIVES: c_uint = 0x2142;

const _: () = {
    assert!(size_of::<BuildInput>() == 1032, "1032 bytes");
    assert!(offset_of!(BuildInput, array) == 8, "byte 8");
};

/// `OptixAccelBuildOptions`, without motion.
#[derive(Debug)]
#[repr(C)]
pub(super) struct AccelBuildOptions {
    /// `buildFlags`.
    pub(super) build_flags: c_uint,
    /// `operation`.
    pub(super) operation: c_uint,
    /// `motionOptions` (`numKeys`, `flags`, `timeBegin`, `timeEnd`): zero.
    pub(super) motion_options: [u32; 3],
}

/// `OPTIX_BUILD_FLAG_PREFER_FAST_TRACE`.
pub(super) const BUILD_FLAG_PREFER_FAST_TRACE: c_uint = 1 << 2;
/// `OPTIX_BUILD_OPERATION_BUILD`.
pub(super) const BUILD_OPERATION_BUILD: c_uint = 0x2161;

const _: () = assert!(size_of::<AccelBuildOptions>() == 20, "20 bytes");

/// `OptixAccelBufferSizes`.
#[derive(Debug, Default)]
#[expect(clippy::struct_field_names, reason = "OptiX's member names")]
#[repr(C)]
pub(super) struct AccelBufferSizes {
    /// `outputSizeInBytes`.
    pub(super) output_size_in_bytes: usize,
    /// `tempSizeInBytes`.
    pub(super) temp_size_in_bytes: usize,
    /// `tempUpdateSizeInBytes`.
    pub(super) temp_update_size_in_bytes: usize,
}

/// `OptixShaderBindingTable`, without exception or callable records.
#[derive(Debug)]
#[repr(C)]
pub(super) struct ShaderBindingTable {
    /// `raygenRecord`.
    pub(super) raygen_record: DevicePointer,
    /// `exceptionRecord`.
    pub(super) exception_record: DevicePointer,
    /// `missRecordBase`.
    pub(super) miss_record_base: DevicePointer,
    /// `missRecordStrideInBytes`.
    pub(super) miss_record_stride_in_bytes: c_uint,
    /// `missRecordCount`.
    pub(super) miss_record_count: c_uint,
    /// `hitgroupRecordBase`.
    pub(super) hitgroup_record_base: DevicePointer,
    /// `hitgroupRecordStrideInBytes`.
    pub(super) hitgroup_record_stride_in_bytes: c_uint,
    /// `hitgroupRecordCount`.
    pub(super) hitgroup_record_count: c_uint,
    /// `callablesRecordBase`, `callablesRecordStrideInBytes`,
    /// `callablesRecordCount`.
    pub(super) callables: [u64; 2],
}

const _: () = {
    assert!(size_of::<ShaderBindingTable>() == 64, "64 bytes");
    assert!(
        offset_of!(ShaderBindingTable, miss_record_base) == 16,
        "byte 16"
    );
    assert!(
        offset_of!(ShaderBindingTable, hitgroup_record_base) == 32,
        "byte 32"
    );
    assert!(offset_of!(ShaderBindingTable, callables) == 48, "byte 48");
};

/// `OPTIX_SBT_RECORD_HEADER_SIZE`.
pub(crate) const SBT_RECORD_HEADER_SIZE: usize = 32;
