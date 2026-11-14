//! The host's side of the records it shares with the device: the
//! `DeviceRepr` promise that lets `cudarc` copy them, and what a hit record
//! says.

use cudarc::driver::DeviceRepr;
use umbra_geometry::Sphere;
use umbra_gpu_programs::{
    FilmSampleRecord, HitKind, HitRecord, LaunchParams, RayRecord, SamplerRecord, SceneRecord,
    WavefrontParams, WorkUnitRecord,
};

use crate::Error;
use crate::optix::SBT_RECORD_HEADER_SIZE;
use crate::{Geometry, Hit};

/// A value of the layout `umbra-gpu-programs` shares with the device, as
/// `cudarc` copies it: the type is that crate's, so the `DeviceRepr`
/// promise is made for this wrapper, which has its layout.
#[derive(Debug, Clone, Copy, Default)]
#[repr(transparent)]
pub(crate) struct DeviceCopy<T>(pub(crate) T);

/// A shader binding table record: the header OptiX packs for a program
/// group, then the data its programs read with `optixGetSbtDataPointer`.
/// OptiX wants records 16-byte aligned (`OPTIX_SBT_RECORD_ALIGNMENT`).
#[derive(Debug, Clone, Copy)]
#[repr(C, align(16))]
pub(crate) struct SbtRecord<T> {
    /// The header from `optixSbtRecordPackHeader`.
    pub(crate) header: [u8; SBT_RECORD_HEADER_SIZE],
    /// The record's data.
    pub(crate) data: T,
}

const _: () = {
    assert!(
        size_of::<SbtRecord<()>>() == 32,
        "a record without data is its header"
    );
    assert!(
        size_of::<SbtRecord<Sphere>>() == 304,
        "a sphere's record is its header and the 272 bytes of `Sphere`"
    );
};

/// Implements `DeviceRepr` for `cudarc`, which copies values of these types
/// to and from device memory byte for byte.
macro_rules! device_repr {
    ($($t:ty),+ $(,)?) => {$(
        // SAFETY: a `#[repr(C)]` type of `f32`s, integers, byte arrays and
        // device addresses, or a transparent wrapper of one, with no pointer
        // to host memory, whose layout the device shares (the assertions of
        // `umbra-gpu-programs`' `launch.rs` and `wavefront.rs`, and of the
        // core types they hold). Its padding bytes are copied as they are
        // and never read as values; of these types, the host reads back
        // only `HitRecord` and `FilmSampleRecord`, which have no padding and
        // for which any bits are a valid value.
        #[expect(unsafe_code, reason = "copying records to and from the GPU (ADR 0007, ADR 0028)")]
        unsafe impl DeviceRepr for $t {}
    )+};
}

device_repr!(
    DeviceCopy<LaunchParams>,
    DeviceCopy<RayRecord>,
    DeviceCopy<HitRecord>,
    DeviceCopy<SceneRecord>,
    DeviceCopy<SamplerRecord>,
    DeviceCopy<WorkUnitRecord>,
    DeviceCopy<FilmSampleRecord>,
    DeviceCopy<WavefrontParams>,
    SbtRecord<()>,
    SbtRecord<Sphere>
);
#[cfg(feature = "math-program")]
device_repr!(DeviceCopy<umbra_gpu_programs::MathParams>);

impl DeviceCopy<HitRecord> {
    /// The hit this record holds, or `None` for a miss.
    pub(crate) fn hit(self) -> Result<Option<Hit>, Error> {
        let Self(HitRecord { t, kind }) = self;
        let kind = [HitKind::Miss, HitKind::Triangle, HitKind::Sphere]
            .into_iter()
            .find(|&k| k as u32 == kind)
            .ok_or(Error::HitKind(kind))?;
        let geometry = match kind {
            HitKind::Miss => return Ok(None),
            HitKind::Triangle => Geometry::Triangle,
            HitKind::Sphere => Geometry::Sphere,
        };
        Ok(Some(Hit { t, geometry }))
    }
}
