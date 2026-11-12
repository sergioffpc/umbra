//! `Arena`: handles minted in order, each giving back its own object.

use umbra_geometry::Handle;
use umbra_scene::Arena;

#[test]
fn each_handle_gets_its_object() {
    let mut arena = Arena::new();
    let a = arena.push("a").unwrap();
    let b = arena.push("b").unwrap();
    assert_ne!(a, b, "two objects, two handles");
    assert_eq!(arena.get(a), Some(&"a"));
    assert_eq!(arena.get(b), Some(&"b"));
    assert_eq!(arena.as_slice(), ["a", "b"], "slices in handle order");
    assert_eq!(arena.len(), 2);
}

#[test]
fn a_handle_past_the_end_gets_nothing() {
    let mut arena = Arena::new();
    arena.push(1.0_f32).unwrap();
    assert_eq!(arena.get(Handle::new(1)), None);
    assert!(Arena::<f32>::new().is_empty());
}
