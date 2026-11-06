//! What every 2- and 3-tuple shares (pbrt-v4 `Tuple2`, `Tuple3`): component
//! access, scaling, negation, component-wise `Abs`, `Floor`, `Ceil`, `Min`,
//! `Max`, and the component queries.
//!
//! PBR 4e §3.2. pbrt-v4 derives `Vector3`, `Point3` and `Normal3` from
//! `Tuple3` by CRTP; here a macro gives each type its own copy, so a point
//! never silently becomes a vector.

/// The operations of pbrt-v4 `Tuple2`/`Tuple3` for `$name<T>`, whose fields
/// are `$f` (2 or 3 of them).
macro_rules! impl_tuple {
    ($name:ident, $($f:ident),+) => {
        impl<T> $name<T> {
            #[doc = concat!("A `", stringify!($name), "` from its components.")]
            #[inline]
            #[must_use]
            pub const fn new($($f: T),+) -> Self {
                Self { $($f),+ }
            }
        }

        impl $name<f32> {
            /// Whether any component is NaN.
            ///
            /// pbrt-v4 `Tuple::HasNaN`.
            #[must_use]
            pub fn has_nan(self) -> bool {
                $(self.$f.is_nan())||+
            }

            /// The component-wise absolute value.
            ///
            /// pbrt-v4 `Abs(Tuple)`.
            #[must_use]
            pub fn abs(self) -> Self {
                Self { $($f: self.$f.abs()),+ }
            }

            /// The component-wise `⌈·⌉`.
            ///
            /// pbrt-v4 `Ceil(Tuple)`.
            #[must_use]
            pub fn ceil(self) -> Self {
                Self { $($f: libm::ceilf(self.$f)),+ }
            }

            /// The component-wise `⌊·⌋`.
            ///
            /// pbrt-v4 `Floor(Tuple)`.
            #[must_use]
            pub fn floor(self) -> Self {
                Self { $($f: libm::floorf(self.$f)),+ }
            }
        }

        impl<T: PartialOrd + Copy> $name<T> {
            /// The component-wise minimum, with `std::min` semantics.
            ///
            /// pbrt-v4 `Min(Tuple, Tuple)`.
            #[must_use]
            pub fn min(self, other: Self) -> Self {
                Self { $($f: crate::min(self.$f, other.$f)),+ }
            }

            /// The component-wise maximum, with `std::max` semantics.
            ///
            /// pbrt-v4 `Max(Tuple, Tuple)`.
            #[must_use]
            pub fn max(self, other: Self) -> Self {
                Self { $($f: crate::max(self.$f, other.$f)),+ }
            }
        }

        impl<T: Copy> $name<T> {
            /// The tuple with its components picked by index from `self`.
            ///
            /// pbrt-v4 `Permute`.
            #[must_use]
            pub fn permute(self, p: [usize; [$(stringify!($f)),+].len()]) -> Self {
                let [$($f),+] = p.map(|i| self[i]);
                Self { $($f),+ }
            }
        }

        impl<T: core::ops::Mul<Output = T> + Copy> $name<T> {
            /// The product of the components.
            ///
            /// pbrt-v4 `HProd`.
            #[must_use]
            pub fn h_prod(self) -> T {
                crate::vecmath::tuple::product!(self, $($f),+)
            }
        }

        impl<T: core::ops::Neg<Output = T>> core::ops::Neg for $name<T> {
            type Output = Self;
            #[inline]
            fn neg(self) -> Self {
                Self { $($f: -self.$f),+ }
            }
        }

        impl<T: core::ops::Mul<Output = T> + Copy> core::ops::Mul<T> for $name<T> {
            type Output = Self;
            /// Scales by `s`, computed as `s · component` as pbrt-v4 does.
            #[inline]
            fn mul(self, s: T) -> Self {
                Self { $($f: s * self.$f),+ }
            }
        }

        impl<T: core::ops::Div<Output = T> + Copy> core::ops::Div<T> for $name<T> {
            type Output = Self;
            /// Divides each component by `d`, not multiplies by `1 / d`.
            #[inline]
            fn div(self, d: T) -> Self {
                Self { $($f: self.$f / d),+ }
            }
        }

        impl<T: core::ops::Mul<Output = T> + Copy> core::ops::MulAssign<T> for $name<T> {
            #[inline]
            fn mul_assign(&mut self, s: T) {
                *self = *self * s;
            }
        }

        impl<T: core::ops::Div<Output = T> + Copy> core::ops::DivAssign<T> for $name<T> {
            #[inline]
            fn div_assign(&mut self, d: T) {
                *self = *self / d;
            }
        }

        impl core::ops::Mul<f32> for $name<crate::Interval> {
            type Output = Self;
            #[inline]
            fn mul(self, s: f32) -> Self {
                Self { $($f: s * self.$f),+ }
            }
        }

        impl core::ops::Div<f32> for $name<crate::Interval> {
            type Output = Self;
            #[inline]
            fn div(self, d: f32) -> Self {
                Self { $($f: self.$f / d),+ }
            }
        }

        crate::vecmath::tuple::impl_left_scalar!($name, f32, $($f),+);
        crate::vecmath::tuple::impl_left_scalar!($name, i32, $($f),+);
        crate::vecmath::tuple::impl_left_scalar!($name, crate::Interval, $($f),+);

        impl From<$name<i32>> for $name<f32> {
            #[expect(clippy::cast_precision_loss, reason = "pbrt-v4's `Float(int)` conversion")]
            fn from(t: $name<i32>) -> Self {
                Self { $($f: t.$f as f32),+ }
            }
        }

        impl From<$name<f32>> for $name<crate::Interval> {
            /// Exact intervals around each component.
            fn from(t: $name<f32>) -> Self {
                Self { $($f: crate::Interval::from(t.$f)),+ }
            }
        }

        impl From<$name<crate::Interval>> for $name<f32> {
            /// The midpoints, as pbrt-v4's `Float(Interval)`.
            fn from(t: $name<crate::Interval>) -> Self {
                Self { $($f: t.$f.midpoint()),+ }
            }
        }

        impl $name<crate::Interval> {
            /// Each component as the interval around its value with the given
            /// error.
            ///
            /// pbrt-v4 `Point3fi(Point3f p, Vector3f e)`,
            /// `Vector3fi(Vector3f v, Vector3f e)`.
            #[must_use]
            pub fn from_value_and_error(v: $name<f32>, e: $name<f32>) -> Self {
                Self { $($f: crate::Interval::from_value_and_error(v.$f, e.$f)),+ }
            }

            /// The half-widths of the component intervals.
            ///
            /// pbrt-v4 `Point3fi::Error`.
            #[must_use]
            pub fn error(self) -> $name<f32> {
                $name { $($f: self.$f.width() / 2.0),+ }
            }

            /// Whether every component is exact, of zero width.
            ///
            /// pbrt-v4 `Point3fi::IsExact`.
            #[must_use]
            pub fn is_exact(self) -> bool {
                $(self.$f.width() == 0.0)&&+
            }
        }
    };
}

/// `s * t` for the scalar type `$s` on the left (pbrt-v4
/// `operator*(U, Tuple)`).
macro_rules! impl_left_scalar {
    ($name:ident, $s:ty, $($f:ident),+) => {
        impl<T> core::ops::Mul<$name<T>> for $s
        where
            $s: core::ops::Mul<T, Output = T>,
        {
            type Output = $name<T>;
            #[inline]
            fn mul(self, t: $name<T>) -> $name<T> {
                $name { $($f: self * t.$f),+ }
            }
        }
    };
}

/// `x * y` or `x * y * z`, left to right.
macro_rules! product {
    ($t:ident, $first:ident $(, $f:ident)*) => {
        $t.$first $(* $t.$f)*
    };
}

/// Indexing of a 2-tuple, `[x, y]`.
macro_rules! impl_tuple2 {
    ($name:ident) => {
        crate::vecmath::tuple::impl_tuple!($name, x, y);

        impl<T> core::ops::Index<usize> for $name<T> {
            type Output = T;
            fn index(&self, i: usize) -> &T {
                match i {
                    0 => &self.x,
                    1 => &self.y,
                    _ => panic!("index {i} out of range for a 2-tuple"),
                }
            }
        }

        impl<T> core::ops::IndexMut<usize> for $name<T> {
            fn index_mut(&mut self, i: usize) -> &mut T {
                match i {
                    0 => &mut self.x,
                    1 => &mut self.y,
                    _ => panic!("index {i} out of range for a 2-tuple"),
                }
            }
        }

        impl<T: PartialOrd + Copy> $name<T> {
            /// pbrt-v4 `MinComponentValue`.
            #[must_use]
            pub fn min_component_value(self) -> T {
                crate::min(self.x, self.y)
            }

            /// pbrt-v4 `MaxComponentValue`.
            #[must_use]
            pub fn max_component_value(self) -> T {
                crate::max(self.x, self.y)
            }

            /// pbrt-v4 `MinComponentIndex`.
            #[must_use]
            pub fn min_component_index(self) -> usize {
                if self.x < self.y { 0 } else { 1 }
            }

            /// pbrt-v4 `MaxComponentIndex`.
            #[must_use]
            pub fn max_component_index(self) -> usize {
                if self.x > self.y { 0 } else { 1 }
            }
        }
    };
}

/// Indexing of a 3-tuple, `[x, y, z]`.
macro_rules! impl_tuple3 {
    ($name:ident) => {
        crate::vecmath::tuple::impl_tuple!($name, x, y, z);

        impl<T> core::ops::Index<usize> for $name<T> {
            type Output = T;
            fn index(&self, i: usize) -> &T {
                match i {
                    0 => &self.x,
                    1 => &self.y,
                    2 => &self.z,
                    _ => panic!("index {i} out of range for a 3-tuple"),
                }
            }
        }

        impl<T> core::ops::IndexMut<usize> for $name<T> {
            fn index_mut(&mut self, i: usize) -> &mut T {
                match i {
                    0 => &mut self.x,
                    1 => &mut self.y,
                    2 => &mut self.z,
                    _ => panic!("index {i} out of range for a 3-tuple"),
                }
            }
        }

        impl<T: PartialOrd + Copy> $name<T> {
            /// pbrt-v4 `MinComponentValue`.
            #[must_use]
            pub fn min_component_value(self) -> T {
                crate::min(self.x, crate::min(self.y, self.z))
            }

            /// pbrt-v4 `MaxComponentValue`.
            #[must_use]
            pub fn max_component_value(self) -> T {
                crate::max(self.x, crate::max(self.y, self.z))
            }

            /// pbrt-v4 `MinComponentIndex`.
            #[must_use]
            pub fn min_component_index(self) -> usize {
                if self.x < self.y {
                    if self.x < self.z { 0 } else { 2 }
                } else if self.y < self.z {
                    1
                } else {
                    2
                }
            }

            /// pbrt-v4 `MaxComponentIndex`.
            #[must_use]
            pub fn max_component_index(self) -> usize {
                if self.x > self.y {
                    if self.x > self.z { 0 } else { 2 }
                } else if self.y > self.z {
                    1
                } else {
                    2
                }
            }
        }
    };
}

/// Component-wise `+` and `−` between `$a<T>` and `$b<U>`, giving `$out`
/// (pbrt-v4's `operator+`/`operator-` of the tuple types, which mix element
/// types such as `Point3fi + Vector3f`).
macro_rules! impl_add_sub {
    ($a:ident + $b:ident = $out:ident, $($f:ident),+) => {
        impl<T: core::ops::Add<U>, U> core::ops::Add<$b<U>> for $a<T> {
            type Output = $out<T::Output>;
            #[inline]
            fn add(self, o: $b<U>) -> Self::Output {
                $out { $($f: self.$f + o.$f),+ }
            }
        }
    };
    ($a:ident - $b:ident = $out:ident, $($f:ident),+) => {
        impl<T: core::ops::Sub<U>, U> core::ops::Sub<$b<U>> for $a<T> {
            type Output = $out<T::Output>;
            #[inline]
            fn sub(self, o: $b<U>) -> Self::Output {
                $out { $($f: self.$f - o.$f),+ }
            }
        }
    };
    ($a:ident += $b:ident, $($f:ident),+) => {
        impl<T: core::ops::AddAssign<U>, U> core::ops::AddAssign<$b<U>> for $a<T> {
            #[inline]
            fn add_assign(&mut self, o: $b<U>) {
                $(self.$f += o.$f;)+
            }
        }
    };
    ($a:ident -= $b:ident, $($f:ident),+) => {
        impl<T: core::ops::SubAssign<U>, U> core::ops::SubAssign<$b<U>> for $a<T> {
            #[inline]
            fn sub_assign(&mut self, o: $b<U>) {
                $(self.$f -= o.$f;)+
            }
        }
    };
}

/// `From<$from<T>> for $to<T>`: the same components, reinterpreted (pbrt-v4's
/// explicit conversion constructors between tuple types).
macro_rules! impl_convert {
    ($from:ident => $to:ident, $($f:ident),+) => {
        impl<T> From<$from<T>> for $to<T> {
            #[inline]
            fn from(t: $from<T>) -> Self {
                Self { $($f: t.$f),+ }
            }
        }
    };
}

pub(crate) use {
    impl_add_sub, impl_convert, impl_left_scalar, impl_tuple, impl_tuple2, impl_tuple3, product,
};
