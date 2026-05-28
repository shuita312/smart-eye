#include "rgb_thread.hpp"

#include "maix_basic.hpp"
#include "maix_camera.hpp"
#include "maix_image.hpp"

#include "app_context.hpp"
#include "dbg.hpp"

using namespace maix;

// 获取当前毫秒时间戳
static uint64_t now_ms()
{
    return static_cast<uint64_t>(time::ticks_ms());
}

// RGB采集线程
// 功能：
// 1. 从主相机读取原始图像
// 2. 根据配置参数裁剪出和ToF对齐的有效区域
// 3. 生成给AI使用的 640x480 图像
// 4. 生成给融合使用的 ToF尺寸图像（如50x50）
// 5. 将结果打包成 RgbFrame 推送给后续线程
void rgb_capture_thread_func(AppContext &ctx)
{

    // 先读取一次配置，用于初始化相机
    UserConfig cfg;
    {
        std::lock_guard<std::mutex> lock(ctx.config_mutex);
        cfg = ctx.config;
    }
    // 创建相机对象
    std::unique_ptr<camera::Camera> cam;

    int read_fail_count = 0;
    uint64_t last_ok_ms = 0;

    // 主循环
    while (ctx.running.load())
    {

        // 初始化相机
        if (!cam)
        {
            try
            {
                cam = std::make_unique<camera::Camera>(cfg.rgb_width, cfg.rgb_height);
                // 初始化成功，重置失败计数和时间戳
                read_fail_count = 0;
                last_ok_ms = now_ms();
                println("RGB thread: init OK, %dx%d", cfg.rgb_width, cfg.rgb_height);
            }
            catch (...)
            {
                eprintln("RGB thread: init failed");
                time::sleep_ms(100);
                continue;
            }
        }

        // 读取一帧图像
        std::unique_ptr<image::Image> raw;
        try
        {
            raw.reset(cam->read());
        }
        catch (...)
        {
            eprintln("RGB thread: cam->read() exception, reopen");
            cam.reset();
            time::sleep_ms(100);
            continue;
        }

        // 如果读取失败，可能是设备问题，重试几次，如果持续失败则重启设备
        if (!raw)
        {
            read_fail_count++;
            uint64_t now = now_ms();

            if (read_fail_count >= 5 || (last_ok_ms > 0 && now - last_ok_ms > 500))
            {
                eprintln("RGB thread: read failed too long, reopen camera");
                cam.reset();
                read_fail_count = 0;
                time::sleep_ms(100);
            }
            else
            {
                time::sleep_ms(10);
            }
            continue;
        }

        // 成功读取到一帧，更新成功时间戳和失败计数
        read_fail_count = 0;
        last_ok_ms = now_ms();

        // 转成 shared_ptr，便于多线程共享
        std::shared_ptr<image::Image> raw_img(raw.release());
        if (!raw_img)
        {
            println("RGB thread: failed to create shared image");
            time::sleep_ms(5);
            continue;
        }

        // 生成给AI的图：固定 640x480
        std::shared_ptr<image::Image> ai_img;
        try
        {
            std::unique_ptr<image::Image> ai_tmp(
                raw_img->resize(cfg.ai_input_w, cfg.ai_input_h));
            if (!ai_tmp)
            {
                eprintln("RGB thread: resize ai image failed");
                time::sleep_ms(5);
                continue;
            }
            ai_img.reset(ai_tmp.release());
        }
        catch (...)
        {
            eprintln("RGB thread: resize exception");
            time::sleep_ms(10);
            continue;
        }

        // 打包成一帧RGB数据
        RgbFrame frame;
        frame.raw_image = raw_img; // 原始主相机图
        frame.ai_image = ai_img;   // 给AI推理用
        frame.raw_width = raw_img->width();
        frame.raw_height = raw_img->height();
        frame.ts_ms = last_ok_ms; // 时间戳：成功读取到图像的时间，单位毫秒

        // 推送到队列
        // ctx.rgb_queue.push(frame);       // 给其他模块
        // ctx.ai_input_queue.push(frame);  // 给AI线程

        // 更新最新RGB帧缓存
        {
            std::lock_guard<std::mutex> lock(ctx.latest_mutex);
            ctx.latest_rgb = std::move(frame);
            ctx.rgb_ready = true;
        }

        // 给调度留一点余量
        time::sleep_ms(5);
    }
}