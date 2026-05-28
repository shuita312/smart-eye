#pragma once

#include <unordered_map>
#include <set>
#include <string>

// COCO 80 类别英文标签到中文的映射
static const std::unordered_map<std::string, std::string> coco_en_to_zh = {
    {"person", "行人"},
    {"bicycle", "自行车"},
    {"car", "汽车"},
    {"motorcycle", "摩托车"},
    {"airplane", "飞机"},
    {"bus", "公交车"},
    {"train", "火车"},
    {"truck", "卡车"},
    {"boat", "船"},
    {"traffic light", "交通灯"},
    {"fire hydrant", "消防栓"},
    {"stop sign", "停车标志"},
    {"parking meter", "停车计时器"},
    {"bench", "长椅"},
    {"bird", "鸟"},
    {"cat", "猫"},
    {"dog", "狗"},
    {"horse", "马"},
    {"sheep", "羊"},
    {"cow", "牛"},
    {"elephant", "大象"},
    {"bear", "熊"},
    {"zebra", "斑马"},
    {"giraffe", "长颈鹿"},
    {"backpack", "背包"},
    {"umbrella", "雨伞"},
    {"handbag", "手提包"},
    {"tie", "领带"},
    {"suitcase", "行李箱"},
    {"frisbee", "飞盘"},
    {"skis", "滑雪板"},
    {"snowboard", "滑雪板"},
    {"sports ball", "运动球"},
    {"kite", "风筝"},
    {"baseball bat", "棒球棒"},
    {"baseball glove", "棒球手套"},
    {"skateboard", "滑板"},
    {"surfboard", "冲浪板"},
    {"tennis racket", "网球拍"},
    {"bottle", "瓶子"},
    {"wine glass", "酒杯"},
    {"cup", "杯子"},
    {"fork", "叉子"},
    {"knife", "刀"},
    {"spoon", "勺子"},
    {"bowl", "碗"},
    {"banana", "香蕉"},
    {"apple", "苹果"},
    {"sandwich", "三明治"},
    {"orange", "橙子"},
    {"broccoli", "西兰花"},
    {"carrot", "胡萝卜"},
    {"hot dog", "热狗"},
    {"pizza", "披萨"},
    {"donut", "甜甜圈"},
    {"cake", "蛋糕"},
    {"chair", "椅子"},
    {"couch", "沙发"},
    {"potted plant", "盆栽植物"},
    {"bed", "床"},
    {"dining table", "餐桌"},
    {"toilet", "厕所"},
    {"tv", "电视"},
    {"laptop", "笔记本电脑"},
    {"mouse", "鼠标"},
    {"remote", "遥控器"},
    {"keyboard", "键盘"},
    {"cell phone", "手机"},
    {"microwave", "微波炉"},
    {"oven", "烤箱"},
    {"toaster", "烤面包机"},
    {"sink", "水槽"},
    {"refrigerator", "冰箱"},
    {"book", "书"},
    {"clock", "钟"},
    {"vase", "花瓶"},
    {"scissors", "剪刀"},
    {"teddy bear", "泰迪熊"},
    {"hair drier", "吹风机"},
    {"toothbrush", "牙刷"}
};

// SmartEye Phase 1 白名单（需要播报的类别）
static const std::set<std::string> smarteye_whitelist = {
    "person",      // 行人
    "bicycle",     // 自行车
    "car",         // 汽车
    "motorcycle",  // 摩托车
    "bus",         // 公交车
    "truck",       // 卡车
    "chair",       // 椅子
    "dining table",// 餐桌
    "tv",          // 电视
    "laptop",      // 笔记本电脑
    "cell phone"   // 手机
};

// 获取类别的中文名称
inline std::string getChineseLabel(const std::string &en_label, const std::string &default_name = "物体")
{
    auto it = coco_en_to_zh.find(en_label);
    if (it != coco_en_to_zh.end())
    {
        return it->second;
    }
    return default_name;
}

// 检查是否在播报白名单内
inline bool isInWhitelist(const std::string &en_label)
{
    return smarteye_whitelist.find(en_label) != smarteye_whitelist.end();
}

// 获取类别优先级（用于播报优先级判断）
inline int getClassPriority(const std::string &en_label)
{
    // 返回数字越小优先级越高
    static const std::unordered_map<std::string, int> priority_map = {
        {"person", 1},     // P2 - 重要
        {"bicycle", 1},
        {"car", 1},
        {"motorcycle", 1},
        {"bus", 1},
        {"truck", 1},
        {"chair", 2},      // P3 - 环境信息
        {"dining table", 2},
        {"tv", 2},
        {"laptop", 2},
        {"cell phone", 2}
    };

    auto it = priority_map.find(en_label);
    if (it != priority_map.end())
    {
        return it->second;
    }
    return 3; // P4 - 按需查询
}

// 生成播报文本
inline std::string generateSpeech(const std::string &en_label, float distance_m, const std::string &direction)
{
    std::string label = getChineseLabel(en_label);
    std::string dir_text = (direction == "left") ? "左前方" :
                           (direction == "right") ? "右前方" : "正前方";

    if (distance_m < 0.5f)
    {
        return dir_text + "非常近有" + label + "，请注意安全！";
    }
    else if (distance_m < 1.0f)
    {
        return dir_text + std::to_string((int)(distance_m * 10) / 10.0f) + "米处有" + label + "，请小心";
    }
    else
    {
        return dir_text + std::to_string((int)(distance_m * 10) / 10.0f) + "米处有" + label;
    }
}
