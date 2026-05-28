#include "tof_thread.hpp"
#include "maix_basic.hpp"
#include "maix_tof100.hpp"
#include "dbg.hpp"

using namespace maix;
using namespace maix::ext_dev::tof100;

// 获取当前毫秒时间戳
static uint64_t now_ms()
{
    return static_cast<uint64_t>(time::ticks_ms());
}

// ToF 采集线程
// 负责持续从 ToF 传感器读取深度矩阵
// 1. 初始化 ToF 设备
// 2. 读取深度矩阵，转换成 ToF 帧
// 3. 更新最新 ToF 帧缓存，设置标志位
// 4. 推送到融合线程（可选，当前改为直接更新缓存）
void tof_capture_thread_func(AppContext &ctx)
{
    int spi_num = 2; // MaixCAM2
    int empty_count = 0;
    uint64_t last_ok_ms = 0;
    std::unique_ptr<Tof100> tof;

    // 读取当前配置
    UserConfig cfg;
    {
        // 加锁读取配置，确保线程安全
        std::lock_guard<std::mutex> lock(ctx.config_mutex);
        // 拷贝副本，避免长时间持锁
        cfg = ctx.config;
        // 自动在生命周期解锁
    }

    while (ctx.running.load())
    {

        // 由于设备不稳定 ，每次循环先尝试初始化
        if (!tof)
        {
            try
            {
                tof.reset(new Tof100(spi_num, cfg.tof_res, cfg.tof_cmap, cfg.tof_min_mm, cfg.tof_max_mm));
                empty_count = 0;
                last_ok_ms = now_ms();
                // 初始化后先等设备稳定
                time::sleep_ms(200);
                println("ToF init OK");
            }
            catch (...)
            {
                eprintln("ToF init failed");
                time::sleep_ms(100);
                continue;
            }
        }

        // 读取一帧深度矩阵
        TOFMatrix matrix;
        try
        {
            matrix = tof->matrix();
        }
        catch (...)
        {
            eprintln("ToF matrix() exception, recreate device");
            tof.reset();
            time::sleep_ms(100);
            continue;
        }

        // 如果数据异常（全零），可能是设备又卡住了，重试几次，如果持续异常则重启设备
        if (matrix.empty())
        {
            empty_count++;
            if (empty_count >= 30 || now_ms() - last_ok_ms > 500)
            {
                eprintln("ToF empty too long, recreate device");
                tof.reset();
                time::sleep_ms(100);
            }
            else
            {
                time::sleep_ms(10);
            }
            continue;
        }

        empty_count = 0;
        last_ok_ms = now_ms();

        TofFrame frame;
        frame.matrix = std::move(matrix);
        frame.res = cfg.tof_res;
        frame.ts_ms = last_ok_ms;

        // 推送到融合线程
        // ctx.tof_queue.push(frame);

        // 更新最新 ToF 帧缓存
        {
            std::lock_guard<std::mutex> lock(ctx.latest_mutex);
            ctx.latest_tof = std::move(frame);
            // 标志位：ToF 已准备好
            ctx.tof_ready = true;
        }

        // 休息一下，避免过度占用 CPU
        time::sleep_ms(5);
    }
}

/*
用法示例：
int wh = static_cast<int>(cfg.tof_res);
auto depth_img = tof_matrix_to_image(
    matrix,
    wh,
    cfg.tof_cmap,
    cfg.tof_min_mm,
    cfg.tof_max_mm
);
*/
// 将 ToF 深度矩阵转换成伪彩色图像
std::shared_ptr<maix::image::Image> tof_matrix_to_image(
    const maix::ext_dev::tof100::TOFMatrix &matrix,
    int wh,
    maix::ext_dev::cmap::Cmap cmap_type,
    int min_mm,
    int max_mm)
{
    using namespace maix;
    using namespace maix::image;

    if (matrix.empty() || matrix[0].empty() || wh <= 0)
    {
        return nullptr;
    }

    const auto *array = maix::ext_dev::cmap::get(cmap_type);
    if (!array || array->empty())
    {
        return nullptr;
    }

    int max_v = max_mm;
    int min_v = min_mm;
    if (max_v <= min_v)
    {
        uint32_t local_min = std::numeric_limits<uint32_t>::max();
        uint32_t local_max = std::numeric_limits<uint32_t>::min();
        for (const auto &row : matrix)
        {
            for (auto d : row)
            {
                if (d < local_min)
                    local_min = d;
                if (d > local_max)
                    local_max = d;
            }
        }
        min_v = static_cast<int>(local_min);
        max_v = static_cast<int>(local_max);
        if (max_v <= min_v)
            max_v = min_v + 1;
    }

    const int range = max_v - min_v;
    std::vector<uint8_t> buffer(wh * wh * 3);

    int pixel_cnt = 0;
    for (const auto &line : matrix)
    {
        for (auto dis_u32 : line)
        {
            int dis = static_cast<int>(dis_u32);
            dis -= min_v;
            if (dis < 0)
                dis = 0;
            if (dis > range)
                dis = range;

            uint32_t index = static_cast<uint32_t>(
                array->size() - 1 - static_cast<int>(floor(static_cast<float>(dis) / range * (array->size() - 1))));

            buffer[pixel_cnt * 3 + 0] = std::get<0>((*array)[index]);
            buffer[pixel_cnt * 3 + 1] = std::get<1>((*array)[index]);
            buffer[pixel_cnt * 3 + 2] = std::get<2>((*array)[index]);
            pixel_cnt++;
        }
    }

    if (wh == 25)
    {
        std::vector<uint8_t> up(50 * 50 * 3);
        for (int y = 0; y < 25; ++y)
        {
            for (int x = 0; x < 25; ++x)
            {
                int src = (y * 25 + x) * 3;
                for (int dy = 0; dy < 2; ++dy)
                {
                    for (int dx = 0; dx < 2; ++dx)
                    {
                        int ny = y * 2 + dy;
                        int nx = x * 2 + dx;
                        int dst = (ny * 50 + nx) * 3;
                        up[dst + 0] = buffer[src + 0];
                        up[dst + 1] = buffer[src + 1];
                        up[dst + 2] = buffer[src + 2];
                    }
                }
            }
        }
        return std::make_shared<Image>(50, 50, FMT_RGB888, up.data(), up.size(), true);
    }

    return std::make_shared<Image>(wh, wh, FMT_RGB888, buffer.data(), buffer.size(), true);
}