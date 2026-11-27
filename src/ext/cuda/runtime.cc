// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors

#include "umbra/ext/cuda/runtime.h"

#include <cuda_runtime_api.h>
#include <driver_types.h>

#include <expected>
#include <span>
#include <string_view>

#include "umbra/ext/cuda/errors.h"

namespace umbra::ext::cuda {

Result<int> DeviceCount(std::string_view what) {
  int count = 0;
  return Check(cudaGetDeviceCount(&count), what).transform([&] {
    return count;
  });
}

Result<cudaDeviceProp> DeviceProperties(int ordinal, std::string_view what) {
  cudaDeviceProp properties{};
  if (auto read = Check(cudaGetDeviceProperties(&properties, ordinal), what);
      !read.has_value()) {
    static_cast<void>(cudaGetLastError());
    return std::unexpected(read.error());
  }
  return properties;
}

Result<int> ComputeCapabilityMajor(int ordinal, std::string_view what) {
  int major = 0;
  return Check(cudaDeviceGetAttribute(&major, cudaDevAttrComputeCapabilityMajor,
                                      ordinal),
               what)
      .transform([&] { return major; });
}

Result<void> PciBusId(int ordinal, std::span<char> bus_id,
                      std::string_view what) {
  return Check(cudaDeviceGetPCIBusId(bus_id.data(),
                                     static_cast<int>(bus_id.size()), ordinal),
               what);
}

Result<int> DriverVersion(std::string_view what) {
  int version = 0;
  return Check(cudaDriverGetVersion(&version), what).transform([&] {
    return version;
  });
}

Result<void> SetDevice(int ordinal, std::string_view what) {
  return Check(cudaSetDevice(ordinal), what);
}

Result<void> Synchronize(cudaStream_t stream, std::string_view what) {
  return Check(cudaStreamSynchronize(stream), what);
}

Result<void> Copy2DAsync(const Copy2D& copy, cudaStream_t stream,
                         std::string_view what) {
  return Check(
      cudaMemcpy2DAsync(copy.to, copy.to_pitch, copy.from, copy.from_pitch,
                        copy.row_bytes, copy.rows, copy.kind, stream),
      what);
}

}  // namespace umbra::ext::cuda
