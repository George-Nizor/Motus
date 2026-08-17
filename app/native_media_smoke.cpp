#include "project_controller.h"

#include "ve/media_probe.h"
#include "ve/native_media.h"
#include "ve/project_store.h"
#include "ve/project_workflows.h"

#include <QCoreApplication>
#include <QAudioOutput>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QMediaPlayer>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QVideoFrame>
#include <QVideoSink>

#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace {

constexpr std::int32_t kFrameRateNumerator = 30'000;
constexpr std::int32_t kFrameRateDenominator = 1'001;
constexpr std::int64_t kFrameCount = 90;

QString tool(QString name) {
#ifdef Q_OS_WIN
    name += ".exe";
#endif
    const auto bundled = QDir(QCoreApplication::applicationDirPath()).filePath(name);
    if (QFile::exists(bundled)) return bundled;
    return QStandardPaths::findExecutable(name);
}

QByteArray run(const QString& program, const QStringList& arguments,
               QByteArray* standardError = nullptr) {
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForStarted(10'000)) throw std::runtime_error("process did not start");
    if (!process.waitForFinished(60'000)) {
        process.kill();
        process.waitForFinished();
        throw std::runtime_error("process timed out");
    }
    if (standardError) *standardError = process.readAllStandardError();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        throw std::runtime_error("process failed: " + process.readAllStandardError().toStdString());
    }
    return process.readAllStandardOutput();
}

template <typename Predicate>
bool waitUntil(Predicate&& predicate, int timeoutMs = 15'000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(5);
    }
    return predicate();
}

void pumpEventsFor(int durationMs) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < durationMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(5);
    }
}

std::filesystem::path nativePath(const QString& value) {
#ifdef Q_OS_WIN
    return std::filesystem::path(value.toStdWString());
#else
    return std::filesystem::path(value.toStdString());
#endif
}

void exerciseQtPlayback(const QString& source) {
    QAudioOutput audio;
    QVideoSink video;
    // QMediaPlayer stores output pointers, so keep both outputs alive until after its teardown.
    QMediaPlayer player;
    audio.setMuted(true);
    player.setAudioOutput(&audio);
    player.setVideoOutput(&video);
    QString failure;
    bool loaded = false;
    bool decodedFrame = false;
    bool reachedEnd = false;
    QObject::connect(&player, &QMediaPlayer::errorOccurred,
                     [&](QMediaPlayer::Error error, const QString& message) {
        if (error != QMediaPlayer::NoError) failure = message;
    });
    QObject::connect(&player, &QMediaPlayer::mediaStatusChanged,
                     [&](QMediaPlayer::MediaStatus status) {
        loaded = loaded || status == QMediaPlayer::LoadedMedia ||
                           status == QMediaPlayer::BufferedMedia;
        reachedEnd = reachedEnd || status == QMediaPlayer::EndOfMedia;
    });
    QObject::connect(&video, &QVideoSink::videoFrameChanged, [&](const QVideoFrame& frame) {
        decodedFrame = decodedFrame || frame.isValid();
    });
    player.setSource(QUrl::fromLocalFile(source));
    QElapsedTimer timer;
    timer.start();
    while (!loaded && failure.isEmpty() && timer.elapsed() < 10'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(5);
    }
    if (!failure.isEmpty()) throw std::runtime_error("Qt preview load failed: " + failure.toStdString());
    if (!loaded) throw std::runtime_error("Qt preview did not load media");
    if (player.playbackState() != QMediaPlayer::StoppedState) {
        throw std::runtime_error("Qt preview was not stopped after loading");
    }

    player.setPosition(1'000);
    player.play();
    timer.restart();
    while ((player.position() < 1'150 || !decodedFrame ||
            player.playbackState() != QMediaPlayer::PlayingState) &&
           failure.isEmpty() && timer.elapsed() < 10'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(5);
    }
    player.pause();
    timer.restart();
    while (player.playbackState() != QMediaPlayer::PausedState &&
           failure.isEmpty() && timer.elapsed() < 2'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(5);
    }
    if (!failure.isEmpty()) throw std::runtime_error("Qt preview decode failed: " + failure.toStdString());
    if (player.position() < 1'150) throw std::runtime_error("Qt preview transport did not advance after seek");
    if (!decodedFrame) throw std::runtime_error("Qt preview produced no decoded video frame");
    if (player.playbackState() != QMediaPlayer::PausedState) {
        throw std::runtime_error("Qt preview did not enter the paused state");
    }
    const auto pausedPosition = player.position();
    timer.restart();
    while (timer.elapsed() < 150) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(5);
    }
    if (player.position() > pausedPosition + 40) {
        throw std::runtime_error("Qt preview position advanced while paused");
    }

    player.play();
    timer.restart();
    while ((!reachedEnd || player.playbackState() != QMediaPlayer::StoppedState) &&
           failure.isEmpty() && timer.elapsed() < 10'000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
        QThread::msleep(5);
    }
    if (!failure.isEmpty()) throw std::runtime_error("Qt preview failed before end of media: " + failure.toStdString());
    if (!reachedEnd || player.playbackState() != QMediaPlayer::StoppedState) {
        throw std::runtime_error("Qt preview did not reach EOF and return to stopped state");
    }
}

void assertAcceptanceMedia(const ve::MediaProbeResult& result, std::string_view label) {
    bool video = false;
    bool audio = false;
    for (const auto& stream : result.streams) {
        if (stream.kind == ve::MediaStreamKind::Video) {
            video = stream.codec == "h264" && stream.width == 320 && stream.height == 180 &&
                    stream.averageRateNumerator == kFrameRateNumerator &&
                    stream.averageRateDenominator == kFrameRateDenominator &&
                    stream.frameCount == kFrameCount;
        } else if (stream.kind == ve::MediaStreamKind::Audio) {
            audio = stream.codec == "aac" && stream.sampleRate == 48'000 &&
                    stream.channels == 2 && stream.channelLayout == "stereo";
        }
    }
    if (!video || !audio) {
        throw std::runtime_error(std::string(label) +
                                 " metadata does not match the NTSC H.264/AAC fixture");
    }
    if (result.duration.units < 2'950'000 || result.duration.units > 3'100'000) {
        throw std::runtime_error(std::string(label) + " duration is outside tolerance");
    }
}

ve::MediaProbeResult probeMedia(const QString& ffprobe, const QString& path) {
    const auto document = run(ffprobe, {"-v", "error", "-count_frames", "-show_streams",
                                        "-show_format", "-of", "json", path});
    return ve::parseFfprobeJson(
        std::string_view(document.constData(), static_cast<std::size_t>(document.size())));
}

QStringList clipIdentity(const ProjectController& controller) {
    QStringList result;
    for (const auto& value : controller.timelineClips()) {
        const auto clip = value.toMap();
        result.push_back(clip.value(QStringLiteral("id")).toString() + QStringLiteral(":") +
                         clip.value(QStringLiteral("assetId")).toString());
    }
    return result;
}

void assertExportComplete(ProjectController& controller, const QString& output,
                          const QString& ffprobe) {
    controller.exportSequence(QUrl::fromLocalFile(output));
    if (!controller.exporting()) throw std::runtime_error("controller export did not start");
    if (!waitUntil([&] { return !controller.exporting(); }, 30'000)) {
        throw std::runtime_error("controller export timed out");
    }
    if (controller.exportProgress() != 1.0 || !QFile::exists(output)) {
        throw std::runtime_error("controller export did not publish its final output");
    }
    assertAcceptanceMedia(probeMedia(ffprobe, output), "controller output");
}

void exerciseControllerWorkflow(const QString& source, const QString& replacement,
                                const QString& directory, const QString& ffprobe) {
    const auto projectFile = QDir(directory).filePath(QStringLiteral("controller.veproj"));
    const auto cancelledOutput = QDir(directory).filePath(QStringLiteral("cancelled.mp4"));
    const auto retryOutput = QDir(directory).filePath(QStringLiteral("retry.mp4"));
    const auto missingOutput = QDir(directory).filePath(QStringLiteral("missing.mp4"));
    const auto relinkedOutput = QDir(directory).filePath(QStringLiteral("relinked.mp4"));
    auto project = ve::makeNewProject(
        "Controller smoke", {320, 180, kFrameRateNumerator, kFrameRateDenominator,
                             48'000, 2, "Rec.709 SDR"});
    ve::ProjectStore::saveAtomically(project, nativePath(projectFile));

    QString assetId;
    QStringList clipsBeforeRelink;
    {
        ProjectController controller;
        controller.openProject(QUrl::fromLocalFile(projectFile));
        controller.appendMediaReference(QUrl::fromLocalFile(source));
        if (!waitUntil([&] {
                return !controller.probing() && controller.assetItems().size() == 1;
            })) {
            throw std::runtime_error("controller import/probe did not finish");
        }
        if (!controller.canPreview()) {
            throw std::runtime_error("controller did not enable preview after a durable probe");
        }
        const auto asset = controller.assetItems().front().toMap();
        assetId = asset.value(QStringLiteral("id")).toString();
        if (assetId.isEmpty() || asset.value(QStringLiteral("provisional")).toBool() ||
            !asset.value(QStringLiteral("online")).toBool()) {
            throw std::runtime_error("controller did not persist online probed media state");
        }
        clipsBeforeRelink = clipIdentity(controller);

        QVideoSink video;
        bool decodedFrame = false;
        qint64 furthestMediaPosition = -1;
        QObject::connect(&video, &QVideoSink::videoFrameChanged,
                         [&](const QVideoFrame& frame) { decodedFrame = decodedFrame || frame.isValid(); });
        auto* mediaPlayer = controller.findChild<QMediaPlayer*>();
        auto* audioOutput = controller.findChild<QAudioOutput*>();
        if (!mediaPlayer || !audioOutput) {
            throw std::runtime_error("controller media backend was not constructed");
        }
        audioOutput->setMuted(true);
        QObject::connect(mediaPlayer, &QMediaPlayer::positionChanged,
                         [&](qint64 position) { furthestMediaPosition = std::max(furthestMediaPosition, position); });
        controller.setVideoOutput(&video);
        controller.setPlayheadFrame(30);
        controller.togglePlayback();
        if (!waitUntil([&] { return controller.playing() && decodedFrame &&
                                    controller.playheadFrame() > 34; })) {
            throw std::runtime_error(
                "controller preview did not play from the requested seek (playing=" +
                std::to_string(controller.playing()) + ", decoded=" +
                std::to_string(decodedFrame) + ", frame=" +
                std::to_string(controller.playheadFrame()) + ", mediaPosition=" +
                std::to_string(mediaPlayer->position()) + ", furthestMediaPosition=" +
                std::to_string(furthestMediaPosition) + ", mediaDuration=" +
                std::to_string(mediaPlayer->duration()) + ", mediaStatus=" +
                std::to_string(static_cast<int>(mediaPlayer->mediaStatus())) + ", detail=" +
                controller.previewDetail().toStdString() + ")");
        }
        controller.pausePlayback();
        if (!waitUntil([&] { return !controller.playing(); }, 2'000)) {
            throw std::runtime_error("controller preview did not pause");
        }
        const auto pausedFrame = controller.playheadFrame();
        pumpEventsFor(200);
        if (controller.playheadFrame() != pausedFrame) {
            throw std::runtime_error("controller playhead advanced while paused");
        }
        controller.setPlayheadFrame(60);
        controller.togglePlayback();
        if (!waitUntil([&] {
                return !controller.playing() &&
                       controller.playheadFrame() == controller.durationFrames() &&
                       controller.previewDetail().contains(QStringLiteral("end"), Qt::CaseInsensitive);
            })) {
            throw std::runtime_error("controller preview did not reach its canonical EOF state");
        }

        controller.exportSequence(QUrl::fromLocalFile(cancelledOutput));
        if (!controller.exporting()) throw std::runtime_error("cancellable export did not start");
        controller.cancelExport();
        if (!waitUntil([&] { return !controller.exporting(); })) {
            throw std::runtime_error("cancelled export did not terminate");
        }
        if (!controller.exportDetail().contains(QStringLiteral("cancelled"), Qt::CaseInsensitive) ||
            QFile::exists(cancelledOutput) ||
            QFile::exists(QString::fromStdString(
                ve::stagedExportPath(nativePath(cancelledOutput)).string()))) {
            throw std::runtime_error("cancelled export did not remove every output");
        }
        assertExportComplete(controller, retryOutput, ffprobe);
        controller.save();
    }

    QFile::remove(replacement);
    if (!QFile::rename(source, replacement)) {
        throw std::runtime_error("could not move source media for missing/relink acceptance");
    }
    {
        ProjectController controller;
        controller.openProject(QUrl::fromLocalFile(projectFile));
        if (controller.assetItems().size() != 1 ||
            !controller.assetItems().front().toMap().value(QStringLiteral("missing")).toBool()) {
            throw std::runtime_error("controller did not report missing source media");
        }
        if (clipIdentity(controller) != clipsBeforeRelink) {
            throw std::runtime_error("opening missing media changed clip identity");
        }
        controller.exportSequence(QUrl::fromLocalFile(missingOutput));
        if (controller.exporting() || QFile::exists(missingOutput)) {
            throw std::runtime_error("missing media did not fail export preflight");
        }

        controller.relinkMedia(assetId, QUrl::fromLocalFile(replacement));
        if (!waitUntil([&] {
                return !controller.probing() && controller.assetItems().size() == 1 &&
                       controller.assetItems().front().toMap().value(QStringLiteral("online")).toBool();
            })) {
            throw std::runtime_error("controller relink/probe did not finish");
        }
        const auto relinked = controller.assetItems().front().toMap();
        if (relinked.value(QStringLiteral("id")).toString() != assetId ||
            relinked.value(QStringLiteral("provisional")).toBool() ||
            clipIdentity(controller) != clipsBeforeRelink) {
            throw std::runtime_error("relink changed durable asset or clip identity");
        }
        assertExportComplete(controller, relinkedOutput, ffprobe);
    }
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    try {
        const auto ffmpeg = tool("ffmpeg");
        const auto ffprobe = tool("ffprobe");
        if (ffmpeg.isEmpty() || ffprobe.isEmpty()) throw std::runtime_error("FFmpeg tools unavailable");
        QTemporaryDir temporary(QDir::temp().filePath(QStringLiteral("motus-native-smoke-XXXXXX")));
        if (!temporary.isValid()) throw std::runtime_error("cannot create temporary directory");
        const auto source = temporary.filePath("source.mp4");
        const auto output = temporary.filePath("output.mp4");
        const auto replacement = temporary.filePath("replacement.mp4");
        run(ffmpeg, {"-hide_banner", "-y", "-f", "lavfi", "-i",
            "testsrc2=size=320x180:rate=30000/1001,drawtext=text='%{n}':x=20:y=20:fontsize=28:fontcolor=white:box=1:boxcolor=black@0.65",
            "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=3.1",
            "-frames:v", QString::number(kFrameCount), "-c:v", "libx264", "-pix_fmt", "yuv420p",
            "-c:a", "aac", "-ac", "2", "-shortest", source});
        const auto probeJson = run(ffprobe, {"-v", "error", "-count_frames",
                                             "-show_streams", "-show_format",
                                             "-of", "json", source});
        const auto probe = ve::parseFfprobeJson(
            std::string_view(probeJson.constData(), static_cast<std::size_t>(probeJson.size())));
        assertAcceptanceMedia(probe, "source");
        exerciseQtPlayback(source);
        auto project = ve::makeNewProject(
            "Native smoke", {320, 180, kFrameRateNumerator, kFrameRateDenominator,
                             48'000, 2, "Rec.709 SDR"});
#ifdef Q_OS_WIN
        const std::filesystem::path sourcePath(source.toStdWString());
        const std::filesystem::path outputPath(output.toStdWString());
#else
        const std::filesystem::path sourcePath(source.toStdString());
        const std::filesystem::path outputPath(output.toStdString());
#endif
        (void)ve::appendProbedMediaReference(project, sourcePath, probe);
        const auto plan = ve::buildSimpleTimelinePlan(project, project.sequences[0]);
        const auto staged = ve::stagedExportPath(outputPath);
        const auto arguments = ve::buildFfmpegExportArguments(plan, staged);
        QStringList qtArguments;
        for (const auto& argument : arguments) qtArguments.push_back(QString::fromUtf8(argument));
        run(ffmpeg, qtArguments);
        std::filesystem::rename(staged, outputPath);
        const auto outputJson = run(ffprobe, {"-v", "error", "-count_frames",
                                              "-show_streams", "-show_format",
                                              "-of", "json", output});
        const auto rendered = ve::parseFfprobeJson(
            std::string_view(outputJson.constData(), static_cast<std::size_t>(outputJson.size())));
        assertAcceptanceMedia(rendered, "rendered output");
        exerciseControllerWorkflow(source, replacement, temporary.path(), ffprobe);
        std::cout << "native media smoke passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "native media smoke failed: " << error.what() << '\n';
        return 1;
    }
}
