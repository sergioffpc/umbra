# Lights are physical: no artistic light controls

UsdLux gives its lights controls with no physical counterpart, besides
light and shadow linking (docs/adr/0009-lights-light-everything.md):

- `diffuse` and `specular`, which scale what a light gives the diffuse
  and the specular lobes of a surface apart;
- `shaping:focus` and `shaping:focusTint`, which concentrate the emission
  of an area light along its normal and tint what falls outside it;
- `shaping:cone:softness`, which fades the emission towards the edge of
  the cone;
- `ShadowAPI`'s `shadow:enable`, `shadow:color`, `shadow:distance`,
  `shadow:falloff` and `shadow:falloffGamma`, which turn off, tint or fade
  a light's shadows.

No physical emitter lights the diffuse part of a surface more than its
specular part, and no shadow is colored or ends at a distance. umbra aims
to render as physically as it can, so it does not honor them: the Scene's
lights hold none of them. What UsdLux gives them that is physical stays:
the shape and size of the emitter, its intensity, exposure and power
normalization, its color, its color temperature, as a blackbody
(docs/adr/0007-spectral-rendering-from-acescg.md), and the angle of its
cone, a barn door that blocks emission outside it.

A stage that authors them is still rendered, and the difference is not
silent: at the end of `Syncing scene`, one warning for each control
reports the lights that author it, with their count and the first.

## Consequences

- The integrator evaluates every light with the same BSDF, its shadows
  traced as any other ray: a light needs no per-lobe weight nor shadow
  state.
- A stage lit for another renderer with these controls renders differently
  in umbra: a light lights the lobe it was kept from, a focused light
  spreads as UsdLux's unfocused emission, a soft cone ends sharply, and
  shadows fall where the artist removed or tinted them.
