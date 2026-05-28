#include "config_thread.hpp"

#include "maix_basic.hpp"
// #include "maix_ble.hpp"
// #include "json.hpp"

using namespace maix;

// 配置管理线程
// 负责 BLE 接收配置、JSON 解析、更新系统参数
void config_thread_func(AppContext &ctx)
{
    while (ctx.running.load()) {
        // TODO:
        // 1. 监听 BLE 消息
        // 2. 解析 JSON 配置
        // 3. 更新 ctx.config

        time::sleep_ms(200);
    }
}