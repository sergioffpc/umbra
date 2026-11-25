# umbra renders spectrally from ACEScg inputs

umbra transports light as spectra, sampled per wavelength, so that
dispersion and the spectra of light sources are rendered as they are,
rather than through three RGB channels. Scenes, however, are authored in
RGB, in many color spaces.

Every color input is converted to ACEScg when the scene loads: the
reference color space of the Scene. OpenColorIO converts it, with its
built-in `ocio://cg-config-latest` configuration, or the configuration
`$OCIO` names. ACEScg is wide enough for the colors scenes are authored in,
and is the working space of the pipelines umbra renders for.

The Scene keeps each color as ACEScg RGB with its spectral kind:
reflectance (bounded by 1, as an albedo), unbounded (a scattering
coefficient), or illuminant (an emission). The renderer uplifts it to a
spectrum with the sigmoid polynomials of Jakob and Hanika (2019), which give
a smooth spectrum per RGB color from one precomputed table for the ACEScg
gamut; the kind chooses how the uplifted spectrum is scaled. A `UsdLux`
`colorTemperature` is a blackbody spectrum, not an RGB color.

OSL shaders and MaterialX networks shade in RGB, as they are written; the
closures they return are uplifted per wavelength when the integrator
evaluates them.

The film accumulates the spectral samples into CIE XYZ, with the CIE 1931 2°
observer, and writes ACEScg OpenEXR images, tagged with their
`chromaticities`.

## Consequences

- Color management happens once, at load: the integrator never converts
  between color spaces, and the Scene holds no color space but ACEScg.
- The uplift table is umbra's own data, built for the ACEScg gamut; a
  different reference space would need a new table.
- An image is in ACEScg wherever it is read: OpenColorIO converts it for
  display.
