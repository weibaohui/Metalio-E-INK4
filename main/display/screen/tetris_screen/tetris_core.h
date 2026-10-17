#pragma once
// main/display/screen/tetris_screen/tetris_core.h
// 俄罗斯方块纯逻辑核心：无 LVGL、无锁，只做内存运算（出块随机用 esp_random）。
// 只在 LVGL 线程触碰（lv_timer 重力步进 + lv_async 输入投递），无需加锁。
#include <cstdint>

namespace tetris {

constexpr int kW = 10;
constexpr int kH = 20;

enum class State { Running, Paused, Over };

class TetrisCore {
public:
    void Reset();

    // 输入；返回是否发生有效动作（供重绘判断）
    bool Move(int dx);
    bool Rotate();
    bool Hold();       // 暂存交换（每块限一次）
    void HardDrop();
    void SoftDrop();   // 软降一格，到底即锁定
    void TogglePause();
    void Tick();       // 重力步进（定时器按 drop_interval_ms 调用）

    bool filled(int x, int y) const { return board_[y][x] != 0; }
    State state() const { return state_; }
    int score() const { return score_; }
    int lines() const { return lines_; }
    int level() const { return level_; }
    int pieces() const { return pieces_; }  // 已落定块数(全刷节奏用)
    int drop_interval_ms() const { return drop_ms_; }

    // current/ghost 输出棋盘绝对坐标；next/hold 输出形状相对坐标（预览用）
    void current_cells(int out[4][2]) const;
    void ghost_cells(int out[4][2]) const;
    void next_cells(int out[4][2]) const;
    void hold_cells(int out[4][2]) const;
    bool has_hold() const { return hold_ >= 0; }

private:
    static constexpr int kTypes = 7;
    // [type][rot][cell][x,y]，rot 顺时针；y 向下
    static const int kShapes[kTypes][4][4][2];

    bool Fits(int type, int rot, int px, int py) const;
    int ClearLines();  // 返回消除行数
    void Spawn();
    int GhostY() const;

    uint8_t board_[kH][kW] = {};
    int type_ = 0, rot_ = 0, px_ = 0, py_ = 0;
    int next_ = 0, hold_ = -1;
    bool can_hold_ = true;
    State state_ = State::Running;
    int score_ = 0, lines_ = 0, level_ = 1, pieces_ = 0;
    int drop_ms_ = 800;
};

}  // namespace tetris
