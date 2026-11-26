# umbra renders spectrally from ACEScg inputs

umbra transports light as spectra, sampled per wavelength, rather than as
three RGB channels, so that light sources and surfaces interact through
their spectra. Scenes, however, are authored in RGB, in many color spaces.

ACEScg is the reference color space of the Scene. Every color value is
converted to ACEScg when the scene loads; a texture keeps its file and its
color space, and the renderer converts its texels to ACEScg as it reads
them. OpenColorIO converts both, with its built-in `ocio://cg-config-latest`
configuration, or the configuration `$OCIO` names. ACEScg is wide enough for
the colors scenes are authored in.

The Scene keeps each color as ACEScg RGB with its spectral kind:
reflectance (bounded by 1, as an albedo), unbounded (a scattering
coefficient), or illuminant (an emission). The renderer uplifts it to a
spectrum with the sigmoid polynomials of Jakob and Hanika (2019), which give
a smooth spectrum per RGB color from one precomputed table for the ACEScg
gamut; the kind chooses how the uplifted spectrum is scaled. A `UsdLux`
`colorTemperature` is a blackbody spectrum, not an RGB color. The kind
applies to the colors outside material networks, such as a light's: a
network's colors have none, since a color can reach a closure through any
number of nodes, and the closure gives it its kind when the integrator
evaluates it (an emission is illuminant, a BSDF's albedo reflectance, a
medium's coefficients unbounded).

OSL shaders and MaterialX networks shade in RGB, as they are written; the
closures they return are uplifted per wavelength when the integrator
evaluates them.

The film accumulates the spectral samples into CIE XYZ, with the CIE 1931 2°
observer, and writes ACEScg OpenEXR images, tagged with their
`chromaticities`.

## Consequences

- The integrator works in one color space: every color it reads, from the
  Scene or from a texture, is ACEScg.
- The uplift table is umbra's own data, built for the ACEScg gamut; a
  different reference space would need a new table.
