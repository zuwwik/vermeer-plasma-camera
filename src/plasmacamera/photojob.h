// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDateTime>
#include <QImage>
#include <QString>
#include <libcamera/still_image.h>

struct PhotoOptions {
    int id = 0;
    QString path;
    int rotation = 0;
    bool mirror = false;
    QDateTime captured;
};

struct PhotoRawFrame {
    std::vector<uint8_t> data;
    libcamera::StillImageParameters parameters;
};

struct PhotoResult {
    PhotoOptions options;
    QString error;
};

PhotoResult processPhoto(PhotoOptions options, std::vector<PhotoRawFrame> frames, QImage fallback);
