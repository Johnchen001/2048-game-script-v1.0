import ctypes
import os
import sys

# ---------------------------------------------------------------------------
# C++ 核心库的 ctypes 绑定（v2：支持 65536 及更大方块）。
#
# 说明：2048.cpp 现在用 5 bit 存一个格子（rank 指数），16 格共 80 位，
# 通过两个 uint64（lo=第 0..7 格，hi=第 8..15 格）组成的 16 字节缓冲区
# 在 Python 与 C++ 之间传递。格子存的是"指数 rank"：
#   rank 0 = 空, rank 1 = 2, rank 2 = 4, ..., rank 15 = 32768,
#   rank 16 = 65536, rank 17 = 131072, ...（上限 rank 31 = 2^31）
# 本模块提供 to_c_board / from_c_board / to_c_index / from_c_index
# 在"面值棋盘"与"rank 棋盘"之间转换。
# ---------------------------------------------------------------------------

# rank 0..31 对应的面值表（2^0..2^31）
TILE_VALUES = [0] + [2 ** k for k in range(1, 32)]
VALUE_TO_RANK = {v: r for r, v in enumerate(TILE_VALUES)}


def _load_library():
    """按平台惯例查找已编译的 C++ 库并返回 CDLL 实例。"""
    for suffix in ['so', 'dll', 'dylib']:
        dllfn = os.path.join('bin', '2048.' + suffix)
        if os.path.isfile(dllfn):
            return ctypes.CDLL(dllfn)
    sys.exit("找不到 2048 核心库 bin/2048.{so,dll,dylib}，请先编译：\n"
             "  方式一：./configure && make\n"
             "  方式二：g++ -O3 -fPIC -shared -o bin/2048.so 2048.cpp\n"
             "编译成功后重试。")


ailib = _load_library()

ailib.init_tables()

# 80-bit 棋盘 = 2 个 uint64 组成的 16 字节缓冲区
BOARD_BUF = ctypes.c_uint64 * 2

ailib.find_best_move.argtypes = [ctypes.POINTER(ctypes.c_uint64)]
ailib.find_best_move.restype = ctypes.c_int
ailib.score_toplevel_move.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.c_int]
ailib.score_toplevel_move.restype = ctypes.c_float
ailib.execute_move.argtypes = [ctypes.c_int, ctypes.POINTER(ctypes.c_uint64),
                               ctypes.POINTER(ctypes.c_uint64)]
ailib.ask_for_move.argtypes = [ctypes.POINTER(ctypes.c_uint64)]
ailib.ask_for_move.restype = ctypes.c_int


def to_c_board(m):
    """把 4x4 rank 棋盘（0=空, 1=2, 2=4, 3=8, ..., 16=65536, ...）打包成缓冲区。

    注意：本函数接收的是 rank 而不是面值。浏览器/手动控制等路径返回的
    棋盘已经是 rank（见 gamectrl.get_board / manualctrl），直接传入即可；
    若你手头是面值棋盘，请先经 to_c_index() 逐格转换。
    """
    buf = BOARD_BUF(0, 0)
    lo = 0
    hi = 0
    for i in range(16):
        v = int(m[i // 4][i % 4]) & 0x1f
        if i < 8:
            lo |= v << (5 * i)
        else:
            hi |= v << (5 * (i - 8))
    buf[0] = lo
    buf[1] = hi
    return buf


def from_c_board(n):
    """把 80-bit 棋盘（缓冲区或 (lo, hi) 可迭代）解包成 4x4 rank 棋盘。

    若需要面值，请再用 from_c_index() 逐格换算（或参考 2048.py 的 to_val）。
    """
    lo = int(n[0])
    hi = int(n[1])
    board = []
    for r in range(4):
        row = []
        for c in range(4):
            i = 4 * r + c
            w = lo if i < 8 else hi
            row.append((w >> (5 * (i & 7))) & 0x1f)
        board.append(row)
    return board


def to_c_index(n):
    """面值 -> rank（例如 2048 -> 11，65536 -> 16）。"""
    return VALUE_TO_RANK[n]


def from_c_index(c):
    """rank -> 面值（例如 11 -> 2048，16 -> 65536）。"""
    return TILE_VALUES[c]
