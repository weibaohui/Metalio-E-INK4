// main/display/screen/game_2048_screen/game_2048_core.cc
#include "game_2048_core.h"

#include <cstring>
#include <esp_random.h>

namespace game_2048 {

Game2048Core::Game2048Core() {
    Reset();
}

void Game2048Core::Reset() {
    std::memset(board_, 0, sizeof(board_));
    std::memset(undo_board_, 0, sizeof(undo_board_));
    score_ = 0;
    undo_score_ = 0;
    has_undo_ = false;
    is_over_ = false;
    has_won_ = false;

    SpawnRandomTile();
    SpawnRandomTile();
}

void Game2048Core::SpawnRandomTile() {
    struct Pos { int r, c; };
    Pos empty[kGridSize * kGridSize];
    int count = 0;

    for (int r = 0; r < kGridSize; ++r) {
        for (int c = 0; c < kGridSize; ++c) {
            if (board_[r][c] == 0) {
                empty[count++] = {r, c};
            }
        }
    }

    if (count == 0) return;

    const int idx = static_cast<int>(esp_random() % count);
    const int val = (esp_random() % 10 == 0) ? 4 : 2;  // 90% 出 2，10% 出 4
    board_[empty[idx].r][empty[idx].c] = val;
}

bool Game2048Core::CanMove() const {
    for (int r = 0; r < kGridSize; ++r) {
        for (int c = 0; c < kGridSize; ++c) {
            if (board_[r][c] == 0) return true;
            if (c + 1 < kGridSize && board_[r][c] == board_[r][c + 1]) return true;
            if (r + 1 < kGridSize && board_[r][c] == board_[r + 1][c]) return true;
        }
    }
    return false;
}

namespace {

// 单行合并辅助函数（入参 4 格，处理紧凑与单次合并）
bool ProcessLine(int line[kGridSize], int& score_add) {
    int original[kGridSize];
    std::memcpy(original, line, sizeof(original));

    // 1. 紧凑非零元素
    int compacted[kGridSize] = {0};
    int w = 0;
    for (int i = 0; i < kGridSize; ++i) {
        if (line[i] != 0) compacted[w++] = line[i];
    }

    // 2. 相邻合并（每格每次滑动至多合并一次）
    int merged[kGridSize] = {0};
    int mw = 0;
    for (int i = 0; i < w; ++i) {
        if (i + 1 < w && compacted[i] == compacted[i + 1]) {
            const int val = compacted[i] * 2;
            merged[mw++] = val;
            score_add += val;
            ++i;  // 跳过被合并的下一格
        } else {
            merged[mw++] = compacted[i];
        }
    }

    std::memcpy(line, merged, sizeof(merged));
    return std::memcmp(original, line, sizeof(original)) != 0;
}

}  // namespace

bool Game2048Core::SlideAndMerge(Direction dir) {
    bool changed = false;
    int add_score = 0;

    switch (dir) {
        case Direction::kLeft:
            for (int r = 0; r < kGridSize; ++r) {
                int line[kGridSize];
                for (int c = 0; c < kGridSize; ++c) line[c] = board_[r][c];
                if (ProcessLine(line, add_score)) {
                    changed = true;
                    for (int c = 0; c < kGridSize; ++c) board_[r][c] = line[c];
                }
            }
            break;

        case Direction::kRight:
            for (int r = 0; r < kGridSize; ++r) {
                int line[kGridSize];
                for (int c = 0; c < kGridSize; ++c) line[c] = board_[r][kGridSize - 1 - c];
                if (ProcessLine(line, add_score)) {
                    changed = true;
                    for (int c = 0; c < kGridSize; ++c) board_[r][kGridSize - 1 - c] = line[c];
                }
            }
            break;

        case Direction::kUp:
            for (int c = 0; c < kGridSize; ++c) {
                int line[kGridSize];
                for (int r = 0; r < kGridSize; ++r) line[r] = board_[r][c];
                if (ProcessLine(line, add_score)) {
                    changed = true;
                    for (int r = 0; r < kGridSize; ++r) board_[r][c] = line[r];
                }
            }
            break;

        case Direction::kDown:
            for (int c = 0; c < kGridSize; ++c) {
                int line[kGridSize];
                for (int r = 0; r < kGridSize; ++r) line[r] = board_[kGridSize - 1 - r][c];
                if (ProcessLine(line, add_score)) {
                    changed = true;
                    for (int r = 0; r < kGridSize; ++r) board_[kGridSize - 1 - r][c] = line[r];
                }
            }
            break;
    }

    if (changed) {
        score_ += add_score;
        if (score_ > best_score_) best_score_ = score_;
    }
    return changed;
}

bool Game2048Core::Move(Direction dir) {
    if (is_over_) return false;

    // 备份供撤销
    int prev_board[kGridSize][kGridSize];
    std::memcpy(prev_board, board_, sizeof(board_));
    const int prev_score = score_;

    if (!SlideAndMerge(dir)) {
        return false;
    }

    std::memcpy(undo_board_, prev_board, sizeof(prev_board));
    undo_score_ = prev_score;
    has_undo_ = true;

    SpawnRandomTile();

    // 达成 2048 检查
    if (!has_won_) {
        for (int r = 0; r < kGridSize; ++r) {
            for (int c = 0; c < kGridSize; ++c) {
                if (board_[r][c] >= 2048) {
                    has_won_ = true;
                    break;
                }
            }
        }
    }

    if (!CanMove()) {
        is_over_ = true;
    }

    return true;
}

bool Game2048Core::Undo() {
    if (!has_undo_) return false;

    std::memcpy(board_, undo_board_, sizeof(board_));
    score_ = undo_score_;
    has_undo_ = false;
    is_over_ = false;
    return true;
}

}  // namespace game_2048
