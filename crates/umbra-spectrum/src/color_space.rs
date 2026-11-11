//! RGB colour spaces (pbrt-v4 `src/pbrt/util/colorspace.h`). sRGB is the
//! only one so far; the others join when a milestone needs them (arc42
//! §8.1).

use umbra_math::{Point2f, SquareMatrix};

use crate::color::{RGB, RGBSigmoidPolynomial, XYZ};
use crate::narrow;
use crate::rgb_to_spectrum::{self, RGBToSpectrumTable};
use crate::spectra::StdIlluminant;
use crate::spectrum::{DenselySampledSpectrum, spectrum_to_xyz};

/// An RGB colour space: its primaries' and white point's chromaticities,
/// its illuminant, its matrices to and from XYZ, and its RGB-to-spectrum
/// table.
///
/// PBR 4e §4.6.3, pbrt-v4 `RGBColorSpace`.
#[derive(Debug, Clone)]
pub struct RGBColorSpace {
    r: Point2f,
    g: Point2f,
    b: Point2f,
    w: Point2f,
    illuminant: StdIlluminant,
    xyz_from_rgb: SquareMatrix<3>,
    rgb_from_xyz: SquareMatrix<3>,
    rgb_to_spectrum_table: &'static RGBToSpectrumTable,
}

impl RGBColorSpace {
    /// The colour space with primaries `r`, `g`, `b` and white `illuminant`,
    /// or `None` if the primaries are not independent (where pbrt-v4 exits).
    ///
    /// pbrt-v4 `RGBColorSpace::RGBColorSpace`.
    fn new(
        [r, g, b]: [Point2f; 3],
        illuminant: StdIlluminant,
        rgb_to_spectrum_table: &'static RGBToSpectrumTable,
    ) -> Option<Self> {
        // The white point, and the primaries at unit luminance.
        let white = spectrum_to_xyz(illuminant.spectrum());
        let [pr, pg, pb] = [r, g, b].map(|c| XYZ::from_xy_y(c, 1.0));

        // The XYZ of each primary scaled so that they sum to the white.
        let rgb = SquareMatrix::new([[pr.x, pg.x, pb.x], [pr.y, pg.y, pb.y], [pr.z, pg.z, pb.z]]);
        let c = rgb.inverse()? * <[f32; 3]>::from(white);
        let xyz_from_rgb = rgb * SquareMatrix::diag(c);
        Some(Self {
            r,
            g,
            b,
            w: white.xy(),
            illuminant,
            xyz_from_rgb,
            rgb_from_xyz: xyz_from_rgb.inverse()?,
            rgb_to_spectrum_table,
        })
    }

    /// sRGB: the Rec. ITU-R BT.709 primaries, with D65 white.
    ///
    /// pbrt-v4 `RGBColorSpace::sRGB`, which pbrt-v4 builds once at start-up;
    /// this computes it on each call, so callers keep the result.
    ///
    /// # Panics
    ///
    /// Never: the sRGB primaries are independent.
    #[must_use]
    pub fn srgb() -> Self {
        // pbrt-v4's `double` literals, rounded to `float` as C++ does, at
        // compile time: no `f64` at run time (ADR 0028).
        const PRIMARIES: [(f32, f32); 3] = [
            (narrow(0.64), narrow(0.33)),
            (narrow(0.3), narrow(0.6)),
            (narrow(0.15), narrow(0.06)),
        ];
        let primaries = PRIMARIES.map(|(x, y)| Point2f::new(x, y));
        Self::new(primaries, StdIlluminant::D65, &rgb_to_spectrum::SRGB)
            .expect("the sRGB primaries are independent")
    }

    /// The chromaticity of the red primary.
    #[must_use]
    pub fn r(&self) -> Point2f {
        self.r
    }

    /// The chromaticity of the green primary.
    #[must_use]
    pub fn g(&self) -> Point2f {
        self.g
    }

    /// The chromaticity of the blue primary.
    #[must_use]
    pub fn b(&self) -> Point2f {
        self.b
    }

    /// The chromaticity of the white point, the illuminant's.
    #[must_use]
    pub fn w(&self) -> Point2f {
        self.w
    }

    /// The illuminant, whose spectrum is this space's white.
    #[must_use]
    pub fn std_illuminant(&self) -> StdIlluminant {
        self.illuminant
    }

    /// The spectrum of the illuminant.
    ///
    /// pbrt-v4 `RGBColorSpace::illuminant`.
    #[must_use]
    pub fn illuminant(&self) -> &'static DenselySampledSpectrum<'static> {
        self.illuminant.spectrum()
    }

    /// The matrix from linear RGB to XYZ.
    #[must_use]
    pub fn xyz_from_rgb(&self) -> SquareMatrix<3> {
        self.xyz_from_rgb
    }

    /// The matrix from XYZ to linear RGB.
    #[must_use]
    pub fn rgb_from_xyz(&self) -> SquareMatrix<3> {
        self.rgb_from_xyz
    }

    /// The RGB whose dot product with a colour is its luminance Y: the
    /// second row of [`Self::xyz_from_rgb`].
    ///
    /// pbrt-v4 `RGBColorSpace::LuminanceVector`.
    #[must_use]
    pub fn luminance_vector(&self) -> RGB {
        RGB::from(self.xyz_from_rgb[1])
    }

    /// `xyz` in this space's linear RGB.
    ///
    /// pbrt-v4 `RGBColorSpace::ToRGB`.
    #[must_use]
    pub fn to_rgb(&self, xyz: XYZ) -> RGB {
        RGB::from(self.rgb_from_xyz * <[f32; 3]>::from(xyz))
    }

    /// `rgb` of this space in XYZ.
    ///
    /// pbrt-v4 `RGBColorSpace::ToXYZ`.
    #[must_use]
    pub fn to_xyz(&self, rgb: RGB) -> XYZ {
        XYZ::from(self.xyz_from_rgb * <[f32; 3]>::from(rgb))
    }

    /// The sigmoid polynomial of `rgb`, clamped below at 0; each component
    /// must be at most 1.
    ///
    /// pbrt-v4 `RGBColorSpace::ToRGBCoeffs`.
    #[must_use]
    pub fn to_rgb_coeffs(&self, rgb: RGB) -> RGBSigmoidPolynomial {
        debug_assert!(
            rgb.r >= 0.0 && rgb.g >= 0.0 && rgb.b >= 0.0,
            "to_rgb_coeffs: negative {rgb:?}"
        );
        self.rgb_to_spectrum_table.lookup(rgb.clamp_zero())
    }
}
