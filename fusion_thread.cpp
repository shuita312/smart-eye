#include <cstdint>
#include <vector>
#include <algorithm>
#include <cmath>

#include "fusion_thread.hpp"
#include "maix_basic.hpp"

#include "dbg.hpp"

using namespace maix;
using namespace maix::ext_dev::tof100;

// 获取当前毫秒时间戳
static uint64_t now_ms()
{
    return static_cast<uint64_t>(time::ticks_ms());
}

// 时间对齐窗口
constexpr uint64_t SAMP_TO_YOLO_MS = 200;
// 计算 |AI.ts_ms - tof.ts_ms|
static auto abs_diff_ms = [](uint64_t a, uint64_t b) -> uint64_t
{
    return (a > b) ? (a - b) : (b - a);
};

/*  函数 get_depth_from_box_center 的实现
// 从检测框中心映射到 ToF，取中心邻域的中值深度（mm）
// 参数说明：
// tof_matrix   : ToF 深度矩阵，单位 mm
// det_x/y/w/h  : 检测框，坐标基于 AI/RGB 图像
// rgb_width    : 检测图宽（通常就是 ai_input_size）
// rgb_height   : 检测图高
// rgb_dx/dy    : RGB 相对于 ToF 的平移补偿（和你融合函数保持一致）
// min_mm/max_mm: 合法深度范围，用于过滤无效值
// radius       : 取样半径。radius=1 表示取 3x3 邻域；radius=2 表示 5x5
//
// 返回值：
// 成功返回距离（mm），失败返回 0
*/
static uint32_t get_depth_from_box_center(
    const TOFMatrix &tof_matrix,
    int det_x, int det_y, int det_w, int det_h,
    int rgb_width, int rgb_height,
    int rgb_dx, int rgb_dy,
    int min_mm, int max_mm,
    int radius = 2)
{
    // ---------- 基本检查 ----------
    if (tof_matrix.empty() || tof_matrix[0].empty())
    {
        return 0;
    }
    if (rgb_width <= 0 || rgb_height <= 0)
    {
        return 0;
    }
    if (det_w <= 0 || det_h <= 0)
    {
        return 0;
    }
    if (max_mm <= min_mm)
    {
        return 0;
    }

    const int tof_h = static_cast<int>(tof_matrix.size());
    const int tof_w = static_cast<int>(tof_matrix[0].size());

    // ---------- 1. 取检测框中心 ----------
    const float cx = static_cast<float>(det_x) + static_cast<float>(det_w) * 0.5f;
    const float cy = static_cast<float>(det_y) + static_cast<float>(det_h) * 0.5f;

    // ---------- 2. 复用融合时的坐标映射 ----------
    // 和 fuse_tof_and_rgb 里的逻辑保持一致：
    //   src_x = (x - rgb_dx) * scale_x
    //   src_y = (y - rgb_dy) * scale_y
    const float scale_x = static_cast<float>(tof_w) / static_cast<float>(rgb_width);
    const float scale_y = static_cast<float>(tof_h) / static_cast<float>(rgb_height);

    const float src_x = (cx - static_cast<float>(rgb_dx)) * scale_x;
    const float src_y = (cy - static_cast<float>(rgb_dy)) * scale_y;

    // ---------- 3. 越界检查 ----------
    if (src_x < 0.0f || src_y < 0.0f ||
        src_x > static_cast<float>(tof_w - 1) ||
        src_y > static_cast<float>(tof_h - 1))
    {
        return 0;
    }

    // ---------- 4. 以映射中心为基准，取一个邻域 ----------
    const int tx = static_cast<int>(std::round(src_x));
    const int ty = static_cast<int>(std::round(src_y));

    std::vector<uint32_t> valid_depths;
    valid_depths.reserve((radius * 2 + 1) * (radius * 2 + 1));

    for (int dy = -radius; dy <= radius; ++dy)
    {
        for (int dx = -radius; dx <= radius; ++dx)
        {
            const int px = tx + dx;
            const int py = ty + dy;

            if (px < 0 || px >= tof_w || py < 0 || py >= tof_h)
            {
                continue;
            }

            uint32_t d = tof_matrix[py][px];

            // 过滤无效深度
            if (d < static_cast<uint32_t>(min_mm) ||
                d > static_cast<uint32_t>(max_mm))
            {
                continue;
            }

            valid_depths.push_back(d);
        }
    }

    if (valid_depths.empty())
    {
        return 0;
    }

    // ---------- 5. 取中值，比平均值更抗噪 ----------
    std::sort(valid_depths.begin(), valid_depths.end());
    return valid_depths[valid_depths.size() / 2];
}

// 按检测框中心判断 left / center / right
static ObjectPosition get_object_position(int x, int w, int img_w)
{
    int cx = x + w / 2;
    if (cx < img_w / 3)
    {
        return ObjectPosition::LEFT;
    }
    else if (cx < img_w * 2 / 3)
    {
        return ObjectPosition::CENTER;
    }
    else
    {
        return ObjectPosition::RIGHT;
    }
}

// 融合线程
// 功能：
// 1. 等待 数据中转线程传回的 FusionFrame 时间帧相近的 ToF 数据 和 ai处理结果
// 2. 进行数据融合
// 3. 输出 FusionFrame 给决策线程
void fusion_thread_func(AppContext &ctx)
{

    // 读取当前配置
    UserConfig cfg;
    {
        std::lock_guard<std::mutex> lock(ctx.config_mutex);
        cfg = ctx.config;
    }

    println("fusion thread: start");

    uint64_t last_log_ms = 0;

    //  主循环
    while (ctx.running.load())
    {

        // 1.等待data_relay 返回  处理后的结果
        FusionFrame fus;
        JudgeFrame judge; // 后续给决策线程的输入

        if (!ctx.fusion_queue.wait_pop(fus, ctx.running))
        {
            println("fusion thread: wait_pop fusion frame failed, exiting");
            break;
        }

        // 2.检测当前时间和 fus.tof.ts_ms 的时间差，如果在 SAMP_TO_YOLO_MS 以内，认为是相似帧数据，可以使用
        // 否则丢弃这帧数据，继续等待下一帧
        uint64_t now = now_ms();
        uint64_t dt_ms = abs_diff_ms(fus.det.ts_ms, now);
        if (dt_ms > SAMP_TO_YOLO_MS)
        {
            println("fusion thread: got frame with dt=%lu ms, skipping", dt_ms);
            last_log_ms = now;
            continue;
        }

        // 3.检测结果处理逻辑
        // 根据det 识别到的物体框和类别 ，计算tof深度 ，将 物体 ，和距离打包成一个结构体，推送给后续模块
        for (auto &obj : fus.det.boxes)
        {
            uint32_t depth_mm = get_depth_from_box_center(
                fus.tof.matrix,
                obj.x, obj.y, obj.w, obj.h, // 检测框坐标
                cfg.ai_input_w,             // rgb_width
                cfg.ai_input_h,             // rgb_height
                cfg.rgb_dx,                 // rgb_dx
                cfg.rgb_dy,                 // rgb_dy
                cfg.tof_min_mm,             // min_mm
                cfg.tof_max_mm              // max_mm
            );

            // 计算物体位置（左中右)
            ObjectPosition pos = get_object_position(obj.x, obj.w, cfg.ai_input_w);
            // 填充给决策的输入
            judge.objects.push_back({obj.class_id,
                                     obj.label,
                                     obj.score,
                                     depth_mm,
                                     pos});

            judge.ts_ms = fus.ts_ms; // 以 ToF 时间戳为准
        }

        // 4. 推送给后续模块： - 决策线程
        ctx.judge_queue.push(std::move(judge)); // 后续给决策线程

        // 给调度留一点余量
        time::sleep_ms(5);
    }
}
