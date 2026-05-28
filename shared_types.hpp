#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <chrono>

#include "maix_image.hpp"
#include "maix_tof100.hpp"
#include "maix_audio.hpp"

#define use_eSpeak_NG 1

using namespace maix;
using namespace maix::ext_dev::tof100;
// ToF 单帧数据
// matrix: 深度矩阵，通常是 100x100
// width/height: 当前深度图尺寸
// ts_ms: 时间戳，后续做多传感器同步时要用
struct TofFrame
{
    TOFMatrix matrix;
    Resolution res;
    uint64_t ts_ms = 0;
};

// RGB 相机单帧数据
// image: 图像对象
// width/height: 图像尺寸
// ts_ms: 时间戳
// RGB线程输出的数据
// raw_image: 主相机原始图
// ai_image: 给AI推理用，固定 480x480
// fusion_image: 给融合用，固定到ToF边长，如 100x100
struct RgbFrame
{
    std::shared_ptr<maix::image::Image> raw_image;
    std::shared_ptr<maix::image::Image> ai_image;
    int raw_width = 0;
    int raw_height = 0;

    uint64_t ts_ms = 0;
};

// 单个检测框
struct DetectionBox
{
    int x = 0; // 检测框左上角坐标     横
    int y = 0; // 检测框左上角坐标     纵
    int w = 0; // 检测框宽度           x
    int h = 0; // 检测框高度           y
    float score = 0.0f;
    int class_id = -1;
    std::string label;
};

// 一帧检测结果
struct DetectionResult
{
    std::vector<DetectionBox> boxes;
    uint64_t ts_ms = 0;
    bool valid = false; // 是否有效，推理失败时为 false
};

// 融合结果帧
// 包含一帧 ToF 数据
// det: 当前帧关联到的检测结果
// ts_ms: 时间戳
struct FusionFrame
{
    TofFrame tof; // 关联的 ToF 数据
    DetectionResult det;
    uint64_t ts_ms = 0;
};

// 单个物体的位置关系/距离信息
enum struct ObjectPosition
{
    LEFT = 0,
    CENTER,
    RIGHT
};

// 单个物体
struct ObjectInfo
{
    int class_id = -1;
    std::string label;
    float score = 0.0f;
    uint32_t distance_mm = 0;                         // 距离，单位 mm
    ObjectPosition position = ObjectPosition::CENTER; // 位于画面左中右哪个位置
};

// fus -> 决策线程的输入
struct JudgeFrame
{
    std::vector<ObjectInfo> objects; // 检测到的物体列表
    uint64_t ts_ms = 0;
};

// 告警等级
enum class AlertLevel
{
    NONE = 0,
    LOW,
    MEDIUM,
    HIGH
};

// 告警事件
// 由决策线程生成，再交给播报线程处理
struct AlertEvent
{
    AlertLevel level = AlertLevel::NONE;
    std::string text;
    uint64_t ts_ms = 0;
};

// 用户配置参数
// 后续可以从 BLE / JSON / 本地文件中读取
struct UserConfig
{
    int tof_min_mm = 40;      // ToF 最小有效距离
    int tof_max_mm = 2000;    // ToF 最大有效距离
    bool fusion_mode = false; // 是否开启融合显示
    bool enable_tts = true;   // 是否开启语音播报

    int rgb_width = 640;  // RGB 分辨率宽
    int rgb_height = 640; // RGB 分辨率高

    int rgb_dx = 0; // RGB 图像的平移偏移量, 向右为正，向左为负
    int rgb_dy = 0; // RGB 图像的垂直偏移量, 向下为正，向上为负

    int ai_input_w = 640; // 给AI推理的图像尺寸
    int ai_input_h = 480; // 给AI推理的图像尺寸

    maix::image::Format ai_input_fmt = maix::image::FMT_RGB888;              // AI推理的输入图像格式
    maix::ext_dev::cmap::Cmap tof_cmap = maix::ext_dev::cmap::Cmap::RED_HOT; // ToF 伪彩色方案

    int tof_res2len = 50;                       // ToF 分辨率宽转长度
    Resolution tof_res = Resolution::RES_50x50; // ToF 分辨率宽

    int cooldown_ms = 2000; // 告警冷却时间
};


#pragma pack(push, 1)
struct WavHeader
{
    char riff[4];              // "RIFF"
    uint32_t file_size;        // 文件大小 - 8
    char wave[4];              // "WAVE"

    char fmt_tag[4];           // "fmt "
    uint32_t fmt_size;         // fmt chunk size
    uint16_t audio_format;     // PCM = 1
    uint16_t num_channels;     // 声道数
    uint32_t sample_rate;      // 采样率
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;  // 位深

    char data_tag[4];          // "data"
    uint32_t data_size;        // PCM 数据长度
};
#pragma pack(pop)