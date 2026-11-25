# scene

The Scene: what umbra renders, in umbra's own types
(see `docs/adr/0006-scenes-enter-through-hydra.md`). The OpenUSD bridge,
`umbra::ext::usd::LoadScene`, builds it from a stage; it is immutable once
built and holds no OpenUSD type, so the code that renders it never includes
an OpenUSD header.

Public headers (`include/umbra/scene/`):

- `scene.h`: the Scene of one frame.
