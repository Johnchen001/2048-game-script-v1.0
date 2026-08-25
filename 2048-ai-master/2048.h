#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "platdefs.h"

/* 2048 board representation.
 *
 * v2 (after the 65536 push): each tile now uses a 5-bit field, so the 4x4
 * board needs 16 * 5 = 80 bits. It is stored in two 64-bit words:
 *
 *   board_t.lo holds tiles 0..7   (tile i at bit offset 5*i,  i = 0..7)
 *   board_t.hi holds tiles 8..15  (tile i at bit offset 5*(i-8))
 *
 * Tile index i = 4*row + col (row-major, (0,0) first).
 * Each field stores the *rank* (exponent): rank 0 = empty, rank r = tile 2^r.
 * With 5 bits per field, ranks 0..31 are representable, i.e. tiles up to
 * 2^31 — far beyond 65536 (rank 16) / 131072 (rank 17). The merge of two
 * rank-31 tiles is treated as a no-op (representational limit).
 *
 * A row (4 tiles) packs into 20 bits: row_t.
 * Rows 0..1 live in lo (bits 20*row), rows 2..3 live in hi.
 */

typedef struct {
    uint64_t lo;
    uint64_t hi;
} board_t;

typedef uint32_t row_t; /* 4 tiles * 5 bits */

#define ROW_BITS 20
#define ROW_COUNT (1u << ROW_BITS)
#define ROW_MASK 0xFFFFFULL
#define TILE_MASK 0x1f
#define MAX_RANK 31

/* transposition table entry: depth at which the heuristic was recorded + value */
struct trans_table_entry_t {
    uint8_t depth;
    float heuristic;
};

/* ---- basic bit helpers ------------------------------------------------ */

static inline board_t board_make(uint64_t lo, uint64_t hi) {
    board_t b;
    b.lo = lo;
    b.hi = hi;
    return b;
}

static inline int board_eq(board_t a, board_t b) {
    return a.lo == b.lo && a.hi == b.hi;
}

/* get/set a single tile (index 0..15) */
static inline int get_tile(board_t b, int i) {
    uint64_t w = (i < 8) ? b.lo : b.hi;
    return (int)((w >> (5 * (i & 7))) & TILE_MASK);
}

static inline board_t set_tile(board_t b, int i, int v) {
    int s = 5 * (i & 7);
    uint64_t mask = (uint64_t)TILE_MASK << s;
    if (i < 8) {
        b.lo = (b.lo & ~mask) | ((uint64_t)v << s);
    } else {
        b.hi = (b.hi & ~mask) | ((uint64_t)v << s);
    }
    return b;
}

/* get/set a full row (row index 0..3), 20 bits */
static inline row_t get_row(board_t b, int r) {
    uint64_t w = (r < 2) ? b.lo : b.hi;
    return (row_t)((w >> (20 * (r & 1))) & ROW_MASK);
}

static inline board_t set_row(board_t b, int r, row_t v) {
    int s = 20 * (r & 1);
    uint64_t mask = ROW_MASK << s;
    uint64_t w = (r < 2) ? b.lo : b.hi;
    w = (w & ~mask) | ((uint64_t)v << s);
    if (r < 2) b.lo = w;
    else       b.hi = w;
    return b;
}

/* get a column (0..3) as a 20-bit "row" in top->bottom order */
static inline row_t get_col(board_t b, int c) {
    return (row_t)(get_tile(b, c) |
                   (get_tile(b, c + 4) << 5) |
                   (get_tile(b, c + 8) << 10) |
                   (get_tile(b, c + 12) << 15));
}

/* xor a 20-bit column diff into the four tiles of column c */
static inline board_t xcol(board_t b, int c, row_t diff) {
    int k;
    for (k = 0; k < 4; k++) {
        int i = c + 4 * k;
        b = set_tile(b, i, get_tile(b, i) ^ ((diff >> (5 * k)) & TILE_MASK));
    }
    return b;
}

static inline void print_board(board_t board) {
    int i, j;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            int rank = get_tile(board, 4 * i + j);
            printf("%12llu", (rank == 0) ? 0ULL : (1ULL << rank));
        }
        printf("\n");
    }
    printf("\n");
}

/* transpose: tile (r,c) <-> tile (c,r) */
static inline board_t transpose(board_t x) {
    board_t r = board_make(0, 0);
    int i;
    for (i = 0; i < 16; i++) {
        int tile = get_tile(x, i);
        int rr = i >> 2, cc = i & 3;
        r = set_tile(r, 4 * cc + rr, tile);
    }
    return r;
}

/* reverse the 4 tiles of a 20-bit row */
static inline row_t reverse_row(row_t row) {
    row_t r = 0;
    int k;
    for (k = 0; k < 4; k++) {
        r |= ((row >> (5 * k)) & TILE_MASK) << (5 * (3 - k));
    }
    return r;
}

/* Functions */
#ifdef __cplusplus
extern "C" {
#endif

/* All exported functions take/return the 80-bit board as a pair of uint64_t
 * (lo, hi) through a 16-byte buffer, so they work from any FFI (ctypes...).
 */

DLL_PUBLIC void init_tables();

/* out = execute_move(move, in) */
DLL_PUBLIC void execute_move(int move, const uint64_t *in, uint64_t *out);
DLL_PUBLIC float score_toplevel_move(const uint64_t *board, int move);
DLL_PUBLIC int find_best_move(const uint64_t *board);
DLL_PUBLIC int ask_for_move(const uint64_t *board);

typedef int (*get_move_func_t)(const uint64_t *);
DLL_PUBLIC void play_game(get_move_func_t get_move);

#ifdef __cplusplus
}
#endif
