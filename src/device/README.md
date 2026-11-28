# device

What a device builds its render data from, and the interface every device
implements (see `docs/adr/0013-devices-render-from-their-own-memory.md`). It
holds no OpenUSD type and owns no host copy: the render delegate,
`umbra::delegate`, converts what its prims sync into host copies, and hands
every device views of them at each commit.

The devices implement its interface in modules of their own, which depend
on it and never the reverse: the CPU device is `umbra::device::cpu`
(`src/device/cpu/`), and the GPU device `umbra::device::gpu`
(`src/device/gpu/`).

Public headers (`include/umbra/device/`):

- `render_data.h`: the views of what a device builds its render data from:
  prototypes with their instances, lights, cameras, material networks and
  textures, each named by a dense id per kind. Each attribute is one array:
  points are `Float3` at a 16-byte stride, one array per motion sample, and
  every numeric array is aligned to 16 bytes.
- `change_set.h`: the change set of a commit, the prims added, updated and
  removed by id, with the change flags of an updated prototype.
- `device.h`: the `Device` interface, which commits a change set into the
  device's own memory and renders a render product from it, and the frame
  parameters a device receives for each product besides render data: its
  image, its samples per pixel and the render buffers of the vars it
  renders. `TileQueue` is the counter every device takes the product's
  tiles from, in batches, 64×64 pixels of its data window each. A device finishes each tile once it has copied it
  into the render buffers, which marks its pixels final for a display and
  counts it in the product's progress phase
  (docs/adr/0014-frames-render-in-tiles-from-one-counter.md,
  docs/adr/0015-tiles-reach-the-render-buffers-as-they-finish.md).
- `accel_update.h`: `PlanAccelUpdate`, how every device decides, at an
  update, to keep, refit or rebuild a prototype's acceleration structure,
  and why: it rebuilds it when the topology changes, when it was built for
  tracing alone, when the number of motion samples or of points changes,
  and after `kMaxRefits`, 8, refits in a row, since every refit loosens its
  bounds. Each device builds, refits and logs in its own way; the GPU
  module builds `accel_update.cc` in.
- `image.h`: the image of a render product: its resolution, its data
  window, the pixels whose centers lie in its dataWindowNDC, the quantity
  each of its render vars holds (color, depth, normal and albedo), and how a
  camera's aperture conforms to it.
- `geometry.h`: points, vectors and affine transforms, row vectors as in
  OpenUSD, for single-source code.
- `camera_rays.h`: the camera model a device makes once per product, and
  `GenerateRay`, single-source, the ray through a pixel at its samples: the
  pixel filter's offset, the point of a thin lens and the time within the
  shutter. `CameraToNdc` gives OpenEXR's worldToNDC.
- `pixel_filter.h`: the pixel filter, single-source: Blackman-Harris, 1.5
  pixels wide, sampled by filter importance sampling, so every sample
  weighs the same.
- `sampler.h`: the sampler every device draws its samples from, OpenQMC's
  `PmjBnSampler`, behind one alias. Each device initializes its cache
  once, in its own memory.
- `light_shapes.h`: the shapes of the lights that rays meet (sphere, rect,
  disk and cylinder), in the light's own space: their bounds, and where a
  ray first meets them. CPU and GPU devices intersect them with the same
  code.
- `film.h`: the film every device renders a product into, single-source.
  `PlanFilm` lays out one float32 plane per render var, at most
  `kMaxFilmPlanes`, each as its var's render buffer, into a `FilmLayout` of
  fixed capacity, which a GPU reads as plain data. `PixelIndex` and
  `TileRowOffset` find a pixel and a tile's row in a plane and its render
  buffer, `WritePixel` writes a pixel into each plane, and
  `PlaceholderPixel`, a template over the device's trace of a camera ray,
  is the placeholder integrator's pixel: the mean over its samples of the
  color 0.5·direction + 0.5 with alpha 1, the depth of the first hit, +∞
  where a ray meets nothing, and its geometric normal.
- `single_source.h`: `UMBRA_HOST_DEVICE`, which marks the functions written
  once for the CPU and the GPU: clang compiles them for the CPU devices, and
  nvcc for the GPU devices. `UMBRA_NONBLOCKING` declares those of the hot
  path `[[clang::nonblocking]]` for clang, and nothing for nvcc. `single_source_check.cu` calls them from a
  kernel, for nvcc to compile them whenever the GPU module builds; nothing
  runs it.
- `diagnostic_device.h`: a device that renders nothing, takes no tile, and checks each
  change set it receives instead, against what it holds from the commits
  before: ids in range, an id removed or updated held and an id added free,
  an update carrying only the members its change flags name,
  cross-references that resolve, sizes and alignment. It logs what it
  received as the `device` component, at debug level, with a digest of what
  it holds that does not depend on the ids.
