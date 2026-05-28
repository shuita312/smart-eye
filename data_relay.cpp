#include <cstdint>
#include <vector>

#include "fusion_thread.hpp"

#include "maix_basic.hpp"
#include "maix_image.hpp"

#include "maix_cmap.hpp"

#include "dbg.hpp"

using namespace maix;
using namespace maix::image;
using namespace maix::ext_dev::tof100;

// 获取当前毫秒时间戳
static uint64_t now_ms()
{
    return static_cast<uint64_t>(time::ticks_ms());
}

// 时间对齐窗口
constexpr uint64_t SYNC_DELTA_MS = 50;
// 计算 |tof.ts_ms - rgb.ts_ms|
static auto abs_diff_ms = [](uint64_t a, uint64_t b) -> uint64_t
{
    return (a > b) ? (a - b) : (b - a);
};

/*  第一版  融合函数
第二版
融合 ToF + RGB（单次遍历版本）
// 核心思想：
// 不再先 upscale + crop，而是对每个 RGB 像素反算到 ToF 坐标，
// 现场做双线性插值，然后直接融合。
// 优点：
// - 少两次内存分配
// - 少两次全图遍历
// - cache 友好
// - 实时性更好


// 融合 ToF + RGB（增强版：直接映射 + 双线性插值 + 距离归一化）
// 改进点：
// 1. 不再先 upscale / crop，再融合，而是直接在融合时做坐标映射。
// 2. ToF 深度值按 [min_mm, max_mm] 归一化到 [0,255]。
// 3. 超出量程的点当作无效点处理，避免画面异常发白。
// 4. 保持单次遍历，减少内存分配和 CPU 开销。

//  第三版 融合 对于第二版加了camp，主要是为了调试用，方便观察不同距离的点在融合图上的表现
std::shared_ptr<Image> fuse_tof_and_rgb(
    const TOFMatrix &tof_matrix,                    // ToF 深度矩阵（单位：mm）
    const std::shared_ptr<Image> &rgb_img,          // RGB 图像（RGB888）
    int rgb_dx,                                     // RGB 相对于 ToF 的水平偏移（右正）
    int rgb_dy,                                     // RGB 相对于 ToF 的垂直偏移（下正）
    float alpha,                                    // ToF 融合权重 [0,1]
    int min_mm,                                     // ToF 最小有效距离
    int max_mm,                                     // ToF 最大有效距离
    maix::ext_dev::cmap::Cmap cmap_type             // 伪彩色类型
) {
    using namespace maix::ext_dev;

    // ---------- 基本检查 ----------
    if (!rgb_img || tof_matrix.empty() || tof_matrix[0].empty()) {
        return nullptr;
    }

    const int rgb_width  = rgb_img->width();
    const int rgb_height = rgb_img->height();

    const int tof_height = static_cast<int>(tof_matrix.size());
    const int tof_width  = static_cast<int>(tof_matrix[0].size());

    if (rgb_width <= 0 || rgb_height <= 0 || tof_width <= 0 || tof_height <= 0) {
        return nullptr;
    }

    if (max_mm <= min_mm) {
        return nullptr;
    }

    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;

    auto *rgb_data = static_cast<uint8_t *>(rgb_img->data());
    if (!rgb_data) {
        return nullptr;
    }

    // 取伪彩色表
    const auto *cmap_array = cmap::get(cmap_type);
    if (!cmap_array || cmap_array->empty()) {
        return nullptr;
    }

    std::vector<uint8_t> fused_image_data(rgb_width * rgb_height * 3);

    const float scale_x = static_cast<float>(tof_width)  / static_cast<float>(rgb_width);
    const float scale_y = static_cast<float>(tof_height) / static_cast<float>(rgb_height);

    const float range_mm = static_cast<float>(max_mm - min_mm);

    auto clamp_u8 = [](float v) -> uint8_t {
        if (v < 0.0f) return 0;
        if (v > 255.0f) return 255;
        return static_cast<uint8_t>(v);
    };

    // 深度值(mm) -> cmap 颜色
    auto depth_to_cmap_rgb =
        [min_mm, max_mm, range_mm, cmap_array](float depth_mm, uint8_t &out_r, uint8_t &out_g, uint8_t &out_b)
    {
        // 无效值或越界值，直接黑色
        if (depth_mm < static_cast<float>(min_mm) || depth_mm > static_cast<float>(max_mm)) {
            out_r = 0;
            out_g = 0;
            out_b = 0;
            return;
        }

        // 归一化到 0~1
        float norm = (depth_mm - static_cast<float>(min_mm)) / range_mm;
        if (norm < 0.0f) norm = 0.0f;
        if (norm > 1.0f) norm = 1.0f;

        // 近距离亮/热，远距离冷
        // 和你前面 tof_matrix_to_image 的思路保持一致：用 (size-1-index)
        uint32_t idx = static_cast<uint32_t>(
            cmap_array->size() - 1 -
            static_cast<int>(norm * static_cast<float>(cmap_array->size() - 1))
        );

        out_r = std::get<0>((*cmap_array)[idx]);
        out_g = std::get<1>((*cmap_array)[idx]);
        out_b = std::get<2>((*cmap_array)[idx]);
    };

    // 主循环：直接对每个 RGB 像素映射到 ToF
    for (int y = 0; y < rgb_height; ++y) {
        for (int x = 0; x < rgb_width; ++x) {

            // RGB 坐标映射到 ToF 浮点坐标
            float src_x = static_cast<float>(x - rgb_dx) * scale_x;
            float src_y = static_cast<float>(y - rgb_dy) * scale_y;

            uint8_t tr = 0, tg = 0, tb = 0;

            // 越界时视为无效深度
            if (src_x >= 0.0f && src_y >= 0.0f &&
                src_x <= static_cast<float>(tof_width - 1) &&
                src_y <= static_cast<float>(tof_height - 1)) {

                // 双线性插值
                int x1 = static_cast<int>(std::floor(src_x));
                int y1 = static_cast<int>(std::floor(src_y));
                int x2 = std::min(x1 + 1, tof_width - 1);
                int y2 = std::min(y1 + 1, tof_height - 1);

                float dx = src_x - static_cast<float>(x1);
                float dy = src_y - static_cast<float>(y1);

                float d11 = static_cast<float>(tof_matrix[y1][x1]);
                float d12 = static_cast<float>(tof_matrix[y1][x2]);
                float d21 = static_cast<float>(tof_matrix[y2][x1]);
                float d22 = static_cast<float>(tof_matrix[y2][x2]);

                float top    = d11 * (1.0f - dx) + d12 * dx;
                float bottom = d21 * (1.0f - dx) + d22 * dx;
                float depth  = top * (1.0f - dy) + bottom * dy;

                // 深度值转伪彩色
                depth_to_cmap_rgb(depth, tr, tg, tb);
            }

            // 与 RGB 融合
            int idx = (y * rgb_width + x) * 3;

            uint8_t r = rgb_data[idx + 0];
            uint8_t g = rgb_data[idx + 1];
            uint8_t b = rgb_data[idx + 2];

            fused_image_data[idx + 0] = clamp_u8(alpha * tr + (1.0f - alpha) * r);
            fused_image_data[idx + 1] = clamp_u8(alpha * tg + (1.0f - alpha) * g);
            fused_image_data[idx + 2] = clamp_u8(alpha * tb + (1.0f - alpha) * b);
        }
    }

    return std::make_shared<Image>(
        rgb_width,
        rgb_height,
        FMT_RGB888,
        fused_image_data.data(),
        static_cast<int>(fused_image_data.size()),
        true
    );
}
*/

// 逻辑：
// 1. 读取最新的 ToF 和 RGB 数据（要求已经裁剪对齐）
// 2. 如果时间戳不匹配（不同步），跳过本轮
// 3. 把对应的rgb数据发送给 AI yolo线程
// 4. 等待yolo返回 处理后的结果
// 5. 把相近时间戳的Tof 和 检测结果数据 输出给 融合线程
void data_relay_thread_func(AppContext &ctx)
{

    // 启动前等待 RGB 和 ToF 线程准备就绪
    while (ctx.running.load())
    {
        if (ctx.rgb_ready && ctx.tof_ready)
            break;
        time::sleep_ms(10);
    }

    if (!ctx.running.load())
        return;

    println("Data relay thread: started");
    uint64_t last_tof_ts = 0;
    uint64_t last_rgb_ts = 0;

    // 读取当前配置
    UserConfig cfg;
    {
        std::lock_guard<std::mutex> lock(ctx.config_mutex);
        cfg = ctx.config;
    }

    while (ctx.running.load())
    {

        // 1.读取最新的 Tof 数据 和 RGB 图像
        TofFrame tof;
        RgbFrame rgb;

        {
            std::lock_guard<std::mutex> lock(ctx.latest_mutex);
            tof = ctx.latest_tof;
            rgb = ctx.latest_rgb;
        }

        // 2.如果当前还没有可用的图，跳过本轮
        if (!rgb.raw_image || tof.matrix.empty())
        {
            time::sleep_ms(5);
            continue;
        }
        // ---------- 去重 ----------
        if (tof.ts_ms == last_tof_ts && rgb.ts_ms == last_rgb_ts)
        {
            time::sleep_ms(5);
            continue;
        }

        last_tof_ts = tof.ts_ms;
        last_rgb_ts = rgb.ts_ms;

        // 没有有效时间戳(时间同步)，跳过
        uint64_t dt_ms = abs_diff_ms(tof.ts_ms, rgb.ts_ms);
        if (dt_ms > SYNC_DELTA_MS)
        {
            println("data_relay skip: dt=%lu ms", dt_ms);
            continue;
        }

        // 3. 把对应的rgb数据发送给 AI yolo线程
        ctx.ai_input_queue.push(std::move(rgb));

        // println("data_relay: pushed RGB frame to AI thread, ts=%lu ms", last_rgb_ts);

        // 4. 等待yolo返回 处理后的结果
        DetectionResult det;
        if (!ctx.det_queue.wait_pop(det, ctx.running))
        {
            println("data_relay: wait_pop det failed, exiting");
            break;
        }

        // 这里可以根据 det.valid 判断推理结果是否有效，或者根据业务需求进行过滤
        if (det.valid)
        {
            // 5. 把相近时间戳的Tof 和 检测数据 输出给 融合线程
            FusionFrame out;
            out.tof = std::move(tof); // 关联的 ToF 数据
            out.ts_ms = tof.ts_ms;    // 以 ToF 时间戳为准
            out.det = std::move(det); // 检测结果

            ctx.fusion_queue.push(std::move(out)); // 输出给融合线程
        }

        // 避免过度占用CPU
        time::sleep_ms(5);
    }
}