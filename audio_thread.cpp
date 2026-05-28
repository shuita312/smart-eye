#include "audio_thread.hpp"
#include "maix_basic.hpp"
#include "maix_audio.hpp"
#include "dbg.hpp"

using namespace maix;
using namespace maix::audio;

// audio_thread 线程职能：
// 1. 从 audio_queue 阻塞式获取音频 PCM 数据块
// 2. 使用 maix::audio::Player 进行播放
// 3. 循环执行直至 ctx.running 被置为 false
void audio_thread_func(AppContext &ctx){

    // 读取当前用户配置（上锁防止与其他线程并发读写冲突）
    UserConfig cfg;
    {
        std::lock_guard<std::mutex> lock(ctx.config_mutex);
        cfg = ctx.config;
    }

    // 初始化音频播放器
    // Player(输出设备名称，采样率，采样格式，声道数，是否异步)
    // 这里："" 表示默认输出设备，44100Hz 采样率，16bit 小端单声道，异步播放
#if use_eSpeak_NG
    Player player("", 22050, Format::FMT_S16_LE, 1, true);
#else
    Player player("", 44100, Format::FMT_S16_LE, 1, true);
#endif
    // 设置音量为 80%（最大音量100）
    player.volume(80);
    // reset(true) 通常用于启动或重置播放状态，这里用于开始播放
    player.reset(true);   // 开始播放
    
    println("Audio thread: initialized ..." );

    // 主循环：只要 ctx.running 为 true，就持续从队列取音频数据并播放
    while (ctx.running.load()) {

        Bytes *pcm = nullptr;
        // 从音频队列中取出一块 PCM 数据；
        // 当 ctx.running 变为 false 或发生错误时，wait_pop 返回 false
        if (!ctx.audio_queue.wait_pop(pcm, ctx.running))
        {
            println("Audio thread: wait_pop failed, exiting");
            break;
        }

        if (!pcm)
            continue;

        // 播放当前 PCM 缓冲区
        auto ret = player.play(pcm);
        if (ret != err::ERR_NONE)
        {
            println("Audio play failed");
        }

        // 播放完成后立刻释放缓冲区，避免内存泄漏
        delete pcm;

        // 简单的调度延时，防止线程过于频繁抢占 CPU
        time::sleep_ms(5);
    }

}