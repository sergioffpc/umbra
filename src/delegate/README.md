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
- `hydra.h`: the render delegate, which creates the prims, chooses the
  shutter and the dicing view, places the lights, and commits.
- `render_task.h`: the task that runs in HdEngine::Execute: it syncs, then
  renders each product through its render pass, into the render buffers
  it creates.
- `render_settings_prim.h`, `camera_prim.h`, `mesh_prim.h`,
  `unsupported_prim.h`: the RenderSettings, Camera and Mesh prims, and the
  rprims of the types umbra does not take, which are only counted.
- `mesh.h`, `instancer.h`, `light.h`, `material.h`: the prims' sync into
  their host copies (`host_copy.h`).
- `gf.h`: Gf's vectors and matrices as the numbers the host copies hold:
  their components, and transforms row-major, in meters.
- `scene_index.h`: a prim's data source in the terminal scene index, for
  what the scene delegate API does not give.
- `commit.h`, `views.h`: the commit of the host copies to the devices, as
  the change set.
- `device_threads.h`: one host thread per device, its floating point
  configured, on which each device commits and renders.
- `ledger.h`: what the devices hold after a commit, which the next one
  starts from: a held table per kind of prim, by key, with dense ids.
- `commit_cameras.h`, `commit_textures.h`, `commit_materials.h`,
  `commit_geometry.h`, `commit_lights.h`: what a commit gives the devices
  of each kind, from the synced prims and the ledger.
- `commit_products.h`: the render products the render settings commit, as
  the render writes them.
- `commit_warnings.h`: the warnings of the commits, each logged once.
- `commit_description.h`: the debug description of a commit.
- `plural.h`: the words of a count in the messages logged.
- `render_pass.h`: the render pass and the render buffers; it runs the
  denoise of a product's color once the devices have rendered it.
- `denoiser.h`: the denoise through Open Image Denoise, through the OIDN
  bridge, `umbra::ext::oidn`: the device it runs on, its three filters,
  its buffers and its progress.
- `outputs.h`: the files of the render products.
- `ies_profile.h`, `ies_tilt.h`: IES profiles and their TILT data.
- `motion.h`, `motion_samples.h`: motion within the shutter.
- `subdivision.h`, `dicing.h`: the refinement of subdivision surfaces
  through OpenSubdiv, diced to the dicing rate for the view of the active
  camera (`docs/adr/0016-subdivision-surfaces-are-diced-in-the-sync.md`).
- `sync_counter.h`: the phase the prims count their syncs into.
