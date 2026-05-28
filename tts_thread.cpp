#include "tts_thread.hpp"

#include "maix_basic.hpp"
#include "dbg.hpp"
#include "maix_nn.hpp"
#include "maix_nn_melotts.hpp"

// #include "maix_audio.hpp"
// #include "maix_tts.hpp"

using namespace maix;
using namespace maix::nn;



// 获取当前毫秒时间戳
static uint64_t now_ms()
{
    return static_cast<uint64_t>(time::ticks_ms());
}
// 将字符串中的单引号转义，适用于 shell 命令参数
static std::string shell_escape_single_quotes(const std::string &s)
{
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s)
    {
        if (c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    return out;
}

// 读取 wav 文件，提取裸 PCM 数据到 Bytes
static Bytes *read_wav_pcm(const std::string &wav_path)
{
    std::ifstream ifs(wav_path, std::ios::binary);
    if (!ifs.is_open())
    {
        println("TTS: failed to open wav file: %s", wav_path.c_str());
        return nullptr;
    }

    WavHeader hdr{};
    ifs.read(reinterpret_cast<char *>(&hdr), sizeof(hdr));
    if (!ifs)
    {
        println("TTS: failed to read wav header");
        return nullptr;
    }

    if (memcmp(hdr.riff, "RIFF", 4) != 0 || memcmp(hdr.wave, "WAVE", 4) != 0)
    {
        println("TTS: invalid wav header");
        return nullptr;
    }

    if (memcmp(hdr.fmt_tag, "fmt ", 4) != 0)
    {
        println("TTS: invalid wav fmt chunk");
        return nullptr;
    }

    if (memcmp(hdr.data_tag, "data", 4) != 0)
    {
        println("TTS: unsupported wav layout, data chunk not in expected position");
        return nullptr;
    }

    if (hdr.audio_format != 1)
    {
        println("TTS: unsupported wav format: %u", hdr.audio_format);
        return nullptr;
    }

    if (hdr.num_channels != 1)
    {
        println("TTS: unsupported channels: %u", hdr.num_channels);
        return nullptr;
    }

    if (hdr.bits_per_sample != 16)
    {
        println("TTS: unsupported bits_per_sample: %u", hdr.bits_per_sample);
        return nullptr;
    }

    if (hdr.sample_rate != 22050)
    {
        println("TTS: warning, wav sample_rate=%u, audio thread should use same rate", hdr.sample_rate);
    }

    Bytes *pcm = new Bytes(nullptr, hdr.data_size);
    if (!pcm || !pcm->data)
    {
        println("TTS: alloc pcm buffer failed");
        delete pcm;
        return nullptr;
    }

    ifs.read(reinterpret_cast<char *>(pcm->data), hdr.data_size);
    if (!ifs)
    {
        println("TTS: failed to read wav pcm data");
        delete pcm;
        return nullptr;
    }

    pcm->data_len = hdr.data_size;
    return pcm;
}


// 调用 espeak-ng 生成 wav，再读取 PCM
static Bytes *espeak_tts_to_pcm(const std::string &text)
{
    uint64_t ts = now_ms();
    std::string wav_path = "/tmp/tts_" + std::to_string(ts) + ".wav";

    // -v zh      中文
    // -s 150     语速，可调
    std::string cmd =
        "espeak-ng -v zh -s 150 -w " + wav_path + " '" +
        shell_escape_single_quotes(text) + "'";

    int ret = std::system(cmd.c_str());
    if (ret != 0)
    {
        println("TTS: espeak-ng command failed, ret=%d", ret);
        std::remove(wav_path.c_str());
        return nullptr;
    }

    Bytes *pcm = read_wav_pcm(wav_path);

    std::remove(wav_path.c_str());
    return pcm;
}


// 语音播报线程职责：
// 1. 从 alert_queue 中阻塞式获取告警事件
// 2. 根据事件内容调用 TTS 模块合成语音
// 3. 将生成的 PCM 音频数据推送到 audio_queue，由音频线程播放

void tts_thread_func(AppContext &ctx)
{
    // 读取当前用户配置（加锁防止多线程同时访问配置）
    UserConfig cfg;
    {
        std::lock_guard<std::mutex> lock(ctx.config_mutex);
        cfg = ctx.config;
    }

#if use_eSpeak_NG

    println("eSpeak_NG TTS thread: initialized ...");

    while (ctx.running.load())
    {
        AlertEvent evt;

        // 等待告警事件
        if (!ctx.alert_queue.wait_pop(evt, ctx.running))
        {
            println("TTS thread: wait_pop alert event failed, exiting");
            break;
        }


        if (!cfg.enable_tts || evt.level == AlertLevel::NONE || evt.text.empty())
        {
            continue;
        }

        uint64_t t0 = now_ms();
        Bytes *pcm = espeak_tts_to_pcm(evt.text);
        uint64_t t1 = now_ms();

        if (!pcm || pcm->data_len == 0)
        {
            println("TTS thread: espeak tts failed");
            if (pcm)
                delete pcm;
            continue;
        }

        // 推送给 audio 线程播放
        ctx.audio_queue.push(pcm);

        println("TTS thread: alert text=\"%s\", event_ts=%lu ms, tts_cost=%lu ms, total_delay=%lu ms, pcm_len=%u",
                evt.text.c_str(),
                evt.ts_ms,
                t1 - t0,
                t1 - evt.ts_ms,
                (unsigned int)pcm->data_len);

        // 队列接管生命周期
        pcm = nullptr;

        // 简单调度延时，防止线程空转
        time::sleep_ms(5);
    }

    while (ctx.running.load())
    {
        AlertEvent evt;

        if (!ctx.alert_queue.wait_pop(evt, ctx.running))
        {
            println("TTS thread: wait_pop alert event failed, exiting");
            break;
        }

        // 每次循环重新读取配置，避免启动后配置变化不生效
        UserConfig cfg;
        {
            std::lock_guard<std::mutex> lock(ctx.config_mutex);
            cfg = ctx.config;
        }

        if (!cfg.enable_tts || evt.level == AlertLevel::NONE || evt.text.empty())
        {
            continue;
        }

        uint64_t t0 = now_ms();
        Bytes *pcm = espeak_tts_to_pcm(evt.text);
        uint64_t t1 = now_ms();

        if (!pcm || pcm->data_len == 0)
        {
            println("eSpeak TTS failed");
            if (pcm)
                delete pcm;
            continue;
        }

        // 推给音频播放线程
        ctx.audio_queue.push(pcm);

        println("TTS thread: text=\"%s\", event_ts=%lu ms, infer_cost=%lu ms, total_delay=%lu ms",
                evt.text.c_str(),
                evt.ts_ms,
                t1 - t0,
                t1 - evt.ts_ms);

        pcm = nullptr;
        time::sleep_ms(5);
    }


#else


    // 创建 TTS 对象，加载模型，设置参数
    /*
     * @参数 speed：音频的语音语速由该值控制，数值越低，阅读速度越慢。默认值为 0.8
     * @参数 noise_scale：此参数控制语音的随机性。增大该值会导致语音输出更加多样化且不那么确定。默认值为 0.3
     * @参数 noise_scale_w：此参数控制语音对齐的随机性。较高的值可以增强自然度，但过高值可能会导致音频不稳定或失真。默认值为 0.6
     * @参数 sdp_ratio：对齐权重越高，语音听起来就越自然，但过高值可能会导致不稳定。默认值为 0.2
     */
    // 使用 MeloTTS 语音合成：
    //   - 模型文件路径：/root/models/melotts/melotts-zh.mud
    //   - 语言：zh（中文）
    //   - 其余为语速、随机性等参数（见上方说明）
    nn::MeloTTS tts("/root/models/melotts/melotts-zh.mud", "zh", 0.8f, 0.3f, 0.6f, 0.2f);

    while (ctx.running.load())
    {
        AlertEvent evt;

        // 等待告警事件；当 ctx.running 被置为 false 或队列出错时返回 false
        if (!ctx.alert_queue.wait_pop(evt, ctx.running))
        {
            println("TTS thread: wait_pop alert event failed, exiting");
            break;
        }

        if (!cfg.enable_tts || evt.level == AlertLevel::NONE || evt.text.empty())
        {
            // 如果：
            //   - TTS 功能被禁用
            //   - 告警等级为 NONE
            //   - 或告警文本为空
            // 则不进行语音播报
            continue;
        }

        // 调用 TTS 推理接口，将文本转为 PCM 音频数据
        Bytes *pcm = tts.infer(evt.text, "", true);

        if (!pcm || pcm->data_len == 0)
        {
            println("TTS failed");
            if (pcm)
                delete pcm;
            continue;
        }

        // // 将合成好的 PCM 音频数据推送到 audio_queue，交由音频线程播放
        // ctx.audio_queue.push(pcm);
        
        
        // 测试tts输入输出时间差
        println("TTS thread: alert text=\"%s\", time=%lu ms ,now_time=%lu ms ,delay=%lu ms", evt.text.c_str(), evt.ts_ms, now_ms(), now_ms() - evt.ts_ms);
        
        // 由于队列已接管 pcm 的生命周期，这里不再释放
        pcm = nullptr;

        // 简单调度延时，防止线程空转占用过多 CPU
        time::sleep_ms(5);
    }

#endif
}
