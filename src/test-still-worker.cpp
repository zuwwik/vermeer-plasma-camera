// SPDX-License-Identifier: GPL-2.0-or-later
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QTimer>
#include <algorithm>
#include <iostream>
#include "plasmacamera/worker.h"

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 5 && argc != 6) return 2;
    const int shutterDelay = argc == 6 ? std::stoi(argv[5]) : 2000;
    libcamera::CameraManager manager;
    if (manager.start()) return 3;
    auto camera = manager.get(argv[1]);
    if (!camera || camera->acquire()) return 4;
    const int width = std::stoi(argv[3]), height = std::stoi(argv[4]);
    Worker worker;
    QElapsedTimer elapsed;
    elapsed.start();
    std::vector<qint64> times;
    std::vector<QString> files;
    bool fourth = false;
    int saved = 0;
    QObject::connect(&worker, &Worker::errorOccurred, &app, [&](const QString &error) {
        std::cerr << "ERROR " << error.toStdString() << std::endl; app.exit(5);
    });
    QObject::connect(&worker, &Worker::photoFailed, &app, [&](int id, const QString &error) {
        std::cerr << "PHOTO ERROR " << id << ' ' << error.toStdString() << std::endl; app.exit(6);
    });
    QObject::connect(&worker, &Worker::viewFinderFrame, &app, [&](const QImage &) { times.push_back(elapsed.elapsed()); });
    auto capture = [&](int id) {
        PhotoOptions options;
        options.id = id; options.path = QString::fromLocal8Bit(argv[2]) + QString::number(id) + QStringLiteral(".jpg");
        options.rotation = id == 4 ? 90 : 0;
        options.captured = QDateTime::currentDateTime();
        std::cout << "SHUTTER " << id << ' ' << elapsed.elapsed() << std::endl;
        worker.capture(options);
    };
    QObject::connect(&worker, &Worker::photoSaved, &app, [&](int id, const QString &path) {
        std::cout << "SAVED " << id << ' ' << elapsed.elapsed() << ' ' << QFileInfo(path).size() << std::endl;
        files.push_back(path); saved++;
        if (!fourth) { fourth = true; QTimer::singleShot(400, &app, [&]() { capture(4); }); }
        if (saved == 4) QTimer::singleShot(700, &app, [&]() { app.quit(); });
    });
    worker.init(); worker.setCamera(camera);
    QTimer::singleShot(shutterDelay, &app, [&]() { capture(1); });
    QTimer::singleShot(shutterDelay+200, &app, [&]() { capture(2); });
    QTimer::singleShot(shutterDelay+400, &app, [&]() { capture(3); });
    QTimer::singleShot(60000, &app, [&]() { app.exit(9); });
    int status = app.exec();
    worker.unsetCamera(); camera.reset(); manager.stop();
    if (status) return status;
    if (saved != 4 || times.size() < 60) return 10;
    std::vector<qint64> gaps;
    for (size_t i=1;i<times.size();i++) if (times[i-1]>shutterDelay-100) gaps.push_back(times[i]-times[i-1]);
    std::sort(gaps.begin(),gaps.end());
    double fps = 1000. * (times.size()-1) / (times.back()-times.front());
    std::cout << "PREVIEW " << times.size() << " frames, " << fps << " fps; p95 gap " << gaps[gaps.size()*95/100] << " ms\n";
    for (const auto &file:files) {
        QImage image(file);
        bool rotated = file.endsWith(QStringLiteral("4.jpg"));
        std::cout << "JPEG " << file.toStdString() << ' ' << image.width() << 'x' << image.height() << '\n';
        if (image.width()!=(rotated?height:width)||image.height()!=(rotated?width:height)) return 11;
    }
    return fps >= 26.5 ? 0 : 12;
}
