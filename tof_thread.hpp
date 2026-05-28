#pragma once

#include "app_context.hpp"

// ToF 采集线程入口函数
void tof_capture_thread_func(AppContext &ctx);

// 将 ToF 深度矩阵转换成伪彩色图像
std::shared_ptr<maix::image::Image> tof_matrix_to_image(
    const maix::ext_dev::tof100::TOFMatrix &matrix,
    int wh,
    maix::ext_dev::cmap::Cmap cmap_type,
    int min_mm,
    int max_mm);