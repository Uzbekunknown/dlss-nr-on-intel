/* The one number both the cadence row and its test size their store with.
 *
 * The store is `static char every_names_store[4][N]` in `nr_layer.c`, and N was 24 for as long as
 * the only language that needed more than 23 bytes was the one nobody had opened the panel in.
 * Russian needs 27 for "каждые 2 кадров", so every Russian option was cut mid-word.
 *
 * Why this is a header rather than a literal in two places: the test that catches it originally
 * wrote `<= 48` while the source used 24, so shrinking the store did not fail the test - the check
 * was measuring the strings against a number nothing else used. One constant, both users, and the
 * mutation is caught.
 *
 * If this is ever raised, `test_menu_pacing.cpp`'s width check follows automatically. */
#ifndef NR_LAYER_CADENCE_H
#define NR_LAYER_CADENCE_H

/* Bytes per cadence option, terminator included. 48 covers the longest string any of the four
 * languages builds (27 for Russian) with room for the next one. */
#define CADENCE_NAME_STORE 48

#endif /* NR_LAYER_CADENCE_H */
