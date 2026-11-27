# device

What a device builds its render data from, and the interface every device
implements (see `docs/adr/0013-devices-render-from-their-own-memory.md`). It
holds no OpenUSD type and owns no host copy: the OpenUSD bridge,
`umbra::ext::usd`, converts what its prims sync into host copies, and hands
every device views of them at each commit.

Public headers (`include/umbra/device/`):

- `render_data.h`: the views of what a device builds its render data from:
  prototypes with their instances, lights, cameras, material networks and
  textures, each named by a dense id per kind. Each attribute is one array:
  points are `Float3` at a 16-byte stride, one array per motion sample, and
  every numeric array is aligned to 16 bytes.
- `change_set.h`: the change set of a commit, the prims added, updated and
  removed by id, with the change flags of an updated prototype.
- `device.h`: the `Device` interface, which commits a change set into the
  device's own memory, and the frame parameters a device receives for each
  frame besides render data. `kMaxRefits`, 8, is how many refits in a row
  every device makes of a prototype's acceleration structure as its points
  change, before it rebuilds it, since every refit loosens its bounds.
- `diagnostic_device.h`: a device that renders nothing, and checks each
  change set it receives instead, against what it holds from the commits
  before: ids in range, an id removed or updated held and an id added free,
  an update carrying only the members its change flags name,
  cross-references that resolve, sizes and alignment. It logs what it
  received as the `device` component, at debug level, with a digest of what
  it holds that does not depend on the ids.
