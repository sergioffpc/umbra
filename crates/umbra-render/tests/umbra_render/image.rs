//! The final image is written in the format its path's extension names.

use std::fs;
use std::io;

use image::{ColorType, ImageReader};
use umbra_math::Point2i;
use umbra_render::Image;

/// A 2 × 1 image: one red pixel, one blue.
fn two_pixels() -> Image {
    Image::new(Point2i::new(2, 1), vec![[1.0, 0.0, 0.0], [0.0, 0.0, 0.5]])
}

#[test]
fn an_exr_extension_writes_an_exr_with_the_pixels() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("out.exr");
    two_pixels().write(&path).unwrap();

    let reader = ImageReader::open(&path)
        .unwrap()
        .with_guessed_format()
        .unwrap();
    assert_eq!(reader.format(), Some(image::ImageFormat::OpenExr));
    let decoded = reader.decode().unwrap();
    assert_eq!(decoded.color(), ColorType::Rgb32F);
    assert_eq!(
        decoded.into_rgb32f().into_raw(),
        [1.0, 0.0, 0.0, 0.0, 0.0, 0.5]
    );
}

#[test]
fn an_unknown_extension_is_invalid_input_and_writes_nothing() {
    let dir = tempfile::tempdir().unwrap();
    let error = two_pixels()
        .write(&dir.path().join("out.nope"))
        .unwrap_err();

    assert_eq!(error.kind(), io::ErrorKind::InvalidInput);
    assert_eq!(fs::read_dir(dir.path()).unwrap().count(), 0, "no file");
}
