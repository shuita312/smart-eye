#include "decision_thread.hpp"

#include "maix_basic.hpp"
#include "dbg.hpp"
#include "label_speech_utils.hpp"

using namespace maix;

// 获取当前毫秒时间戳
static uint64_t now_ms()
{
    return static_cast<uint64_t>(time::ticks_ms());
}



// 决策线程
// 职责：根据前端融合结果（JudgeFrame）判断是否触发告警事件，
//       并将告警写入 alert_queue，供 TTS 线程进行语音播报。
void decision_thread_func(AppContext &ctx)
{
    uint64_t last_alert_ms = 0;

    // 读取当前用户配置（需要加锁，防止与其他线程并发读写冲突）
    UserConfig cfg;
    {
        std::lock_guard<std::mutex> lock(ctx.config_mutex);
        cfg = ctx.config;
    }

    // 主循环：只要系统处于运行状态，就不断从 judge_queue 中取融合结果
    while (ctx.running.load())
    {
        JudgeFrame judge;

        // 等待融合结果；当 ctx.running 变为 false 或队列异常时返回 false
        if (!ctx.judge_queue.wait_pop(judge, ctx.running))
        {
            println("decision thread: wait_pop judge frame failed, exiting");
            break;
        }

        // 初始化告警事件，默认不触发告警（NONE）
        AlertEvent evt;
        evt.ts_ms = judge.ts_ms;
        evt.level = AlertLevel::NONE;

        // TODO: 当前为最简单规则，可后续拓展为更复杂的决策逻辑
        // 规则：若检测结果非空，则触发中等级别提醒（MEDIUM）
        if (!judge.objects.empty())
        {
            // 冷却时间：避免在短时间内连续触发多次相同告警，造成频繁播报
            if (judge.ts_ms > last_alert_ms + static_cast<uint64_t>(cfg.cooldown_ms))
            {
                println("Decision thread: detected %zu objects, time = %lu ms ,now_time = %lu ms ,delay = %lu ms", judge.objects.size(), judge.ts_ms, now_ms(), now_ms() - judge.ts_ms);

                // 在白名单中选择一个用于播报的目标，优先级优先，其次距离最近
                const ObjectInfo *best_obj = nullptr;
                int best_priority = 0;

                for (const auto &obj : judge.objects)
                {
                    if (!isInWhitelist(obj.label))
                    {
                        continue;
                    }

                    int pri = getClassPriority(obj.label);
                    if (!best_obj || pri < best_priority || (pri == best_priority && obj.distance_mm < best_obj->distance_mm))
                    {
                        best_obj = &obj;
                        best_priority = pri;
                    }
                }

                // 如果没有白名单内的目标，则不播报
                if (!best_obj)
                {
                    time::sleep_ms(5);
                    continue;
                }

                // 方位字符串，供 generateSpeech 使用
                std::string dir_key;
                switch (best_obj->position)
                {
                case ObjectPosition::LEFT:
                    dir_key = "left";
                    break;
                case ObjectPosition::RIGHT:
                    dir_key = "right";
                    break;
                case ObjectPosition::CENTER:
                default:
                    dir_key = "center";
                    break;
                }

                // 距离转换为米
                float dist_m = static_cast<float>(best_obj->distance_mm) / 1000.0f;

                // 生成播报文本
                evt.level = AlertLevel::MEDIUM;
                evt.text = generateSpeech(best_obj->label, dist_m, dir_key);

                last_alert_ms = judge.ts_ms;
                // 将告警事件推入 alert_queue，由 TTS 线程取出并播报
                ctx.alert_queue.push(std::move(evt));
            }
        }

        // 简单释放调度，让出 CPU，避免本线程占用过多资源，
        // 影响其他线程（如 TTS、音频播放）及时响应
        time::sleep_ms(5);
    }
}