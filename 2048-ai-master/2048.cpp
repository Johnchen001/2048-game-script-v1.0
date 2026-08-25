#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <algorithm>

#include "2048.h"

#include "config.h"
#if defined(HAVE_UNORDERED_MAP)
    #include <unordered_map>
#elif defined(HAVE_TR1_UNORDERED_MAP)
    #include <tr1/unordered_map>
#else
    #include <map>
#endif

/* Custom hash for the 80-bit board_t (no std hash for structs). */
struct board_hash {
    size_t operator()(const board_t &b) const {
        uint64_t h = b.lo * 0x9E3779B97F4A7C15ULL ^ (b.hi + 0x9E3779B97F4A7C15ULL);
        h ^= h >> 29;
        h *= 0xBF58476D1CE4E5B9ULL;
        h ^= h >> 32;
        return (size_t)h;
    }
};

/* Ordering needed by the std::map fallback when <unordered_map> is absent. */
static inline bool operator<(const board_t &a, const board_t &b) {
    return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo);
}

#if defined(HAVE_UNORDERED_MAP)
    typedef std::unordered_map<board_t, trans_table_entry_t, board_hash> trans_table_t;
#elif defined(HAVE_TR1_UNORDERED_MAP)
    typedef std::tr1::unordered_map<board_t, trans_table_entry_t, board_hash> trans_table_t;
#else
    typedef std::map<board_t, trans_table_entry_t> trans_table_t;
#endif

/* MSVC compatibility: undefine max and min macros */
#if defined(max)
#undef max
#endif

#if defined(min)
#undef min
#endif

/* Move tables. A row/column packs 4 tiles * 5 bits = 20 bits.
 * Each 20-bit value is mapped to (old ^ new) assuming the move direction;
 * XORing the diff into the board applies the move.
 *
 * Columns use the exact same tables as rows: a column merged "up" is the same
 * operation as a row merged "left"; "down" == "right".
 */
static row_t row_left_table [ROW_COUNT];
static row_t row_right_table[ROW_COUNT];
static float heur_score_table[ROW_COUNT];
static float score_table[ROW_COUNT];

// Heuristic scoring settings
static const float SCORE_LOST_PENALTY = 200000.0f;
static const float SCORE_MONOTONICITY_POWER = 4.0f;
static const float SCORE_MONOTONICITY_WEIGHT = 47.0f;
static const float SCORE_SUM_POWER = 3.5f;
static const float SCORE_SUM_WEIGHT = 11.0f;
static const float SCORE_MERGES_WEIGHT = 700.0f;
static const float SCORE_EMPTY_WEIGHT = 270.0f;

/* ------- speed knobs (env vars, read once) ------------------------------
 * 2048_DEPTH : integer added to the adaptive depth limit.
 *              positive = deeper/slower/stronger, negative = faster/weaker.
 * 2048_CPROB : cumulative-probability pruning threshold (default 1e-4).
 *              higher values = much faster but weaker.
 */
static int depth_adjust = -2; /* -2 == uninitialized */
static float cprob_thresh = -1.0f;

static int get_depth_adjust() {
    if (depth_adjust == -2) {
        const char *e = getenv("2048_DEPTH");
        depth_adjust = e ? atoi(e) : 0;
    }
    return depth_adjust;
}

static float get_cprob_thresh() {
    if (cprob_thresh < 0.0f) {
        const char *e = getenv("2048_CPROB");
        cprob_thresh = e ? (float)atof(e) : 1e-4f;
    }
    return cprob_thresh;
}

static inline board_t execute_move_internal(int move, board_t board);

void init_tables() {
    unsigned row;
    for (row = 0; row < ROW_COUNT; ++row) {
        unsigned line[4] = {
                (row >>  0) & 0x1f,
                (row >>  5) & 0x1f,
                (row >> 10) & 0x1f,
                (row >> 15) & 0x1f
        };

        // Score: total sum of the tile and all intermediate merged tiles
        float score = 0.0f;
        for (int i = 0; i < 4; ++i) {
            int rank = line[i];
            if (rank >= 2) {
                score += (float)((uint64_t)(rank - 1) * (1ULL << rank));
            }
        }
        score_table[row] = score;

        // Heuristic score
        float sum = 0;
        int empty = 0;
        int merges = 0;

        int prev = 0;
        int counter = 0;
        for (int i = 0; i < 4; ++i) {
            int rank = line[i];
            sum += pow(rank, SCORE_SUM_POWER);
            if (rank == 0) {
                empty++;
            } else {
                if (prev == rank) {
                    counter++;
                } else if (counter > 0) {
                    merges += 1 + counter;
                    counter = 0;
                }
                prev = rank;
            }
        }
        if (counter > 0) {
            merges += 1 + counter;
        }

        float monotonicity_left = 0;
        float monotonicity_right = 0;
        for (int i = 1; i < 4; ++i) {
            if (line[i-1] > line[i]) {
                monotonicity_left += pow(line[i-1], SCORE_MONOTONICITY_POWER) - pow(line[i], SCORE_MONOTONICITY_POWER);
            } else {
                monotonicity_right += pow(line[i], SCORE_MONOTONICITY_POWER) - pow(line[i-1], SCORE_MONOTONICITY_POWER);
            }
        }

        heur_score_table[row] = SCORE_LOST_PENALTY +
            SCORE_EMPTY_WEIGHT * empty +
            SCORE_MERGES_WEIGHT * merges -
            SCORE_MONOTONICITY_WEIGHT * std::min(monotonicity_left, monotonicity_right) -
            SCORE_SUM_WEIGHT * sum;

        // execute a move to the left
        for (int i = 0; i < 3; ++i) {
            int j;
            for (j = i + 1; j < 4; ++j) {
                if (line[j] != 0) break;
            }
            if (j == 4) break; // no more tiles to the right

            if (line[i] == 0) {
                line[i] = line[j];
                line[j] = 0;
                i--; // retry this entry
            } else if (line[i] == line[j]) {
                if (line[i] != MAX_RANK) {
                    /* Pretend that 2^31 + 2^31 = 2^31 (representational limit). */
                    line[i]++;
                }
                line[j] = 0;
            }
        }

        row_t result = (row_t)((line[0] <<  0) |
                               (line[1] <<  5) |
                               (line[2] << 10) |
                               (line[3] << 15));
        row_t rev_result = reverse_row(result);
        unsigned rev_row = reverse_row((row_t)row);

        row_left_table [     row] =                (row_t)row  ^                result;
        row_right_table[ rev_row] =            (row_t)rev_row  ^            rev_result;
    }
}

static inline board_t execute_move_0(board_t board) {
    /* up: merge each column upward */
    board_t ret = board;
    for (int c = 0; c < 4; ++c) {
        ret = xcol(ret, c, row_left_table[get_col(ret, c)]);
    }
    return ret;
}

static inline board_t execute_move_1(board_t board) {
    /* down: merge each column downward */
    board_t ret = board;
    for (int c = 0; c < 4; ++c) {
        ret = xcol(ret, c, row_right_table[get_col(ret, c)]);
    }
    return ret;
}

static inline board_t execute_move_2(board_t board) {
    /* left: merge each row leftward */
    board_t ret = board;
    for (int r = 0; r < 4; ++r) {
        ret = set_row(ret, r, get_row(ret, r) ^ row_left_table[get_row(ret, r)]);
    }
    return ret;
}

static inline board_t execute_move_3(board_t board) {
    /* right: merge each row rightward */
    board_t ret = board;
    for (int r = 0; r < 4; ++r) {
        ret = set_row(ret, r, get_row(ret, r) ^ row_right_table[get_row(ret, r)]);
    }
    return ret;
}

/* Execute a move. */
static inline board_t execute_move_internal(int move, board_t board) {
    switch(move) {
    case 0: // up
        return execute_move_0(board);
    case 1: // down
        return execute_move_1(board);
    case 2: // left
        return execute_move_2(board);
    case 3: // right
        return execute_move_3(board);
    default:
        return board_make(~0ULL, ~0ULL);
    }
}

/* Exported wrapper: read 80-bit board from buffer, write result back. */
void execute_move(int move, const uint64_t *in, uint64_t *out) {
    board_t r = execute_move_internal(move, board_make(in[0], in[1]));
    out[0] = r.lo;
    out[1] = r.hi;
}

static inline int get_max_rank(board_t board) {
    int maxrank = 0;
    for (int i = 0; i < 16; ++i) {
        maxrank = std::max(maxrank, get_tile(board, i));
    }
    return maxrank;
}

static inline int count_distinct_tiles(board_t board) {
    uint32_t bitset = 0;
    for (int i = 0; i < 16; ++i) {
        bitset |= 1u << (get_tile(board, i) & 0x1f);
    }

    // Don't count empty tiles.
    bitset >>= 1;

    int count = 0;
    while (bitset) {
        bitset &= bitset - 1;
        count++;
    }
    return count;
}

/* Optimizing the game */

struct eval_state {
    trans_table_t trans_table; // transposition table, to cache previously-seen moves
    int maxdepth;
    int curdepth;
    int cachehits;
    unsigned long moves_evaled;
    int depth_limit;

    eval_state() : maxdepth(0), curdepth(0), cachehits(0), moves_evaled(0), depth_limit(0) {
    }
};

// score a single board heuristically
static float score_heur_board(board_t board);
// score a single board actually (adding in the score from spawned 4 tiles)
static float score_board(board_t board);
// score over all possible moves
static float score_move_node(eval_state &state, board_t board, float cprob);
// score over all possible tile choices and placements
static float score_tilechoose_node(eval_state &state, board_t board, float cprob);


static float score_helper(board_t board, const float* table) {
    return table[get_row(board, 0)] +
           table[get_row(board, 1)] +
           table[get_row(board, 2)] +
           table[get_row(board, 3)];
}

static float score_heur_board(board_t board) {
    return score_helper(          board , heur_score_table) +
           score_helper(transpose(board), heur_score_table);
}

static float score_board(board_t board) {
    return score_helper(board, score_table);
}

static int count_empty(board_t board) {
    int n = 0;
    for (int i = 0; i < 16; ++i) {
        if (get_tile(board, i) == 0) n++;
    }
    return n;
}

// Statistics and controls
// cprob: cumulative probability
// don't recurse into a node with a cprob less than this threshold
static const int CACHE_DEPTH_LIMIT  = 15;

static float score_tilechoose_node(eval_state &state, board_t board, float cprob) {
    if (cprob < get_cprob_thresh() || state.curdepth >= state.depth_limit) {
        state.maxdepth = std::max(state.curdepth, state.maxdepth);
        return score_heur_board(board);
    }
    if (state.curdepth < CACHE_DEPTH_LIMIT) {
        const trans_table_t::iterator &i = state.trans_table.find(board);
        if (i != state.trans_table.end()) {
            trans_table_entry_t entry = i->second;
            /*
            return heuristic from transposition table only if it means that
            the node will have been evaluated to a minimum depth of state.depth_limit.
            This will result in slightly fewer cache hits, but should not impact the
            strength of the ai negatively.
            */
            if(entry.depth <= state.curdepth)
            {
                state.cachehits++;
                return entry.heuristic;
            }
        }
    }

    int num_open = count_empty(board);
    cprob /= num_open;

    float res = 0.0f;
    for (int i = 0; i < 16; ++i) {
        if (get_tile(board, i) == 0) {
            res += score_move_node(state, set_tile(board, i, 1), cprob * 0.9f) * 0.9f;
            res += score_move_node(state, set_tile(board, i, 2), cprob * 0.1f) * 0.1f;
        }
    }
    res = res / num_open;

    if (state.curdepth < CACHE_DEPTH_LIMIT) {
        trans_table_entry_t entry = {static_cast<uint8_t>(state.curdepth), res};
        state.trans_table[board] = entry;
    }

    return res;
}

static float score_move_node(eval_state &state, board_t board, float cprob) {
    float best = 0.0f;
    state.curdepth++;
    for (int move = 0; move < 4; ++move) {
        board_t newboard = execute_move_internal(move, board);
        state.moves_evaled++;

        if (!board_eq(board, newboard)) {
            best = std::max(best, score_tilechoose_node(state, newboard, cprob));
        }
    }
    state.curdepth--;

    return best;
}

static float _score_toplevel_move(eval_state &state, board_t board, int move) {
    //int maxrank = get_max_rank(board);
    board_t newboard = execute_move_internal(move, board);

    if(board_eq(board, newboard))
        return 0;

    return score_tilechoose_node(state, newboard, 1.0f) + 1e-6;
}

float score_toplevel_move(const uint64_t *board, int move) {
    float res;
    struct timeval start, finish;
    double elapsed;
    eval_state state;
    state.depth_limit = std::max(3, count_distinct_tiles(board_make(board[0], board[1])) - 2 + get_depth_adjust());

    gettimeofday(&start, NULL);
    res = _score_toplevel_move(state, board_make(board[0], board[1]), move);
    gettimeofday(&finish, NULL);

    elapsed = (finish.tv_sec - start.tv_sec);
    elapsed += (finish.tv_usec - start.tv_usec) / 1000000.0;

    printf("Move %d: result %f: eval'd %ld moves (%d cache hits, %d cache size) in %.2f seconds (maxdepth=%d)\n", move, res,
        state.moves_evaled, state.cachehits, (int)state.trans_table.size(), elapsed, state.maxdepth);

    return res;
}

/* Find the best move for a given board. */
static int find_best_move_internal(board_t board) {
    int move;
    float best = 0;
    int bestmove = -1;

    print_board(board);
    printf("Current scores: heur %.0f, actual %.0f\n", score_heur_board(board), score_board(board));

    for(move=0; move<4; move++) {
        uint64_t buf[2] = { board.lo, board.hi };
        float res = score_toplevel_move(buf, move);

        if(res > best) {
            best = res;
            bestmove = move;
        }
    }

    return bestmove;
}

int find_best_move(const uint64_t *board) {
    return find_best_move_internal(board_make(board[0], board[1]));
}

int ask_for_move(const uint64_t *board) {
    board_t b = board_make(board[0], board[1]);
    int move;
    char validstr[5];
    char *validpos = validstr;

    print_board(b);

    for(move=0; move<4; move++) {
        if(!board_eq(execute_move_internal(move, b), b))
            *validpos++ = "UDLR"[move];
    }
    *validpos = 0;
    if(validpos == validstr)
        return -1;

    while(1) {
        char movestr[64];
        const char *allmoves = "UDLR";

        printf("Move [%s]? ", validstr);

        if(!fgets(movestr, sizeof(movestr)-1, stdin))
            return -1;

        if(!strchr(validstr, toupper(movestr[0]))) {
            printf("Invalid move.\n");
            continue;
        }

        return strchr(allmoves, toupper(movestr[0])) - allmoves;
    }
}

/* Playing the game */
static int draw_tile() {
    return (unif_random(10) < 9) ? 1 : 2;
}

static board_t insert_tile_rand(board_t board, int rank) {
    int index = unif_random(count_empty(board));
    for (int i = 0; i < 16; ++i) {
        if (get_tile(board, i) == 0) {
            if (index == 0)
                return set_tile(board, i, rank);
            --index;
        }
    }
    return board;
}

static board_t initial_board() {
    board_t board = set_tile(board_make(0, 0), unif_random(16), draw_tile());
    return insert_tile_rand(board, draw_tile());
}

void play_game(get_move_func_t get_move) {
    board_t board = initial_board();
    int moveno = 0;
    int scorepenalty = 0; // "penalty" for obtaining free 4 tiles

    while(1) {
        int move;
        board_t newboard;

        for(move = 0; move < 4; move++) {
            if(!board_eq(execute_move_internal(move, board), board))
                break;
        }
        if(move == 4)
            break; // no legal moves

        printf("\nMove #%d, current score=%.0f\n", ++moveno, score_board(board) - scorepenalty);

        uint64_t buf[2] = { board.lo, board.hi };
        move = get_move(buf);
        if(move < 0)
            break;

        newboard = execute_move_internal(move, board);
        if(board_eq(newboard, board)) {
            printf("Illegal move!\n");
            moveno--;
            continue;
        }

        int rank = draw_tile();
        if (rank == 2) scorepenalty += 4;
        board = insert_tile_rand(newboard, rank);
    }

    print_board(board);
    printf("\nGame over. Your score is %.0f. The highest rank you achieved was %d.\n", score_board(board) - scorepenalty, get_max_rank(board));
}

static int find_best_move_wrap(const uint64_t *board) {
    return find_best_move_internal(board_make(board[0], board[1]));
}

int main() {
    init_tables();
    play_game(find_best_move_wrap);
}
