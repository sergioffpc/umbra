# delegate

umbra's render delegate, through which umbra renders as a Hydra renderer
(see `docs/adr/0012-umbra-is-a-hydra-renderer.md`): its prims convert what
they sync into host copies, which it commits to every device
(`docs/adr/0013-devices-render-from-their-own-memory.md`), and it renders
each render product through a render pass of its own. No OpenUSD type
leaves it.

It derives from Hydra's classes, so it is built as the bridges are, with
RTTI and exceptions, and catches every exception at its edge
(`docs/adr/0017-ext-is-a-thin-layer-over-each-dependency.md`). It includes
OpenUSD's headers; what can fail on OpenUSD's side goes through the OpenUSD
bridge, `umbra::ext::usd`, which opens its stage.

Public headers (`include/umbra/delegate/`):

- `session.h`: the API a render opens: `OpenSession` opens a stage into a
  `Session`, which renders it frame by frame on the devices, and `Display`
  is what shows the products while they render, tile by tile.
- `render_settings.h`: the render settings, render products and render vars
  the render uses, as the delegate reads them from the stage.

Sources (`src/delegate/`):

- `session.cc`: the session: the stage, the scene indices from it to the
  render index, and the render index on the delegate.
- `hydra.h`: the render delegate, its prims and the render task.
- `mesh.h`, `instancer.h`, `light.h`, `material.h`: the prims' sync into
  their host copies (`host_copy.h`).
- `commit.h`, `views.h`: the commit of the host copies to the devices, as
  the change set.
- `render_pass.h`: the render pass and the render buffers; it runs the
  denoise of a product's color once the devices have rendered it.
- `denoiser.h`: the denoise through Open Image Denoise, through the OIDN
  bridge, `umbra::ext::oidn`: the device it runs on, its three filters,
  its buffers and its progress.
- `outputs.h`: the files of the render products.
- `ies_profile.h`, `ies_tilt.h`: IES profiles and their TILT data.
- `motion.h`, `motion_samples.h`: motion within the shutter.
- `subdivision.h`, `dicing.h`: the refinement of subdivision surfaces
  through OpenSubdiv, diced to the dicing rate
  (`docs/adr/0016-subdivision-surfaces-are-diced-in-the-sync.md`).
- `sync_counter.h`: the phase the prims count their syncs into.
