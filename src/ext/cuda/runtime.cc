// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/cuda/runtime.h"

#include <cuda_runtime_api.h>
#include <driver_types.h>

#include <expected>
#include <span>

#include "umbra/core/lazy_format.h"
#include "umbra/ext/cuda/errors.h"

namespace umbra::ext::cuda {

Result<int> DeviceCount(const core::LazyFormat& what) {
  int count = 0;
  return Check(cudaGetDeviceCount(&count), what).transform([&] {
    return count;
  });
}

Result<cudaDeviceProp> DeviceProperties(int ordinal,
                                        const core::LazyFormat& what) {
  cudaDeviceProp properties{};
  if (auto read = Check(cudaGetDeviceProperties(&properties, ordinal), what);
      !read.has_value()) {
    static_cast<void>(cudaGetLastError());
    return std::unexpected(read.error());
  }
  return properties;
}

Result<int> ComputeCapabilityMajor(int ordinal, const core::LazyFormat& what) {
  int major = 0;
  return Check(cudaDeviceGetAttribute(&major, cudaDevAttrComputeCapabilityMajor,
                                      ordinal),
               what)
      .transform([&] { return major; });
}

Result<void> PciBusId(int ordinal, std::span<char> bus_id,
                      const core::LazyFormat& what) {
  return Check(cudaDeviceGetPCIBusId(bus_id.data(),
                                     static_cast<int>(bus_id.size()), ordinal),
               what);
}

Result<int> DriverVersion(const core::LazyFormat& what) {
  int version = 0;
  return Check(cudaDriverGetVersion(&version), what).transform([&] {
    return version;
  });
}

Result<void> SetDevice(int ordinal, const core::LazyFormat& what) {
  return Check(cudaSetDevice(ordinal), what);
}

Result<void> Synchronize(cudaStream_t stream, const core::LazyFormat& what) {
  return Check(cudaStreamSynchronize(stream), what);
}

Result<void> Copy2DAsync(const Copy2D& copy, cudaStream_t stream,
                         const core::LazyFormat& what) {
  return Check(
      cudaMemcpy2DAsync(copy.to, copy.to_pitch, copy.from, copy.from_pitch,
                        copy.row_bytes, copy.rows, copy.kind, stream),
      what);
}

}  // namespace umbra::ext::cuda
