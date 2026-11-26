# A frame is rendered in tiles that devices take from one counter

This decision revises docs/adr/0013-devices-render-from-their-own-memory.md
on what devices share after the commit: besides the commit and the merge of
their films, they share one counter while a render product renders. It
settles how the work is split between the devices of a render, which
docs/adr/0004-render-devices.md left open.

umbra's render pass renders a frame one render product at a time, each with
its camera, its image and its render vars. It splits a product's data
window, the pixels it renders, into tiles of 64×64 pixels, in rows from the
top left; the tiles on the right and bottom edges are smaller when the
window is not a multiple of 64. The tiles are numbered in that order.

## One counter per render product

Every device of the render takes tiles from one atomic counter, the next
tile to render, which the render pass resets to zero for each product and
keeps in host memory, on a cache line of its own. Each device is driven by
a thread of the host, which takes a batch of consecutive tiles with one
`fetch_add` of the batch's size, has the device render them, and takes the
next batch, until the counter passes the last tile. A GPU's kernels never
read the counter: its host thread does.

Each kind of device has its own batch size, a constant chosen to fill its
hardware: about one tile per thread of its arena for a CPU device, enough
tiles for one launch to fill a GPU. The constants are tuned by measuring.
A device that renders faster takes more batches, so the faster devices
render more of the product, without umbra measuring their speeds during the
render.

The counter is the only data the devices share while a product renders,
and each device touches it once per batch, outside the hot path: no sample,
path or pixel reads it.

## Films and the merge

Each device renders its tiles into a film of its own, in its own memory: on
its node for a CPU device, in its memory for a GPU. A film covers the whole
data window, in float32, for every render var of the product, since no
device knows beforehand which tiles it will take. The device keeps its film
across the products and frames of a render, and reallocates it only between
products, when a data window or its vars need more than it holds.

When the counter is exhausted, each device copies the tiles it rendered,
and only those, from its film into the render buffers, in host memory, from
its own thread: a GPU through device-to-host copies. The tiles of two
devices never overlap, so the copies take no lock. The product is rendered
when every device has copied its tiles.

## Each sample counts for its own pixel

The pixel filter is sampled, not splatted: each sample's position within its
pixel follows the filter's distribution, and the sample counts with weight 1
for that pixel only. A pixel's value is the mean of its samples. No sample
writes to a neighbouring pixel, so no tile writes to another's pixels, and
a tile's pixels are final when the device has rendered it.

## Considered options

- **Equal static bands of rows, one per device.** No device would touch
  shared data at all, but the fastest device would wait for the slowest:
  a GPU and a CPU differ by an order of magnitude or more.
- **Bands sized by measured device speed.** It needs a benchmark or a first
  frame to measure, and the speed of a device changes with the content of
  its band.
- **Splitting the samples instead of the pixels**, every device rendering
  the whole image with part of the samples. The merge would add every
  film's pixels instead of copying tiles.
- **Tiles written into the render buffers as soon as they are rendered.**
  Devices would write host memory, across sockets and the PCIe bus, while
  others render, and a GPU would copy every tile on its own.
- **A queue under a lock**, with priorities or a spiral order from the
  center. The order of the tiles does not change the image, and a lock is
  more than one counter needs.
- **A splatted pixel filter**, each sample weighted into the pixels within
  the filter's radius. Each tile would need a margin that overlaps its
  neighbours', and the merge would add the margins instead of copying.

## Consequences

- The only data devices share while a product renders is one counter,
  touched once per batch, so the split costs nothing as the samples per
  pixel grow.
- Each device's film is as large as the data window, so host memory holds
  the window once per CPU device, and each GPU holds it once.
- The render buffers are written once per product, from the devices'
  threads.
- Since every sample weighs 1, the pixel filter must be non-negative: a
  filter with negative lobes would need signed weights.
- How evenly devices finish depends on each kind of device's batch size.
