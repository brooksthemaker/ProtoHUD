// ── snake_game.cpp ───────────────────────────────────────────────────────────
// Reference GameSource: classic Snake on a grid. Pure logic + an RGBA framebuffer
// (no GL/SDL), so it validates the whole game pipeline — gamepad → buttons →
// tick → frame → glasses + HUB75 — before a real engine (doomgeneric) is wired in.

#include "game_source.h"

#include <deque>
#include <random>

#include <opencv2/imgproc.hpp>

namespace game {
namespace {

class SnakeGame : public GameSource {
public:
    SnakeGame() { frame_.create(kH, kW, CV_8UC4); reset(); }

    const char* name() const override { return "Snake"; }

    void reset() override {
        body_.clear();
        const int cx = kGW / 2, cy = kGH / 2;
        body_.push_front({cx,     cy});   // tail
        body_.push_front({cx + 1, cy});
        body_.push_front({cx + 2, cy});   // head (front)
        dir_ = pending_ = {1, 0};
        dead_ = false; score_ = 0; acc_ = 0.0;
        place_food();
        render();
    }

    void tick(double dt, uint32_t b) override {
        // Queue a turn (ignore 180° reversals onto our own neck).
        if      ((b & BtnUp)    && dir_.y == 0) pending_ = {0, -1};
        else if ((b & BtnDown)  && dir_.y == 0) pending_ = {0,  1};
        else if ((b & BtnLeft)  && dir_.x == 0) pending_ = {-1, 0};
        else if ((b & BtnRight) && dir_.x == 0) pending_ = {1,  0};

        if (dead_) { if (b & (BtnStart | BtnA)) reset(); return; }

        acc_ += dt;
        while (acc_ >= kStep) { acc_ -= kStep; advance(); if (dead_) break; }
        render();
    }

    const cv::Mat& frame() const override { return frame_; }

private:
    static constexpr int    kGW = 24, kGH = 16, kCELL = 8;
    static constexpr int    kW = kGW * kCELL, kH = kGH * kCELL;
    static constexpr double kStep = 0.12;   // seconds per move

    void place_food() {
        std::uniform_int_distribution<int> dx(0, kGW - 1), dy(0, kGH - 1);
        for (int t = 0; t < 256; ++t) {
            cv::Point f{dx(rng_), dy(rng_)};
            bool on = false;
            for (const auto& p : body_) if (p == f) { on = true; break; }
            if (!on) { food_ = f; return; }
        }
        food_ = {0, 0};
    }

    void advance() {
        dir_ = pending_;
        cv::Point head = body_.front() + dir_;
        if (head.x < 0 || head.x >= kGW || head.y < 0 || head.y >= kGH) { dead_ = true; return; }
        for (const auto& p : body_) if (p == head) { dead_ = true; return; }
        body_.push_front(head);
        if (head == food_) { ++score_; place_food(); }   // grow
        else                 body_.pop_back();
    }

    void cell(cv::Point c, const cv::Scalar& col) {
        cv::rectangle(frame_, {c.x * kCELL, c.y * kCELL},
                      {c.x * kCELL + kCELL - 1, c.y * kCELL + kCELL - 1}, col, cv::FILLED);
    }

    void render() {
        frame_.setTo(cv::Scalar(12, 14, 20, 255));                 // RGBA background
        cell(food_, cv::Scalar(230, 60, 60, 255));                 // food (red)
        for (size_t i = 0; i < body_.size(); ++i)                  // snake (green; head brighter)
            cell(body_[i], i == 0 ? cv::Scalar(120, 255, 140, 255)
                                  : cv::Scalar(40, 200, 90, 255));
        if (dead_)
            cv::rectangle(frame_, {0, 0}, {kW - 1, kH - 1},
                          cv::Scalar(230, 40, 40, 255), 3);        // red border = game over
    }

    cv::Mat               frame_;
    std::deque<cv::Point> body_;
    cv::Point             dir_{1, 0}, pending_{1, 0}, food_{0, 0};
    bool                  dead_  = false;
    int                   score_ = 0;
    double                acc_   = 0.0;
    std::mt19937          rng_{std::random_device{}()};
};

}  // namespace

std::unique_ptr<GameSource> make_snake() { return std::make_unique<SnakeGame>(); }

}  // namespace game
