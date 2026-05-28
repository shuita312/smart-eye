#include <thread>
#include <vector>

#include "maix_basic.hpp"
#include "maix_display.hpp"
#include "maix_image.hpp"
#include "maix_comm.hpp"
#include "maix_nn.hpp"
#include "maix_nn_melotts.hpp"
#include "dbg.hpp"

#include "app_context.hpp"
#include "tof_thread.hpp"
#include "rgb_thread.hpp"
#include "ai_thread.hpp"
#include "fusion_thread.hpp"
#include "decision_thread.hpp"
#include "tts_thread.hpp"
#include "config_thread.hpp"
#include "data_relay.hpp"
#include "audio_thread.hpp"

#include <iostream>
#include <sys/resource.h>



using namespace maix;
using namespace maix::image;



// 获取当前毫秒时间戳
static uint64_t now_ms()
{
    return static_cast<uint64_t>(time::ticks_ms());
}

// 主业务入口
static int app_main()
{

    
    AppContext ctx;
    // 初始化默认配置
    {
        std::lock_guard<std::mutex> lock(ctx.config_mutex);
        ctx.config.tof_min_mm = 40;     // ToF 最小有效距离 4cm
        ctx.config.tof_max_mm = 1000;   // ToF 最大有效距离 2m
        ctx.config.fusion_mode = false; // 开启融合显示
        ctx.config.enable_tts = true;   // 开启语音播报

        ctx.config.rgb_width = 640;  // RGB 分辨率宽
        ctx.config.rgb_height = 480; // RGB 分辨率高

        ctx.config.ai_input_w = 640; // 给AI推理的图像尺寸宽
        ctx.config.ai_input_h = 480; // 给AI推理的图像尺寸高

        ctx.config.tof_res2len = 50;                // ToF 分辨率宽转长度
        ctx.config.tof_res = Resolution::RES_50x50; // ToF 分辨率 50x50

        ctx.config.rgb_dx = 72; // ToF 图像的水平偏移，向右为正，向左为负
        ctx.config.rgb_dy = 25; // ToF 图像的垂直偏移，向下为正，向上为负

        ctx.config.tof_cmap = maix::ext_dev::cmap::Cmap::RED_HOT; // ToF 伪彩色方案
        ctx.config.cooldown_ms = 2000;                            // 告警冷却时间 2s
        
        // 线程安全标志位
        ctx.rgb_ready = false;
        ctx.tof_ready = false;
    }

    // 声明线程对象
    std::thread tof_thread;        // ToF 采集线程
    std::thread rgb_thread;        //  RGB 采集线程
    std::thread ai_thread;         // AI 推理线程
    std::thread fusion_thread;     // 融合线程
    std::thread data_relay_thread; // 数据中继线程
    std::thread decision_thread;   // 决策线程
    std::thread tts_thread;        // 文字转语音线程
    std::thread cfg_thread;        // 配置管理线程
    std::thread audio_thread;      // 音频播放线程

#if use_eSpeak_NG
    println("Using eSpeak NG initialization");
#else
    // isp 和 tts 使用npu冲突
    maix::app::set_sys_config_kv("npu", "ai_isp", "0");
    println("Using MeloTTS initialization");
#endif

    try
    {
        // // 先初始化显示设备
        // display::Display disp = display::Display(-1, -1, image::FMT_RGB888);
        // err::check_bool_raise(disp.is_opened(), "display open failed");

        // 启动线程
        tof_thread = std::thread(tof_capture_thread_func, std::ref(ctx));
        rgb_thread = std::thread(rgb_capture_thread_func, std::ref(ctx));
        ai_thread = std::thread(ai_inference_thread_func, std::ref(ctx));
        data_relay_thread = std::thread(data_relay_thread_func, std::ref(ctx));
        fusion_thread = std::thread(fusion_thread_func, std::ref(ctx));
        decision_thread = std::thread(decision_thread_func, std::ref(ctx));
        tts_thread = std::thread(tts_thread_func, std::ref(ctx));
        audio_thread = std::thread(audio_thread_func, std::ref(ctx));
        // cfg_thread = std::thread(config_thread_func, std::ref(ctx));

        // 主线程负责显示和监控退出信号
        // obj 左中右 位置映射
        const char *position_str[] = {"LEFT", "CENTER", "RIGHT"};

        // 主循环
        while (!app::need_exit() && ctx.running.load())
        {

            JudgeFrame judge;

            // if(ctx.judge_queue.wait_pop(judge, ctx.running)) {
            //     for(auto &obj : judge.objects) {
            //         int pos_idx = static_cast<int>(obj.position);
            //         const char *pos_name =(pos_idx >= 0 && pos_idx < 3) ? position_str[pos_idx] : "UNKNOWN";

            //         println("JudgeFrame: object %s, score=%.2f, distance=%d mm, position=%s",
            //                 obj.label.c_str(), obj.score, obj.distance_mm, pos_name);

            //         println("JudgeFrame: object %s,ts_ms=%lu, now_ms=%lu", obj.label.c_str(),judge.ts_ms, now_ms());
            //     }
            // }

            // FusionFrame fusion;

            // if (ctx.fusion_queue.wait_pop(fusion, ctx.running)) {
            //     if (fusion.fused_image) {
            //         println("MAIN: got fusion frame, ts_ms=%lu ,now_ms=%lu",
            //                 fusion.ts_ms, now_ms());

            //         std::unique_ptr<Image> show_img(
            //             fusion.fused_image->resize(disp.width(), disp.height())
            //         );
            //         disp.show(*show_img);
            //     }
            // }

            // if(true){
            // TofFrame tof;
            //     //尝试获取一帧toF结果
            //     if (ctx.tof_queue.try_pop(tof)) {
            //         int wh = static_cast<int>(ctx.config.tof_res);
            //         auto depth_img = tof_matrix_to_image(
            //             tof.matrix,
            //             wh,
            //             ctx.config.tof_cmap,
            //             ctx.config.tof_min_mm,
            //             ctx.config.tof_max_mm
            //         );
            //         disp.show(*depth_img);  // 直接显示在屏幕

            //     }
            // }

            // if (true) {
            //     //测试RBG的数据帧
            //     RgbFrame rgb;
            //     if(ctx.rgb_queue.try_pop(rgb)) {

            //         if (rgb.fusion_image) {
            //             // 缩放到屏幕尺寸后显示
            //             std::unique_ptr<Image> show_img(rgb.fusion_image->resize(disp.width(), disp.height()));
            //             disp.show(*show_img);
            //         }
            //     }

            // }

            time::sleep_ms(5);

            // 定时一分钟后自动退出，避免长时间运行后可能出现的内存泄漏等问题
            static uint64_t start_time = now_ms();
            if (now_ms() - start_time > 60000)
            {
                println("Auto exit after 1 minute");
                break;
            }
        }
    }
    catch (...)
    {

        // 异常线程退出
        ctx.running = false;

        // 唤醒所有可能阻塞在 wait_pop 的线程
        ctx.tof_queue.notify_all();
        ctx.rgb_queue.notify_all();
        ctx.ai_input_queue.notify_all();
        ctx.judge_queue.notify_all();
        ctx.det_queue.notify_all();
        ctx.fusion_queue.notify_all();
        ctx.alert_queue.notify_all();
        ctx.audio_queue.notify_all();

        // 等待所有线程结束
        if (tof_thread.joinable())
            tof_thread.join();
        if (rgb_thread.joinable())
            rgb_thread.join();
        if (ai_thread.joinable())
            ai_thread.join();
        if (data_relay_thread.joinable())
            data_relay_thread.join();
        if (fusion_thread.joinable())
            fusion_thread.join();
        if (decision_thread.joinable())
            decision_thread.join();
        if (tts_thread.joinable())
            tts_thread.join();
        if (audio_thread.joinable())
            audio_thread.join();
        // if (cfg_thread.joinable()) cfg_thread.join();

        throw; // 继续交给外层 CATCH_EXCEPTION_RUN_RETURN
    }

    // 正常线程退出
    ctx.running = false;

    // 唤醒所有可能阻塞在 wait_pop 的线程
    ctx.tof_queue.notify_all();
    ctx.rgb_queue.notify_all();
    ctx.ai_input_queue.notify_all();
    ctx.det_queue.notify_all();
    ctx.fusion_queue.notify_all();
    ctx.alert_queue.notify_all();
    ctx.judge_queue.notify_all();
    ctx.audio_queue.notify_all();

    // 等待所有线程结束
    if (tof_thread.joinable())
        tof_thread.join();
    if (rgb_thread.joinable())
        rgb_thread.join();
    if (ai_thread.joinable())
        ai_thread.join();
    if (data_relay_thread.joinable())
        data_relay_thread.join();
    if (fusion_thread.joinable())
        fusion_thread.join();
    if (decision_thread.joinable())
        decision_thread.join();
    if (tts_thread.joinable())
        tts_thread.join();
    if (audio_thread.joinable())
        audio_thread.join();
    // if (cfg_thread.joinable()) cfg_thread.join();

    return 0;
}

static int test_main()
{
    sys::register_default_signal_handle();

    display::Display disp(-1, -1, image::FMT_RGB888);
    err::check_bool_raise(disp.is_opened(), "display open failed");

    Image img(disp.width(), disp.height(), FMT_RGB888);
    img.clear();
    img.draw_string(20, 20, "display ok");
    disp.show(img);

    while (1)
    {
        time::sleep_ms(1000);
    }
    return 0;
}



static int test_tts_main()
{
    maix::app::set_sys_config_kv("npu", "ai_isp", "0");
    println("before MeloTTS");
    nn::MeloTTS tts("/root/models/melotts/melotts-zh.mud", "zh", 0.8f, 0.3f, 0.6f, 0.2f);
    println("after MeloTTS");
    return 0;
}


// 程序入口
int main(int argc, char *argv[])
{
    sys::register_default_signal_handle();
    comm::add_default_comm_listener();
    CATCH_EXCEPTION_RUN_RETURN(app_main, -1);
    // CATCH_EXCEPTION_RUN_RETURN(test_main, -1);
    //CATCH_EXCEPTION_RUN_RETURN(test_tts_main, -1);
}

// 关于显示资源 VO释放的问题：
// VO（Video Output）资源的释放

// 检查进程 ps -ef | grep -E 'maix|blind_tof_camera'
// 杀死  launcher/launcher_daemon
