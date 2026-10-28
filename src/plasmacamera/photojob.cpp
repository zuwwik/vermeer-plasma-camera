// SPDX-License-Identifier: GPL-2.0-or-later
#include "photojob.h"

#include <QBuffer>
#include <QColorSpace>
#include <QImageWriter>
#include <QSaveFile>
#include <QTransform>
#include <exiv2/exiv2.hpp>
#include <sys/resource.h>

PhotoResult processPhoto(PhotoOptions options, std::vector<PhotoRawFrame> frames, QImage image)
{
    // Linux nice applies to this dedicated pool thread, not the preview workers.
    setpriority(PRIO_PROCESS, 0, 10);
    PhotoResult result{options, {}};
    if (!frames.empty()) {
        std::vector<libcamera::StillRawFrame> views;
        for (const auto &frame : frames)
            views.push_back({frame.data.data(), frame.data.size(), frame.parameters});
        libcamera::StillImage processed;
        int status = libcamera::StillImageProcessor::processRaw10Burst(views, &processed);
        if (status) {
            result.error = QStringLiteral("RAW photo processing failed (%1)").arg(status);
            return result;
        }
        image = QImage(processed.rgb.data(), processed.width, processed.height,
                       processed.width * 3, QImage::Format_RGB888).copy();
    }
    if (image.isNull()) {
        result.error = QStringLiteral("No image was captured");
        return result;
    }
    QTransform rotation;
    rotation.rotate(options.rotation);
    image = image.transformed(rotation);
    if (options.mirror)
        image = image.mirrored(true, false);
    image.setColorSpace(QColorSpace::SRgb);
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    QImageWriter writer(&buffer, "JPEG");
    writer.setQuality(100);
    writer.setOptimizedWrite(true);
    if (!writer.write(image)) {
        result.error = writer.errorString();
        return result;
    }
    try {
        auto encoded = Exiv2::ImageFactory::open(reinterpret_cast<const Exiv2::byte *>(jpeg.constData()), jpeg.size());
        encoded->readMetadata();
        auto &exif = encoded->exifData();
        const std::string stamp = options.captured.toString(QStringLiteral("yyyy:MM:dd HH:mm:ss")).toStdString();
        exif["Exif.Image.DateTime"] = stamp;
        exif["Exif.Photo.DateTimeOriginal"] = stamp;
        exif["Exif.Photo.DateTimeDigitized"] = stamp;
        exif["Exif.Image.Orientation"] = uint16_t(1);
        exif["Exif.Photo.ColorSpace"] = uint16_t(1);
        exif["Exif.Photo.PixelXDimension"] = uint32_t(image.width());
        exif["Exif.Photo.PixelYDimension"] = uint32_t(image.height());
        exif["Exif.Image.Make"] = "Xiaomi";
        exif["Exif.Image.Model"] = "Vermeer";
        exif["Exif.Image.Software"] = "libcamera";
        if (!frames.empty()) {
            const auto &p = frames.front().parameters;
            double us = p.analogueGain > 0 ? p.exposureProduct / p.analogueGain : 0;
            if (us > 0 && us < UINT32_MAX)
                exif["Exif.Photo.ExposureTime"] = Exiv2::URational(uint32_t(us + .5), 1000000);
        }
        encoded->writeMetadata();
        auto &io = encoded->io();
        jpeg = QByteArray(reinterpret_cast<const char *>(io.mmap()), io.size());
        io.munmap();
    } catch (const Exiv2::Error &) {
        // Keep the valid JPEG if optional EXIF encoding fails.
    }
    QSaveFile file(options.path);
    if (!file.open(QIODevice::WriteOnly) || file.write(jpeg) != jpeg.size() || !file.commit())
        result.error = file.errorString();
    return result;
}
