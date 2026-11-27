# Tiles reach the render buffers as they finish

This decision revises the section "Films and the merge" of
docs/adr/0014-frames-render-in-tiles-from-one-counter.md: a device no longer
copies its tiles into the render buffers once the counter is exhausted, but
each tile as soon as it has rendered it, so that a display can show the
product while it renders.

## Each tile is copied, then finished

Each device still renders into a film of its own, in its own memory. When
it has rendered a tile, the device copies that tile's pixels from its film
into the render buffers, in host memory, from the thread that rendered it:
a CPU device from a worker of its arena, a GPU through a device-to-host copy
of the tiles of each launch. Then it marks the tile finished, in a flag of
the tile queue, with a release store, and counts it into the progress. The
tiles of two devices never overlap, so the copies take no lock, and neither
the copy nor the flag allocates.

A tile's pixels in the render buffers are final once its flag is set. A
thread that reads the flag with an acquire load may read those pixels while
the other tiles render. The product is rendered when every device has
returned.

## The display

A display, when the render has one, shows the product from a host thread of
its own, which the render pass starts as the product starts and stops once
every device has returned. Every few tens of milliseconds it looks for the
tiles finished since it last looked and hands their windows to the display,
which reads their pixels from the render buffers. Nothing the devices run
waits for it: a slow or lost display delays only its own thread.

## Considered options

- **Copying at the end of the product**, as before, and a display that
  shows only rendered products. A product could not be watched while it
  renders.
- **A queue of finished tiles**, which the devices push to and the display
  pops from. A flag per tile is simpler, takes no capacity to size, and the
  display needs no order.
- **The display reading the devices' films.** A GPU's film is in its own
  memory, and a CPU device's film is reused by the next product while the
  display may still read it.

## Consequences

- Devices write host memory while the other devices render: a CPU device
  across sockets when the render buffers are on another node, a GPU across
  the PCIe bus once per launch rather than once per product.
- The tile queue holds one byte per tile, allocated as the product starts,
  outside the hot path.
- A render without a display pays only the flag of each tile.
