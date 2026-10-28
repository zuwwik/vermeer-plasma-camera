// SPDX-FileCopyrightText: 2025 Andrew Wang
// SPDX-License-Identifier: GPL-2.0-or-later

#include "worker.h"
#include <QtConcurrent>
#include <QFutureWatcher>
#include <algorithm>

static const QMap<libcamera::PixelFormat, QImage::Format> NATIVE_FORMATS{
    {libcamera::formats::ABGR8888, QImage::Format_RGBX8888},
    {libcamera::formats::XBGR8888, QImage::Format_RGBX8888},
    {libcamera::formats::ARGB8888, QImage::Format_RGB32},
    {libcamera::formats::XRGB8888, QImage::Format_RGB32},
    {libcamera::formats::RGB888, QImage::Format_BGR888},
    {libcamera::formats::BGR888, QImage::Format_RGB888},
    {libcamera::formats::RGB565, QImage::Format_RGB16},
};

Worker::Worker() = default;

Worker::~Worker()
{
    m_photoPool.waitForDone();
}

void Worker::init()
{
    m_photoPool.setMaxThreadCount(1);
    // start init after we have moved to the other thread
    m_framePollTimer = new QTimer(this);
    m_framePollTimer->setInterval(1000 / 60); // manually set 60 fps
    connect(m_framePollTimer, &QTimer::timeout, this, &Worker::requestNextFrame);

    setState(State::Ready);
}

void Worker::shutdown()
{
    if (m_state == State::Running) {
        unsetCamera();
    }

    // terminate the worker
    Q_EMIT finished();
}

void Worker::setCamera(std::shared_ptr<libcamera::Camera> camera)
{
    switch (m_state) {
    case State::Ready:
        // if state is ready then go to running
        m_camera = camera;
        startViewFinder();

        setState(State::Running);
        setMode(CaptureMode::ViewFinder);
        break;

    case State::Running:
        setMode(CaptureMode::None);

        // Hold this while we switch camera
        setState(State::SwitchingCamera);

        // if state is running then switch out the camera
        stopViewFinder();
        m_camera = camera;
        startViewFinder();

        setState(State::Running);
        setMode(CaptureMode::ViewFinder);
        break;

    default:
        setError(QString::fromStdString("Attempting to set a camera but state is not Ready or Running"));
        // set error
        break;
    }
}

void Worker::unsetCamera()
{
    // we are responsible for cleaning up the camera
    if (m_state == State::Running) {
        setState(State::Stopping);

        stopViewFinder();

        setState(State::Ready);
        setMode(CaptureMode::None);
    }
}

void Worker::capture(const PhotoOptions &options)
{
    const unsigned int queued = m_photoJobs + unsigned(m_photoOptions.has_value()) + m_waitingPhotos.size();
    if (!m_camera || queued >= 3) {
        Q_EMIT photoFailed(options.id, QStringLiteral("The photo queue is busy"));
        Q_EMIT photoCapacity(m_camera && queued < 3);
        return;
    }
    if (m_photoOptions) {
        m_waitingPhotos.enqueue(options);
    } else {
        m_pauseFocus = m_lastAfState == libcamera::controls::AfStateFocused && m_camera->controls().count(&libcamera::controls::AfPause);
        m_photoOptions = options;
        m_photoFrames.clear();
    }
    Q_EMIT photoCapacity(queued + 1 < 3);
}

void Worker::submitPhoto(QImage fallback)
{
    if (!m_photoOptions) return;
    PhotoOptions options = *m_photoOptions;
    auto frames = std::move(m_photoFrames);
    m_photoFrames.clear();
    m_photoOptions.reset();
    m_resumeFocus = m_pauseFocus; m_pauseFocus = false;
    m_photoJobs++;
    if (!m_waitingPhotos.isEmpty()) {
        m_photoOptions = m_waitingPhotos.dequeue();
        m_pauseFocus = m_resumeFocus;
        m_resumeFocus = false;
    }
    Q_EMIT photoCapacity(m_photoJobs + unsigned(m_photoOptions.has_value()) + m_waitingPhotos.size() < 3);
    auto *watcher = new QFutureWatcher<PhotoResult>(this);
    connect(watcher, &QFutureWatcher<PhotoResult>::finished, this, [this, watcher]() {
        const PhotoResult result = watcher->result();
        watcher->deleteLater();
        m_photoJobs--;
        Q_EMIT photoCapacity(m_photoJobs + unsigned(m_photoOptions.has_value()) + m_waitingPhotos.size() < 3);
        if (result.error.isEmpty()) Q_EMIT photoSaved(result.options.id, result.options.path);
        else Q_EMIT photoFailed(result.options.id, result.error);
    });
    watcher->setFuture(QtConcurrent::run(&m_photoPool, [options, frames = std::move(frames), fallback]() mutable {
        return processPhoto(options, std::move(frames), fallback);
    }));
}

void Worker::collectPhoto(libcamera::Request *request, const QImage &fullFrame)
{
    if (!m_photoOptions) return;
    if (!m_rawStream || !request->buffers().count(m_rawStream)) {
        submitPhoto(fullFrame.copy());
        return;
    }
    const auto &config = m_rawStream->configuration();
    auto *buffer = request->buffers().at(m_rawStream);
    auto bytes = m_mappedBuffers.at(buffer)->data(0);
    if (bytes.size() < config.frameSize) {
        Q_EMIT photoFailed(m_photoOptions->id, QStringLiteral("Incomplete RAW buffer"));
        m_photoOptions.reset();
        Q_EMIT photoCapacity(m_photoJobs < 3);
        return;
    }
    PhotoRawFrame frame;
    frame.data.assign(bytes.begin(), bytes.begin() + config.frameSize);
    auto &p = frame.parameters;
    p.width = config.size.width; p.height = config.size.height; p.stride = config.stride;
    if (config.pixelFormat == libcamera::formats::SBGGR10_CSI2P) p.order = libcamera::StillBayerOrder::BGGR;
    else if (config.pixelFormat == libcamera::formats::SGBRG10_CSI2P) p.order = libcamera::StillBayerOrder::GBRG;
    else if (config.pixelFormat == libcamera::formats::SGRBG10_CSI2P) p.order = libcamera::StillBayerOrder::GRBG;
    else p.order = libcamera::StillBayerOrder::RGGB;
    const auto &metadata = request->metadata();
    if (auto gains = metadata.get(libcamera::controls::ColourGains))
        p.whiteBalance = {(*gains)[0], 1, (*gains)[1]};
    if (auto matrix = metadata.get(libcamera::controls::ColourCorrectionMatrix))
        std::copy(matrix->begin(), matrix->end(), p.colorMatrix.begin());
    p.gamma = metadata.get(libcamera::controls::Gamma).value_or(2.2f);
    p.contrast = metadata.get(libcamera::controls::Contrast).value_or(1.f);
    p.analogueGain = std::max(1.f, metadata.get(libcamera::controls::AnalogueGain).value_or(1.f));
    p.exposureProduct = metadata.get(libcamera::controls::ExposureTime).value_or(0) * double(p.analogueGain);
    if (auto black = metadata.get(libcamera::controls::SensorBlackLevels))
        p.blackLevel = (*black)[0] / 64.f;
    m_photoFrames.push_back(std::move(frame));
    if (m_photoFrames.size() == 3) submitPhoto();
}

void Worker::setSettings(const Settings& settings)
{
    m_settings = settings;
}

void Worker::setError(const QString &errorString)
{
    m_error = true;
    m_errorString = errorString;

    qWarning() << m_errorString;

    Q_EMIT errorChanged();
    Q_EMIT errorOccurred(m_errorString);
}

void Worker::setError(const int libcameraError)
{
    m_error = true;
    switch (libcameraError) {
    case -ENOMEM:
        m_errorString = QString::fromStdString("No buffer memory was available to handle the request");
        break;
    case -EACCES:
        m_errorString = QString::fromStdString("The camera is not in a state where it can be configured");
        break;
    case -EBUSY:
        m_errorString = QString::fromStdString("The camera is running and can't be released");
        break;
    case -EXDEV:
        m_errorString = QString::fromStdString("The request does not belong to this camera");
        break;
    case -ENODEV:
        m_errorString = QString::fromStdString("The camera has been disconnected from the system");
        break;
    case -EINVAL:
        m_errorString = QString::fromStdString("The configuration is not valid");
        break;
    default:
        m_errorString = QString::fromStdString("Unknown error: " + std::to_string(libcameraError));
        break;
    }

    qWarning() << m_errorString;

    Q_EMIT errorChanged();
    Q_EMIT errorOccurred(m_errorString);
}

void Worker::unsetError()
{
    m_error = false;
    m_errorString.clear();
}

void Worker::requestNextFrame()
{
    if (!m_camera) {
        return;
    }

    libcamera::Request *request;
    {
        // Grab free request from m_freeQueue, if there are any available
        const QMutexLocker lock(&m_freeMutex);
        if (m_freeQueue.isEmpty()) {
            return;
        }

        request = m_freeQueue.dequeue();
    }

    // Clear request for usage
    request->reuse(libcamera::Request::ReuseBuffers);

    // Load our settings onto the camera request's controls
    m_settings.set(request->controls());

    if (m_camera->controls().count(&libcamera::controls::AfPause)) {
        if (m_pauseFocus) request->controls().set(libcamera::controls::AfPause, libcamera::controls::AfPauseImmediate);
        else if (m_resumeFocus) {
            request->controls().set(libcamera::controls::AfPause, libcamera::controls::AfPauseResume);
            m_resumeFocus = false;
        }
    }
    // Queue request in camera for frame
    m_camera->queueRequest(request);
}

void Worker::requestComplete(libcamera::Request *request)
{
    // This is a slot that is run in libcamera thread, when a request completes.

    if (request->status() == libcamera::Request::RequestCancelled) {
        return;
    }

    {
        // Add finished request to queue to be processed
        const QMutexLocker lock(&m_doneMutex);
        m_doneQueue.enqueue(request);
    }

    // We are currently on the libcamera thread so we need to go back to the Worker thread.
    QMetaObject::invokeMethod(this, "processRequestDataAndEmit", Qt::QueuedConnection);
}

void Worker::processRequestDataAndEmit()
{
    if (!processRequestData()) return;
    Q_EMIT viewFinderFrame(m_image);
}

bool Worker::processRequestData()
{
    libcamera::Request *request;
    {
        // Obtain a finished request from the queue
        const QMutexLocker lock(&m_doneMutex);
        if (m_doneQueue.isEmpty()) {
            return false;
        }
        request = m_doneQueue.dequeue();
    }

    if (!request->buffers().count(m_stream)) {
        return false;
    }

    libcamera::FrameBuffer *buffer = request->buffers().at(m_stream);
    Image *image = m_mappedBuffers[buffer].get();

    // Load frame into m_image
    size_t size = buffer->metadata().planes()[0].bytesused;
    QImage fullFrame;
    if (getNativeFormats().contains(m_format)) {
        fullFrame = QImage(image->data(0).data(), m_size.width(), m_size.height(),
                           static_cast<qsizetype>(size) / m_size.height(), NATIVE_FORMATS[m_format]);
    } else {
        m_converter.convert(image, size, &m_conversionImage);
        fullFrame = m_conversionImage;
    }

    m_lastAfState = request->metadata().get(libcamera::controls::AfState).value_or(libcamera::controls::AfStateIdle);
    // Preserve the complete RAW/RGB photo before reducing the display texture.
    collectPhoto(request, fullFrame);
    m_image = fullFrame.width() > 1920 || fullFrame.height() > 1920
            ? fullFrame.scaled(1920, 1920, Qt::KeepAspectRatio, Qt::FastTransformation)
            : fullFrame.copy();

    {
        // Return request to the free queue to be used for new requests
        const QMutexLocker lock(&m_freeMutex);
        m_freeQueue.enqueue(request);
    }
    return true;
}

void Worker::startViewFinder()
{
    if (!m_camera) {
        return;
    }

    qDebug() << "Starting view finder in worker";

    m_rawStream = nullptr;
    m_lastAfState = libcamera::controls::AfStateIdle;
    m_pauseFocus = m_resumeFocus = false;
    m_config = m_camera->generateConfiguration({libcamera::StreamRole::Viewfinder, libcamera::StreamRole::Raw});
    if (m_config && m_config->size() == 2) {
        auto raw = m_config->at(1).pixelFormat;
        if (raw != libcamera::formats::SRGGB10_CSI2P && raw != libcamera::formats::SGRBG10_CSI2P &&
            raw != libcamera::formats::SGBRG10_CSI2P && raw != libcamera::formats::SBGGR10_CSI2P)
            m_config.reset();
    } else m_config.reset();
    if (!m_config) m_config = m_camera->generateConfiguration({libcamera::StreamRole::Viewfinder});

    configure();

    // Connect signal for when a frame request is complete
    m_camera->requestCompleted.connect(this, &Worker::requestComplete);

    // Start camera
    if (int ret = m_camera->start(); ret < 0) {
        setError(ret);
        return;
    }

    // Add all requests
    for (std::unique_ptr<libcamera::Request> &request : m_requests) {
        if (int ret = m_camera->queueRequest(request.get()); ret < 0) {
            setError(ret);
            return;
        }
    }

    // Start polling for frames
    m_framePollTimer->start();
}

void Worker::stopViewFinder()
{
    qDebug() << "stop view finder";
    m_framePollTimer->stop();
    while (!m_waitingPhotos.isEmpty()) {
        const auto options = m_waitingPhotos.dequeue();
        Q_EMIT photoFailed(options.id, QStringLiteral("Camera stopped before exposure"));
    }
    if (m_photoOptions) {
        if (!m_photoFrames.empty()) submitPhoto();
        else {
            Q_EMIT photoFailed(m_photoOptions->id, QStringLiteral("Camera stopped before exposure"));
            m_photoOptions.reset();
        }
    }
    m_image = QImage();
    m_conversionImage = QImage();

    if (m_camera) {
        // All pending requests are cancelled
        int ret = m_camera->stop();
        if (ret < 0) {
            setError(ret);
            return;
        }

        // Cleanup signals
        m_camera->requestCompleted.disconnect(this);
    }

    // Must be cleared before following variables are cleared
    m_requests.clear();

    m_config.reset();

    if (m_allocator) {
        m_mappedBuffers.clear();
        m_allocator->free(m_stream);
        if (m_rawStream) m_allocator->free(m_rawStream);
        delete m_allocator;
        m_allocator = nullptr;
        m_rawStream = nullptr;
    }

    m_mappedBuffers.clear();

    {
        const QMutexLocker lock(&m_freeMutex);
        m_freeQueue.clear();
    }

    /*
     * A CaptureEvent may have been posted before we stopped the camera,
     * but not processed yet. Clear the queue of done buffers to avoid
     * racing with the event handler.
     */
    {
        const QMutexLocker lock(&m_doneMutex);
        m_doneQueue.clear();
    }

    if (m_camera) {
        int ret = m_camera->release();
        if (ret < 0) {
            setError(ret);
            return;
        }
    }

    m_camera.reset();
}

void Worker::setState(const State state)
{
    // handles sending the signals for state changes
    const State previous_state = m_state;

    switch (state)
    {
    case State::None:
        // after initial init it should not be possible to go back to None
        // m_state = State::None;
        break;

    case State::Ready:
        m_state = State::Ready;
        switch (previous_state)
        {
        case State::None:
        case State::Stopping:
            Q_EMIT ready();
            break;
        default:
            qWarning() << "Transitioned from state other than None or Stopping to Ready";
            break;
        }
        break;

    case State::Running:
        m_state = State::Running;
        switch (previous_state)
        {
        case State::Ready:
        case State::SwitchingCamera:
            break;
        default:
            qWarning() << "Transitioned from state other than Ready to Running";
            break;
        }

        Q_EMIT running();
        break;

    case State::SwitchingCamera:
        m_state = State::SwitchingCamera;
        break;

    case State::Stopping:
        m_state = State::Stopping;
        break;
    }
}

void Worker::setMode(CaptureMode mode)
{
    // Handles sending the signals for state changes
    const CaptureMode previous_mode = m_mode;

    switch (mode) {
    case CaptureMode::None:
        m_mode = CaptureMode::None;
        break;

    case CaptureMode::ViewFinder:
        m_mode = CaptureMode::ViewFinder;
        switch (previous_mode)
        {
        case CaptureMode::None:
            break;
        case CaptureMode::StillImage:
            break;
        default:
            qWarning() << "Transition from state other than None or StillImage to ViewFinder";
            break;
        }
        break;

    case CaptureMode::StillImage:
        m_mode = CaptureMode::StillImage;
        break;

    case CaptureMode::RawImage:
        m_mode = CaptureMode::RawImage;
        break;

    case CaptureMode::Video:
        m_mode = CaptureMode::Video;
        break;
    }
}

void Worker::configure()
{
    if (!m_config || !m_camera) {
        return;
    }

    // Add stream configuration
    libcamera::StreamConfiguration &streamConfig = m_config->at(0);
    qDebug() << "Default camera configuration is: " << streamConfig.toString();

    const QList<libcamera::PixelFormat> wantFormats = getNativeFormats();
    for (const libcamera::PixelFormat &format : wantFormats) {
        qDebug() << "Desired format: " << format.toString();
    }

    std::vector<libcamera::PixelFormat> haveFormats = streamConfig.formats().pixelformats();
    for (const libcamera::PixelFormat &format : haveFormats) {
        qDebug() << "Got format: " << format.toString();
        auto match = std::find_if(wantFormats.begin(), wantFormats.end(), [&](const libcamera::PixelFormat &f) {
            return f == format;
        });
        if (match != wantFormats.end()) {
            streamConfig.pixelFormat = format;
            break;
        }
    }

    // TODO: allow user to override config
    //  - https://git.libcamera.org/libcamera/libcamera.git/tree/src/apps/qcam/main_window.cpp#n399

    qDebug() << "Viewfinder configuration is: " << streamConfig.toString();

    // Validate config
    const libcamera::CameraConfiguration::Status validation = m_config->validate();
    if (validation == libcamera::CameraConfiguration::Adjusted) {
        qInfo() << "Adjusted viewfinder configuration is: " << streamConfig.toString();
    }
    if (validation == libcamera::CameraConfiguration::Invalid) {
        qWarning() << QString::fromStdString("Failed to validate camera configuration");
        return;
    }

    // TODO: it might actually be possible to quick switch the config of the camera
    //  - m_camera->configure can be called when the camera is in the configured state so we don't have to stop all the way
    //  - https://libcamera.org/api-html/classlibcamera_1_1Camera.html#a4c190f4c369628e5cd96770790559a26
    int ret = m_camera->configure(m_config.get());
    if (ret < 0) {
        setError(ret);
        return;
    }

    m_stream = m_config->at(0).stream();
    m_rawStream = m_config->size() > 1 ? m_config->at(1).stream() : nullptr;

    ret = setFormat(QSize(static_cast<int>(streamConfig.size.width), static_cast<int>(streamConfig.size.height)),
                    streamConfig.pixelFormat,
                    streamConfig.colorSpace.value_or(libcamera::ColorSpace::Sycc),
                    streamConfig.stride);
    if (ret < 0) {
        setError(ret);
        return;
    }

    ret = createRequests();
    if (ret < 0) {
        setError(ret);
        return;
    }
}

const QList<libcamera::PixelFormat>& Worker::getNativeFormats()
{
    static const QList<libcamera::PixelFormat> formats = NATIVE_FORMATS.keys();
    return formats;
}

int Worker::setFormat(
    const QSize& size,
    const libcamera::PixelFormat& format,
    const libcamera::ColorSpace& colorSpace,
    const unsigned int stride)
{
    m_image = QImage();
    m_conversionImage = QImage();
    if (!getNativeFormats().contains(format)) {
        if (const int ret = m_converter.configure(format, size, stride); ret < 0) {
            return ret;
        }

        m_conversionImage = QImage(size, QImage::Format_RGB32);
        qDebug() << "Using software format conversion from" << format.toString();
    }

    m_format = format;
    m_size = size;

    return 0;
}

int Worker::createRequests()
{
    if (!m_camera) {
        qWarning() << "No camera set in worker";
        return -1;
    }

    qDebug() << "create req" << QThread::currentThread()->currentThreadId();

    // FrameBufferAllocator creates FrameBuffer instances that can be used by libcamera to store frames
    m_allocator = new libcamera::FrameBufferAllocator(m_camera);

    int ret = m_allocator->allocate(m_stream);
    if (ret < 0) {
        qWarning() << "Can't allocate buffers";
        return ret;
    }

    if (m_rawStream) {
        ret = m_allocator->allocate(m_rawStream);
        if (ret < 0) return ret;
        for (const auto &buffer : m_allocator->buffers(m_rawStream)) {
            auto image = Image::fromFrameBuffer(buffer.get(), Image::MapMode::ReadOnly);
            if (!image) return -ENOMEM;
            m_mappedBuffers[buffer.get()] = std::move(image);
        }
    }
    const size_t allocated = m_allocator->buffers(m_stream).size();
    qDebug() << "Allocated" << allocated << "buffers for stream";

    // using the created buffers we create a mapping from the buffer to an image
    size_t rawIndex = 0;
    for (const std::unique_ptr<libcamera::FrameBuffer> &buffer : m_allocator->buffers(m_stream)) {
        // Map memory buffers and cache the mappings
        std::unique_ptr<Image> image =
            Image::fromFrameBuffer(buffer.get(), Image::MapMode::ReadOnly);
        Q_ASSERT(image);

        m_mappedBuffers[buffer.get()] = std::move(image);

        // Create request from the buffer
        std::unique_ptr<libcamera::Request> request = m_camera->createRequest();
        if (!request) {
            qWarning() << "Can't create request";
            return ret;
        }
        ret = request->addBuffer(m_stream, buffer.get());
        if (ret < 0) {
            qWarning() << "Can't set buffer for request";
            return ret;
        }
        if (m_rawStream) {
            const auto &buffers = m_allocator->buffers(m_rawStream);
            if (rawIndex >= buffers.size()) return -ENOMEM;
            ret = request->addBuffer(m_rawStream, buffers[rawIndex++].get());
            if (ret < 0) return ret;
        }
        qDebug() << "Added new request: " << request->toString();

        m_requests.push_back(std::move(request));
    }

    return 0;
}
