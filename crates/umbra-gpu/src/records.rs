//! The host's side of the records it shares with the device: their
//! conversions, and the `DeviceRepr` promise that lets `cudarc` copy them.

use cudarc::driver::DeviceRepr;
use umbra_geometry::{Ray, Sphere};

use crate::Error;
use crate::launch::{HitKind, HitRecord, LaunchParams, RayRecord};
use crate::optix::SBT_RECORD_HEADER_SIZE;
use crate::{Geometry, Hit};

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
        // SAFETY: a `#[repr(C)]` type of `f32`s, integers and byte arrays,
        // with no pointer to host memory, whose layout the device shares
        // (`launch.rs`, `Sphere`'s assertions). Its padding bytes are copied
        // as they are and never read as values; of these types, the host
        // reads back only `HitRecord`, which has no padding and for which
        // any bits are a valid value.
        #[expect(unsafe_code, reason = "copying records to and from the GPU (ADR 0007, ADR 0028)")]
        unsafe impl DeviceRepr for $t {}
    )+};
}

device_repr!(
    LaunchParams,
    RayRecord,
    HitRecord,
    SbtRecord<()>,
    SbtRecord<Sphere>
);
#[cfg(feature = "math-program")]
device_repr!(crate::math_launch::MathParams);

impl From<&Ray> for RayRecord {
    fn from(ray: &Ray) -> Self {
        Self {
            o: [ray.o.x, ray.o.y, ray.o.z],
            d: [ray.d.x, ray.d.y, ray.d.z],
        }
    }
}

impl TryFrom<u32> for HitKind {
    type Error = Error;

    fn try_from(kind: u32) -> Result<Self, Self::Error> {
        [Self::Miss, Self::Triangle, Self::Sphere]
            .into_iter()
            .find(|&k| k as u32 == kind)
            .ok_or(Error::HitKind(kind))
    }
}

impl HitRecord {
    /// The hit this record holds, or `None` for a miss.
    pub(crate) fn hit(self) -> Result<Option<Hit>, Error> {
        let geometry = match HitKind::try_from(self.kind)? {
            HitKind::Miss => return Ok(None),
            HitKind::Triangle => Geometry::Triangle,
            HitKind::Sphere => Geometry::Sphere,
        };
        Ok(Some(Hit {
            t: self.t,
            geometry,
        }))
    }
}
