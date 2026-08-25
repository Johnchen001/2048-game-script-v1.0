from __future__ import print_function

from ailib import ailib, to_c_board, from_c_board, to_c_index, from_c_index, TILE_VALUES, BOARD_BUF
from gamectrl import Generic2048Control

try:
    input = raw_input
except NameError:
    pass

def print_board(m):
    for row in m:
        for c in row:
            print('%8d' % from_c_index(c), end=' ')
        print()

def _prompt(text):
    """读取一行输入；遇到 EOF（Ctrl-D / Ctrl-Z）时优雅退出。"""
    try:
        return input(text)
    except EOFError:
        print("\n已退出教练模式。")
        raise SystemExit(0)

def parse_tile(text):
    """把用户输入解析为 rank；非法输入抛出 ValueError（非整数）或 KeyError（非 2 的幂）。"""
    n = int(text)
    return to_c_index(n)

class ManualControl(Generic2048Control):
    def __init__(self):
        print("Enter board one row at a time, with entries separated by spaces")
        print("支持的数值：0, 2, 4, 8, ..., %d" % TILE_VALUES[-1])
        board = []
        for ri in range(4):
            while True:
                try:
                    board.append([parse_tile(c) for c in _prompt("Row %d: " % (ri + 1)).split()])
                    break
                except (ValueError, KeyError):
                    print("  输入必须是 0 或 2 的幂（最大 %d），请重新输入。" % TILE_VALUES[-1])
        self.cur_board = board

    def get_status(self):
        return "running"

    def restart_game(self):
        print("Game over - time to restart!")

    def continue_game(self):
        pass

    def get_score(self):
        # don't care
        return 0

    def get_board(self):
        print("Current board:")
        print_board(self.cur_board)

        while True:
            updates = _prompt("Enter update(s) in the form r,c,n (1-indexed row/column); separate multiple updates by spaces: ")
            try:
                for item in updates.split():
                    r, c, n = map(int, item.split(","))
                    self.cur_board[r-1][c-1] = to_c_index(n)
                break
            except (ValueError, KeyError, IndexError):
                print("  更新格式应为 行,列,值（1 起始，值须为 0 或 2 的幂），请重新输入。")

        return self.cur_board

    def execute_move(self, move):
        print("EXECUTE MOVE:", ["up", "down", "left", "right"][move])
        out = BOARD_BUF(0, 0)
        ailib.execute_move(move, to_c_board(self.cur_board), out)
        self.cur_board = from_c_board(out)
