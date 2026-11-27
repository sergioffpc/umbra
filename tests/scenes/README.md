# scenes

The reference scenes of umbra's tests: USD scenes that tests render, and
that anyone can render with `umbra render`. Each renders through the
RenderSettings prim its stage names and writes its products into the
directory it is rendered from.

- `cornell_box.usda`: the Cornell box, from the measurements of the Cornell
  University Program of Computer Graphics, in millimetres. A rect light on
  the ceiling lights a white box with a red wall on the left and a green
  wall on the right, and two white blocks. It renders a 512×512 product,
  `cornell_box.exr`, at 256 samples per pixel, with the color, albedo,
  normal, depth and denoised color.

To render it on every device, from a directory of your own:

```sh
umbra render <repository>/tests/scenes/cornell_box.usda
```

`--devices=cpu0` or `--devices=gpu0` picks the devices, and
`--display=tev` shows the frame in tev as it renders.
