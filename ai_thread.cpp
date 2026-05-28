#include "ai_thread.hpp"

#include "maix_basic.hpp"
#include "maix_nn.hpp" // 真正接 YOLO11n 时再启用
#include "maix_nn_yolo11.hpp"

#include "dbg.hpp"
using namespace maix;

// AI 推理线程
// 从 ai_input_queue 获取 RGB 图像
// 调用yolo11模型进行推理，得到检测结果
// 推理结果打包成 DetectionResult，推送到 fusion_queue
void ai_inference_thread_func(AppContext &ctx)
{
    // 加载 yolo11n 模型  板子上的路径
    nn::YOLO11 detector("/root/models/yolo11n.mud", true);

    // 设置输入图像格式配置参数
    {
        std::lock_guard<std::mutex> lock(ctx.latest_mutex);
        ctx.config.ai_input_fmt = detector.input_format();
        println("AI thread: model input format: %d", (int)ctx.config.ai_input_fmt);
    }

    // 主循环
    while (ctx.running.load())
    {
        RgbFrame rgb;

        // 等待 RGB 输入
        if (!ctx.ai_input_queue.wait_pop(rgb, ctx.running))
        {
            break;
        }
        // 如果当前还没有可用的图，跳过本轮
        if (!rgb.ai_image)
        {
            time::sleep_ms(5);
            continue;
        }
        // // 打印输入图像信息
        // println("AI thread: got input image, width=%d, height=%d, format=%d", rgb.ai_image->width(), rgb.ai_image->height(), (int)rgb.ai_image->format());

        // 调用yolo11模型进行推理，得到检测结果
        DetectionResult det;
        det.ts_ms = rgb.ts_ms;
        det.valid = false;

        try
        {

            // detect 函数的参数：输入图像，置信度阈值，NMS阈值
            nn::Objects *objs = detector.detect(*rgb.ai_image, 0.5, 0.45);

            // 填写检测结果
            for (auto obj : *objs)
            {
                DetectionBox one;

                one.class_id = obj->class_id;
                one.score = obj->score;
                one.label = detector.labels[obj->class_id];

                one.x = obj->x;
                one.y = obj->y;
                one.w = obj->w;
                one.h = obj->h;

                det.boxes.push_back(std::move(one));
            }

            delete objs; // 必须释放
            // 如果没有检测到任何物体，可以根据业务需求设置 det.valid = false，或者保持为 true 但 boxes 为空
            if (det.boxes.empty())
            {
                det.valid = false;
                //println("AI thread: no objects detected");
            }
            else
            {
                det.valid = true;
                // println("AI thread: detected %zu objects", det.boxes.size());
            }
        }
        catch (...)
        {
            eprintln("AI detect failed");
            // 显示输入图片格式
            println("input image format: width=%d, height=%d, format=%d", rgb.ai_image->width(), rgb.ai_image->height(), (int)rgb.ai_image->format());
            time::sleep_ms(10);
            continue;
        }

        // 推送到数据中转线程
        ctx.det_queue.push(det);

        // 更新最新检测结果缓存
        {
            std::lock_guard<std::mutex> lock(ctx.latest_mutex);
            ctx.latest_det = std::move(det);
        }
        // 控制推理频率
        time::sleep_ms(5);
    }
}