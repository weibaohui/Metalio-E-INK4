// main/display/screen/tetris_screen/tetris_core.cc
#include "tetris_core.h"

#include <esp_random.h>

namespace tetris {

// 7 种方块 × 4 旋转态，rot0 为出场朝向；y 向下
const int TetrisCore::kShapes[kTypes][4][4][2] = {
    {  // I
        {{0, 1}, {1, 1}, {2, 1}, {3, 1}},
        {{2, 0}, {2, 1}, {2, 2}, {2, 3}},
        {{0, 2}, {1, 2}, {2, 2}, {3, 2}},
        {{1, 0}, {1, 1}, {1, 2}, {1, 3}},
    },
    {  // O
        {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
        {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
        {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
        {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
    },
    {  // T
        {{1, 0}, {0, 1}, {1, 1}, {2, 1}},
        {{1, 0}, {1, 1}, {2, 1}, {1, 2}},
        {{0, 1}, {1, 1}, {2, 1}, {1, 2}},
        {{1, 0}, {0, 1}, {1, 1}, {1, 2}},
    },
    {  // S
        {{1, 0}, {2, 0}, {0, 1}, {1, 1}},
        {{1, 0}, {1, 1}, {2, 1}, {2, 2}},
        {{1, 1}, {2, 1}, {0, 2}, {1, 2}},
        {{0, 0}, {0, 1}, {1, 1}, {1, 2}},
    },
    {  // Z
        {{0, 0}, {1, 0}, {1, 1}, {2, 1}},
        {{2, 0}, {1, 1}, {2, 1}, {1, 2}},
        {{0, 1}, {1, 1}, {1, 2}, {2, 2}},
        {{1, 0}, {0, 1}, {1, 1}, {0, 2}},
    },
    {  // L
        {{2, 0}, {0, 1}, {1, 1}, {2, 1}},
        {{1, 0}, {1, 1}, {1, 2}, {2, 2}},
        {{0, 1}, {1, 1}, {2, 1}, {0, 2}},
        {{0, 0}, {1, 0}, {1, 1}, {1, 2}},
    },
    {  // J
        {{0, 0}, {0, 1}, {1, 1}, {2, 1}},
        {{1, 0}, {2, 0}, {1, 1}, {1, 2}},
        {{0, 1}, {1, 1}, {2, 1}, {2, 2}},
        {{1, 0}, {1, 1}, {0, 2}, {1, 2}},
    },
};

void TetrisCore::Reset() {
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) board_[y][x] = 0;
    }
    type_ = rot_ = px_ = py_ = 0;
    next_ = static_cast<int>(esp_random() % kTypes);
    hold_ = -1;
    can_hold_ = true;
    state_ = State::Running;
    score_ = lines_ = pieces_ = 0;
    level_ = 1;
    drop_ms_ = 800;
    Spawn();
}

bool TetrisCore::Fits(int type, int rot, int px, int py) const {
    for (int i = 0; i < 4; ++i) {
        const int x = px + kShapes[type][rot][i][0];
        const int y = py + kShapes[type][rot][i][1];
        if (x < 0 || x >= kW || y >= kH) return false;
        if (y < 0) continue;  // 板顶缓冲区允许（锁定时判 Over）
        if (board_[y][x] != 0) return false;
    }
    return true;
}

int TetrisCore::GhostY() const {
    int y = py_;
    while (Fits(type_, rot_, px_, y + 1)) ++y;
    return y;
}

void TetrisCore::current_cells(int out[4][2]) const {
    for (int i = 0; i < 4; ++i) {
        out[i][0] = px_ + kShapes[type_][rot_][i][0];
        out[i][1] = py_ + kShapes[type_][rot_][i][1];
    }
}

void TetrisCore::ghost_cells(int out[4][2]) const {
    const int gy = GhostY();
    for (int i = 0; i < 4; ++i) {
        out[i][0] = px_ + kShapes[type_][rot_][i][0];
        out[i][1] = gy + kShapes[type_][rot_][i][1];
    }
}

void TetrisCore::next_cells(int out[4][2]) const {
    for (int i = 0; i < 4; ++i) {
        out[i][0] = kShapes[next_][0][i][0];
        out[i][1] = kShapes[next_][0][i][1];
    }
}

void TetrisCore::hold_cells(int out[4][2]) const {
    if (hold_ < 0 || hold_ >= kTypes) {
        for (int i = 0; i < 4; ++i) {
            out[i][0] = 0;
            out[i][1] = 0;
        }
        return;
    }
    for (int i = 0; i < 4; ++i) {
        out[i][0] = kShapes[hold_][0][i][0];
        out[i][1] = kShapes[hold_][0][i][1];
    }
}

bool TetrisCore::Move(int dx) {
    if (state_ != State::Running) return false;
    if (!Fits(type_, rot_, px_ + dx, py_)) return false;
    px_ += dx;
    return true;
}

bool TetrisCore::Rotate() {
    if (state_ != State::Running) return false;
    const int nr = (rot_ + 1) % 4;
    // 简单踢墙：原位 → 左移1 → 右移1 → 上移1
    constexpr int kKick[4][2] = {{0, 0}, {-1, 0}, {1, 0}, {0, -1}};
    for (const auto& k : kKick) {
        if (Fits(type_, nr, px_ + k[0], py_ + k[1])) {
            rot_ = nr;
            px_ += k[0];
            py_ += k[1];
            return true;
        }
    }
    return false;
}

bool TetrisCore::Hold() {
    if (state_ != State::Running || !can_hold_) return false;
    if (hold_ < 0) {
        hold_ = type_;
        Spawn();
    } else {
        const int t = type_;
        type_ = hold_;
        hold_ = t;
        rot_ = 0;
        px_ = 3;
        py_ = 0;
        if (!Fits(type_, rot_, px_, py_)) state_ = State::Over;
    }
    can_hold_ = false;
    return true;
}

void TetrisCore::SoftDrop() {
    if (state_ != State::Running) return;
    if (Fits(type_, rot_, px_, py_ + 1)) ++py_;
}

void TetrisCore::HardDrop() {
    if (state_ != State::Running) return;
    py_ = GhostY();
    Tick();
}

void TetrisCore::TogglePause() {
    if (state_ == State::Running) state_ = State::Paused;
    else if (state_ == State::Paused) state_ = State::Running;
}

void TetrisCore::Tick() {
    if (state_ != State::Running) return;
    if (Fits(type_, rot_, px_, py_ + 1)) {
        ++py_;
        return;
    }
    // 锁定
    bool lock_out = false;
    for (int i = 0; i < 4; ++i) {
        const int x = px_ + kShapes[type_][rot_][i][0];
        const int y = py_ + kShapes[type_][rot_][i][1];
        if (y < 0) {
            lock_out = true;
            continue;
        }
        board_[y][x] = 1;
    }
    ClearLines();
    can_hold_ = true;
    ++pieces_;
    Spawn();
    if (lock_out) state_ = State::Over;
}

int TetrisCore::ClearLines() {
    int cleared = 0;
    for (int y = kH - 1; y >= 0; --y) {
        bool full = true;
        for (int x = 0; x < kW; ++x) {
            if (board_[y][x] == 0) {
                full = false;
                break;
            }
        }
        if (!full) continue;
        ++cleared;
        for (int yy = y; yy > 0; --yy) {
            for (int x = 0; x < kW; ++x) board_[yy][x] = board_[yy - 1][x];
        }
        for (int x = 0; x < kW; ++x) board_[0][x] = 0;
        ++y;  // 该行重新检查（上方行已下移）
    }
    if (cleared > 0) {
        static constexpr int kPts[4] = {100, 300, 500, 800};
        score_ += kPts[cleared - 1] * level_;
        lines_ += cleared;
        level_ = lines_ / 10 + 1;
        drop_ms_ = 800 - (level_ - 1) * 70;
        if (drop_ms_ < 150) drop_ms_ = 150;
    }
    return cleared;
}

void TetrisCore::Spawn() {
    type_ = next_;
    next_ = static_cast<int>(esp_random() % kTypes);
    rot_ = 0;
    px_ = 3;
    py_ = 0;
    if (!Fits(type_, rot_, px_, py_)) state_ = State::Over;
}

}  // namespace tetris
