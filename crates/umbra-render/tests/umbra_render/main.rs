//! Tests of `umbra-render` through its public interface, at its two seams
//! (spec #72, "Testing Decisions"): the Film's acceptance of Work Results,
//! and the Renderer turning Work Units of a Scene built with `SceneBuilder`
//! into Work Results.

mod film;
mod renderer;
mod scenes;
