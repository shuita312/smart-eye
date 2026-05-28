#pragma once

#include <atomic>
#include <mutex>

#include "shared_types.hpp"
#include "threadsafe_queue.hpp"

// 全局运行上下文

// 所有线程共享这个对象
struct AppContext
{
    std::atomic<bool> running{true}; // 控制所有线程是否继续运行

    // 配置参数保护锁
    std::mutex config_mutex;
    UserConfig config;

    // 各线程之间的数据通道
    ThreadSafeQueue<TofFrame> tof_queue;        // ToF -> data _relay (暂时没有用)
    ThreadSafeQueue<RgbFrame> rgb_queue;        // RGB -> data _relay (暂时没有用)
    ThreadSafeQueue<RgbFrame> ai_input_queue;   // data _relay -> AI 推理
    ThreadSafeQueue<DetectionResult> det_queue; // AI -> data _relay
    ThreadSafeQueue<FusionFrame> fusion_queue;  // data _relay -> fus
    ThreadSafeQueue<JudgeFrame> judge_queue;    // fus -> 决策线程
    ThreadSafeQueue<AlertEvent> alert_queue;    // 决策 -> TTS
    ThreadSafeQueue<Bytes *> audio_queue;       // TTS -> Audio

    // 最新一帧缓存
    // 适合“最近帧配对”场景
    std::mutex latest_mutex; // 暂时没有使用
    TofFrame latest_tof;
    RgbFrame latest_rgb;
    DetectionResult latest_det;

    // 线程竞态保护
    std::atomic<bool> rgb_ready = false;
    std::atomic<bool> tof_ready = false;
};