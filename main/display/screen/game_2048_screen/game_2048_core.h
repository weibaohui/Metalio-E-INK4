// main/display/screen/game_2048_screen/game_2048_core.h
#pragma once

#include <cstdint>

namespace game_2048 {

constexpr int kGridSize = 4;

enum class Direction {
    kUp = 0,
    kDown = 1,
    kLeft = 2,
    kRight = 3
};

class Game2048Core {
public:
    Game2048Core();

    void Reset();

    // 移动并合并。若有方块位移或合并返回 true，并自动生成新块(2 或 4)
    bool Move(Direction dir);

    // 撤销上一步
    bool Undo();

    bool has_undo() const { return has_undo_; }
    bool is_over() const { return is_over_; }
    bool has_won() const { return has_won_; }
    int score() const { return score_; }
    int best_score() const { return best_score_; }
    void set_best_score(int s) { if (s > best_score_) best_score_ = s; }

    int cell(int row, int col) const {
        if (row < 0 || row >= kGridSize || col < 0 || col >= kGridSize) return 0;
        return board_[row][col];
    }

private:
    void SpawnRandomTile();
    bool CanMove() const;
    bool SlideAndMerge(Direction dir);

    int board_[kGridSize][kGridSize]{};
    int undo_board_[kGridSize][kGridSize]{};
    int score_ = 0;
    int undo_score_ = 0;
    int best_score_ = 0;
    bool has_undo_ = false;
    bool is_over_ = false;
    bool has_won_ = false;
};

}  // namespace game_2048
