#include "project_controller.h"

#include "ve/media_integrity.h"
#include "ve/media_probe.h"
#include "ve/mlt_graph.h"
#include "ve/native_media.h"
#include "ve/project_store.h"
#include "ve/project_workflows.h"

#include <QDir>
#include <QAudioOutput>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QProcess>
#include <QStandardPaths>
#include <QVariantMap>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

std::filesystem::path localPath(const QUrl& url) {
    const auto value = url.isLocalFile() ? url.toLocalFile() : url.toString();
#ifdef Q_OS_WIN
    return std::filesystem::path(value.toStdWString());
#else
    return std::filesystem::path(value.toStdString());
#endif
}

QString displayPath(const std::filesystem::path& path) {
#ifdef Q_OS_WIN
    return QDir::toNativeSeparators(QString::fromStdWString(path.wstring()));
#else
    return QDir::toNativeSeparators(QString::fromStdString(path.string()));
#endif
}

QString assetStatus(const ve::Asset& asset) {
    switch (asset.status) {
    case ve::AssetStatus::Online: return QStringLiteral("Online");
    case ve::AssetStatus::Missing: return QStringLiteral("Missing");
    case ve::AssetStatus::Modified: return QStringLiteral("Modified");
    case ve::AssetStatus::Unsupported: return QStringLiteral("Unreadable / unsupported");
    }
    return QStringLiteral("Unknown");
}

QString integrityStatus(QString prefix, const ve::MediaIntegrityReport& report) {
    if (report.checked == 0U) return prefix + QStringLiteral(" No referenced media.");
    auto detail = QStringLiteral(" %1 online · %2 missing · %3 modified")
        .arg(report.online).arg(report.missing).arg(report.modified);
    if (report.unsupported > 0U) {
        detail += QStringLiteral(" · %1 unreadable/unsupported").arg(report.unsupported);
    }
    if (!report.issues.empty()) {
        detail += QStringLiteral(". %1 file error(s); source files were not changed.")
            .arg(report.issues.size());
    } else {
        detail += QStringLiteral(". Source files were not changed.");
    }
    return prefix + detail;
}

QString boundedDiagnostic(QByteArray value, int maximum = 1800) {
    value = value.trimmed();
    if (value.size() > maximum) value = value.right(maximum);
    return QString::fromUtf8(value).trimmed();
}

void promoteStagedOutput(const std::filesystem::path& staged,
                         const std::filesystem::path& output) {
#ifdef Q_OS_WIN
    if (!MoveFileExW(staged.c_str(), output.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        throw std::runtime_error("Windows could not atomically publish the render (error " +
                                 std::to_string(GetLastError()) + ")");
    }
#else
    std::error_code error;
    std::filesystem::rename(staged, output, error);
    if (error) throw std::runtime_error("could not atomically publish the render: " + error.message());
#endif
}

bool sameFilesystemPath(const std::filesystem::path& lhs,
                        const std::filesystem::path& rhs) {
    std::error_code error;
    const auto left = std::filesystem::absolute(lhs, error).lexically_normal();
    if (error) return false;
    const auto right = std::filesystem::absolute(rhs, error).lexically_normal();
    if (error) return false;
#ifdef Q_OS_WIN
    if (QString::fromStdWString(left.wstring()).compare(
            QString::fromStdWString(right.wstring()), Qt::CaseInsensitive) == 0) return true;
#else
    if (left == right) return true;
#endif
    const bool leftExists = std::filesystem::exists(left, error);
    if (error || !leftExists) return false;
    const bool rightExists = std::filesystem::exists(right, error);
    if (error || !rightExists) return false;
    return std::filesystem::equivalent(left, right, error) && !error;
}

} // namespace

ProjectController::ProjectController(QObject* parent) : QObject(parent), history_(200) {
    mediaPlayer_ = new QMediaPlayer(this);
    audioOutput_ = new QAudioOutput(this);
    mediaPlayer_->setAudioOutput(audioOutput_);
    connect(mediaPlayer_, &QMediaPlayer::playbackStateChanged, this, [this] {
        emit mediaStateChanged();
    });
    connect(mediaPlayer_, &QMediaPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
        if (previewSeekPending_ &&
            (status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia)) {
            finishPreviewSeek();
        } else if (status == QMediaPlayer::EndOfMedia && previewPlayAfterSeek_) {
            advancePreviewSegment();
        }
    });
    connect(mediaPlayer_, &QMediaPlayer::positionChanged, this, [this](qint64 positionMs) {
        if (!previewPlan_ || !previewSegmentIndex_ ||
            *previewSegmentIndex_ >= previewPlan_->segments.size()) return;
        const auto& segment = previewPlan_->segments[*previewSegmentIndex_];
        const auto sourceUs = positionMs * 1000;
        const auto sourceEndUs = segment.sourceInUs + segment.durationUs;
        if (playing() && sourceUs >= sourceEndUs - 1000) {
            advancePreviewSegment();
            return;
        }
        const auto offsetUs = std::clamp(sourceUs - segment.sourceInUs,
                                         std::int64_t{0}, segment.durationUs);
        const auto frame = frameForMicroseconds(segment.timelineStartUs + offsetUs);
        if (frame == playheadFrame_) return;
        updatingPlayheadFromPreview_ = true;
        playheadFrame_ = std::clamp(frame, qint64{0}, durationFrames());
        updatingPlayheadFromPreview_ = false;
        emit workspaceChanged();
    });
    connect(mediaPlayer_, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error error, const QString& message) {
        if (error == QMediaPlayer::NoError) return;
        previewSeekPending_ = false;
        previewDetail_ = QStringLiteral("Preview failed: %1").arg(message);
        setStatus(previewDetail_);
        emit mediaStateChanged();
    });

    exportKillTimer_.setSingleShot(true);
    exportKillTimer_.setInterval(1500);
    connect(&exportKillTimer_, &QTimer::timeout, this, [this] {
        if (exportProcess_ && exportProcess_->state() != QProcess::NotRunning) {
            exportProcess_->kill();
        }
    });
    recoveryTimer_.setSingleShot(true);
    recoveryTimer_.setInterval(5000);
    connect(&recoveryTimer_, &QTimer::timeout, this, [this] {
        if (!project_ || project_->projectPath.empty() || !dirty_) return;
        try {
            const auto cache = project_->projectPath.parent_path() /
                (project_->projectPath.stem().string() + ".ve-cache") / "recovery";
            ve::ProjectStore::saveRecoverySnapshot(*project_, cache);
            setStatus(QStringLiteral("Recovery snapshot updated."));
        } catch (const std::exception& error) {
            setStatus(QStringLiteral("Recovery snapshot failed: %1").arg(QString::fromUtf8(error.what())));
        }
    });
}

ve::Project& ProjectController::project() {
    if (!project_) throw std::runtime_error("no project is open");
    return *project_;
}

const ve::Project& ProjectController::project() const {
    if (!project_) throw std::runtime_error("no project is open");
    return *project_;
}

QString ProjectController::projectName() const {
    return project_ ? QString::fromStdString(project_->name) : QStringLiteral("No project");
}
QString ProjectController::projectPath() const {
    return project_ ? displayPath(project_->projectPath) : QString{};
}
QString ProjectController::sequenceName() const {
    if (!project_ || !project_->activeSequenceId) return {};
    const auto* sequence = project_->findSequence(*project_->activeSequenceId);
    return sequence ? QString::fromStdString(sequence->name) : QString{};
}

QStringList ProjectController::assetNames() const {
    QStringList values;
    if (!project_) return values;
    for (const auto& asset : project_->assets) {
        values.push_back(QString::fromStdString(asset.displayName));
    }
    return values;
}

QVariantList ProjectController::assetItems() const {
    QVariantList values;
    if (!project_) return values;
    for (const auto& asset : project_->assets) {
        QVariantMap item;
        item.insert(QStringLiteral("id"), QString::fromStdString(asset.id));
        item.insert(QStringLiteral("name"), QString::fromStdString(asset.displayName));
        item.insert(QStringLiteral("path"), displayPath(asset.path));
        item.insert(QStringLiteral("status"), assetStatus(asset));
        item.insert(QStringLiteral("online"), asset.status == ve::AssetStatus::Online);
        item.insert(QStringLiteral("missing"), asset.status == ve::AssetStatus::Missing);
        item.insert(QStringLiteral("modified"), asset.status == ve::AssetStatus::Modified);
        item.insert(QStringLiteral("relinkable"), asset.status == ve::AssetStatus::Missing ||
                                                   asset.status == ve::AssetStatus::Modified);
        item.insert(QStringLiteral("provisional"), !asset.probe.has_value());
        item.insert(QStringLiteral("probeBackend"), QString::fromStdString(asset.probeBackend));
        item.insert(QStringLiteral("format"), asset.probe
            ? QString::fromStdString(asset.probe->formatName) : QString{});
        item.insert(QStringLiteral("hasVideo"), asset.hasVideo);
        item.insert(QStringLiteral("hasAudio"), asset.hasAudio);
        item.insert(QStringLiteral("durationFrames"), frameFor(asset.duration));
        values.push_back(item);
    }
    return values;
}

QVariantList ProjectController::timelineClips() const {
    QVariantList values;
    if (!project_ || !project_->activeSequenceId) return values;
    const auto* sequence = project_->findSequence(*project_->activeSequenceId);
    if (!sequence) return values;
    for (const auto& track : sequence->tracks) {
        for (const auto& clip : track.clips) {
            const auto* asset = project_->findAsset(clip.assetId);
            const auto start = frameFor(clip.timelineStart);
            const auto duration = frameFor(clip.duration);
            QVariantMap item;
            item.insert(QStringLiteral("id"), QString::fromStdString(clip.id));
            item.insert(QStringLiteral("linkedGroupId"), QString::fromStdString(clip.linkedGroupId));
            item.insert(QStringLiteral("assetId"), QString::fromStdString(clip.assetId));
            item.insert(QStringLiteral("name"), asset
                ? QString::fromStdString(asset->displayName) : QStringLiteral("Offline media"));
            item.insert(QStringLiteral("trackId"), QString::fromStdString(track.id));
            item.insert(QStringLiteral("trackName"), QString::fromStdString(track.name));
            item.insert(QStringLiteral("kind"), track.kind == ve::TrackKind::Video
                ? QStringLiteral("video") : QStringLiteral("audio"));
            item.insert(QStringLiteral("startFrame"), start);
            item.insert(QStringLiteral("endFrame"), start + duration);
            item.insert(QStringLiteral("durationFrames"), duration);
            item.insert(QStringLiteral("sourceInFrame"), frameFor(clip.sourceIn));
            item.insert(QStringLiteral("offline"), !asset || asset->status != ve::AssetStatus::Online);
            item.insert(QStringLiteral("provisional"), !asset || !asset->probe.has_value());
            values.push_back(item);
        }
    }
    return values;
}

QVariantList ProjectController::trackItems() const {
    QVariantList values;
    const auto* sequence = activeSequence();
    if (!sequence) return values;
    for (const auto& track : sequence->tracks) {
        QVariantMap item;
        item.insert(QStringLiteral("id"), QString::fromStdString(track.id));
        item.insert(QStringLiteral("name"), QString::fromStdString(track.name));
        item.insert(QStringLiteral("kind"), track.kind == ve::TrackKind::Video
            ? QStringLiteral("video") : QStringLiteral("audio"));
        item.insert(QStringLiteral("locked"), track.locked);
        item.insert(QStringLiteral("muted"), track.muted);
        item.insert(QStringLiteral("visible"), track.visible);
        values.push_back(item);
    }
    return values;
}

QVariantList ProjectController::markerItems() const {
    QVariantList values;
    const auto* sequence = activeSequence();
    if (!sequence) return values;
    for (const auto& marker : sequence->markers) {
        QVariantMap item;
        item.insert(QStringLiteral("id"), QString::fromStdString(marker.id));
        item.insert(QStringLiteral("label"), QString::fromStdString(marker.label));
        item.insert(QStringLiteral("color"), QString::fromStdString(marker.color));
        item.insert(QStringLiteral("frame"), frameFor(marker.time));
        values.push_back(item);
    }
    return values;
}

const ve::Sequence* ProjectController::activeSequence() const {
    if (!project_ || !project_->activeSequenceId) return nullptr;
    return project_->findSequence(*project_->activeSequenceId);
}

const ve::Clip* ProjectController::selectedClip() const {
    if (!project_ || !selectedClipId_) return nullptr;
    const auto* sequence = activeSequence();
    if (!sequence) return nullptr;
    for (const auto& track : sequence->tracks) {
        const auto clip = std::ranges::find(track.clips, *selectedClipId_, &ve::Clip::id);
        if (clip != track.clips.end()) return &*clip;
    }
    return nullptr;
}

const ve::Track* ProjectController::selectedTrack() const {
    if (!selectedClipId_) return nullptr;
    const auto* sequence = activeSequence();
    if (!sequence) return nullptr;
    for (const auto& track : sequence->tracks) {
        if (std::ranges::find(track.clips, *selectedClipId_, &ve::Clip::id) != track.clips.end()) {
            return &track;
        }
    }
    return nullptr;
}

qint64 ProjectController::frameFor(const ve::MediaTime& time) const {
    if (!project_) return 0;
    return static_cast<qint64>(time.rescaled(project_->profile.frameRateNumerator,
                                             project_->profile.frameRateDenominator).units);
}

qint64 ProjectController::frameForMicroseconds(std::int64_t microseconds) const {
    if (!project_) return 0;
    return static_cast<qint64>(ve::MediaTime{microseconds, 1'000'000, 1}.rescaled(
        project_->profile.frameRateNumerator, project_->profile.frameRateDenominator).units);
}

QString ProjectController::mediaToolPath(const QString& baseName) const {
#ifdef Q_OS_WIN
    const auto executableName = baseName + QStringLiteral(".exe");
#else
    const auto executableName = baseName;
#endif
    const auto bundled = QDir(QCoreApplication::applicationDirPath()).filePath(executableName);
    if (QFileInfo::exists(bundled) && QFileInfo(bundled).isExecutable()) return bundled;
    // An installed bundle has a root qt.conf. Fail closed there so a broken package can never be
    // masked by FFmpeg from a developer PATH; non-deployed source builds may use PATH.
    if (QFileInfo::exists(QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("qt.conf")))) return {};
    return QStandardPaths::findExecutable(executableName);
}

bool ProjectController::mediaToolsAvailable() const {
    return !mediaToolPath(QStringLiteral("ffprobe")).isEmpty() &&
           !mediaToolPath(QStringLiteral("ffmpeg")).isEmpty();
}

bool ProjectController::playing() const {
    return mediaPlayer_ && mediaPlayer_->playbackState() == QMediaPlayer::PlayingState;
}

bool ProjectController::canPreview() const {
    if (!project_ || !project_->activeSequenceId) return false;
    const auto* sequence = activeSequence();
    if (!sequence) return false;
    try {
        (void)ve::buildSimpleTimelinePlan(*project_, *sequence);
        return true;
    } catch (...) {
        return false;
    }
}

bool ProjectController::hasSelectedClip() const { return selectedClip() != nullptr; }

QString ProjectController::selectedClipId() const {
    return selectedClipId_ ? QString::fromStdString(*selectedClipId_) : QString{};
}

QString ProjectController::selectedLinkedGroupId() const {
    const auto* clip = selectedClip();
    return clip ? QString::fromStdString(clip->linkedGroupId) : QString{};
}

QString ProjectController::selectedClipName() const {
    const auto* clip = selectedClip();
    if (!clip || !project_) return {};
    const auto* asset = project_->findAsset(clip->assetId);
    return asset ? QString::fromStdString(asset->displayName) : QStringLiteral("Offline media");
}

QString ProjectController::selectedClipKind() const {
    const auto* track = selectedTrack();
    if (!track) return {};
    return track->kind == ve::TrackKind::Video ? QStringLiteral("Video") : QStringLiteral("Audio");
}

qint64 ProjectController::selectedClipStartFrame() const {
    const auto* clip = selectedClip();
    return clip ? frameFor(clip->timelineStart) : 0;
}

qint64 ProjectController::selectedClipDurationFrames() const {
    const auto* clip = selectedClip();
    return clip ? frameFor(clip->duration) : 0;
}

qint64 ProjectController::selectedClipEndFrame() const {
    return selectedClipStartFrame() + selectedClipDurationFrames();
}

qint64 ProjectController::durationFrames() const {
    if (!project_ || !project_->activeSequenceId) return 0;
    const auto* sequence = project_->findSequence(*project_->activeSequenceId);
    if (!sequence) return 0;
    qint64 end = 0;
    for (const auto& track : sequence->tracks)
        for (const auto& clip : track.clips)
            end = std::max(end, static_cast<qint64>(clip.timelineEnd().rescaled(
                project_->profile.frameRateNumerator, project_->profile.frameRateDenominator).units));
    return end;
}

void ProjectController::setStatus(QString status) {
    if (status_ == status) return;
    status_ = std::move(status);
    emit statusChanged();
}

void ProjectController::changed(bool dirty) {
    dirty_ = dirty;
    if (dirty_ && project_ && !project_->projectPath.empty()) recoveryTimer_.start();
    else recoveryTimer_.stop();
    if (selectedClipId_ && !selectedClip()) selectedClipId_.reset();
    const auto end = durationFrames();
    playheadFrame_ = std::clamp(playheadFrame_, qint64{0}, end);
    if (inPointFrame_ > end) inPointFrame_ = end;
    if (outPointFrame_ > end) outPointFrame_ = end;
    if (inPointFrame_ >= 0 && outPointFrame_ >= 0 && outPointFrame_ <= inPointFrame_) {
        outPointFrame_ = -1;
    }
    invalidatePreview();
    emit projectChanged();
    emit workspaceChanged();
}

void ProjectController::resetWorkspaceState() {
    ++projectEpoch_;
    selectedClipId_.reset();
    inPointFrame_ = -1;
    outPointFrame_ = -1;
    playheadFrame_ = 0;
    emit workspaceChanged();
}

void ProjectController::newProject(const QString& name) {
    project_ = ve::makeNewProject(name.trimmed().isEmpty() ? "Untitled" : name.toStdString());
    history_.clear();
    resetWorkspaceState();
    changed();
    setStatus(QStringLiteral("New project created. Save it before editing important media."));
}

void ProjectController::openProject(const QUrl& path) {
    try {
        project_ = ve::ProjectStore::load(localPath(path));
        const auto integrity = ve::refreshMediaIntegrity(*project_);
        history_.clear();
        resetWorkspaceState();
        // Persist refreshed paths, statuses, and any upgraded legacy fingerprint baseline.
        changed(integrity.changed > 0U);
        setStatus(integrityStatus(QStringLiteral("Project opened and validated."), integrity));
        probeUnprobedAssets();
    } catch (const std::exception& error) {
        setStatus(QStringLiteral("Open failed: %1").arg(QString::fromUtf8(error.what())));
    }
}

void ProjectController::save() {
    if (!project_) return;
    if (project_->projectPath.empty()) {
        emit savePathRequired();
        return;
    }
    try {
        ve::ProjectStore::saveAtomically(*project_, project_->projectPath);
        changed(false);
        setStatus(QStringLiteral("Project saved atomically."));
    } catch (const std::exception& error) {
        setStatus(QStringLiteral("Save failed: %1").arg(QString::fromUtf8(error.what())));
    }
}

void ProjectController::saveAs(const QUrl& path) {
    if (!project_) return;
    project_->projectPath = localPath(path);
    save();
}

void ProjectController::runProbe(
    const std::filesystem::path& path, QString activity,
    std::function<void(const ve::MediaProbeResult&, const QString&)> onSuccess) {
    const auto program = mediaToolPath(QStringLiteral("ffprobe"));
    if (program.isEmpty()) {
        setStatus(QStringLiteral("FFprobe is unavailable. Rebuild the Motus bundle or install FFmpeg for this source build."));
        return;
    }
    auto* process = new QProcess(this);
    process->setProperty("motusProbe", true);
    process->setProgram(program);
    process->setArguments({QStringLiteral("-v"), QStringLiteral("error"),
        QStringLiteral("-show_streams"), QStringLiteral("-show_format"),
        QStringLiteral("-of"), QStringLiteral("json"), displayPath(path)});
    process->setProcessChannelMode(QProcess::SeparateChannels);
    const auto epoch = projectEpoch_;
    auto completed = std::make_shared<bool>(false);
    ++activeProbeCount_;
    emit mediaStateChanged();
    setStatus(std::move(activity));

    const auto finishCount = [this, completed, process] {
        if (*completed) return false;
        *completed = true;
        activeProbeCount_ = std::max(0, activeProbeCount_ - 1);
        emit mediaStateChanged();
        process->deleteLater();
        return true;
    };
    connect(process, &QProcess::errorOccurred, this,
            [this, process, finishCount](QProcess::ProcessError error) mutable {
        if (error != QProcess::FailedToStart || !finishCount()) return;
        setStatus(QStringLiteral("Probe failed to start: %1").arg(process->errorString()));
    });
    connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this, process, finishCount, onSuccess = std::move(onSuccess), epoch, program]
            (int exitCode, QProcess::ExitStatus exitStatus) mutable {
        const auto output = process->readAllStandardOutput();
        const auto errorOutput = process->readAllStandardError();
        const bool timedOut = process->property("motusProbeTimedOut").toBool();
        if (!finishCount() || epoch != projectEpoch_) return;
        if (timedOut) {
            setStatus(QStringLiteral("Probe timed out after 30 seconds; the project was not changed."));
            return;
        }
        if (exitStatus != QProcess::NormalExit || exitCode != 0) {
            const auto detail = boundedDiagnostic(errorOutput);
            setStatus(QStringLiteral("Probe failed (exit %1): %2")
                .arg(exitCode).arg(detail.isEmpty() ? QStringLiteral("no diagnostic output") : detail));
            return;
        }
        try {
            const auto result = ve::parseFfprobeJson(
                std::string_view(output.constData(), static_cast<std::size_t>(output.size())));
            onSuccess(result, QFileInfo(program).fileName());
        } catch (const std::exception& error) {
            setStatus(QStringLiteral("Probe result was unusable: %1")
                .arg(QString::fromUtf8(error.what())));
        }
    });
    QTimer::singleShot(30'000, process, [process, completed] {
        if (*completed || process->state() == QProcess::NotRunning) return;
        process->setProperty("motusProbeTimedOut", true);
        process->kill();
    });
    process->start();
}

void ProjectController::probeUnprobedAssets() {
    if (!project_) return;
    struct Pending { ve::Id id; std::filesystem::path path; };
    std::vector<Pending> pending;
    for (const auto& asset : project_->assets) {
        if (asset.status == ve::AssetStatus::Online && !asset.probe) {
            pending.push_back({asset.id, ve::resolveAssetPath(*project_, asset)});
        }
    }
    if (pending.empty()) return;
    for (const auto& item : pending) {
        runProbe(item.path, QStringLiteral("Probing legacy media metadata…"),
            [this, id = item.id, expectedPath = item.path]
            (const ve::MediaProbeResult& result, const QString& backend) {
                if (!project_) return;
                const auto* asset = project_->findAsset(id);
                if (!asset || ve::resolveAssetPath(*project_, *asset).lexically_normal() !=
                                  expectedPath.lexically_normal()) return;
                const auto name = QString::fromStdString(asset->displayName);
                auto replacement = *project_;
                ve::applyMediaProbe(replacement, id, result, false, backend.toStdString());
                ++replacement.revision;
                project_ = std::move(replacement);
                changed(true);
                setStatus(QStringLiteral("Media metadata probed and stored for %1.")
                    .arg(name));
            });
    }
}

void ProjectController::appendMediaReference(const QUrl& mediaPath, qint64 provisionalFrames) {
    (void)provisionalFrames;
    if (!project_) return;
    const auto path = localPath(mediaPath);
    runProbe(path, QStringLiteral("Probing media before it is added…"),
        [this, path](const ve::MediaProbeResult& result, const QString& backend) {
            if (!project_) return;
            try {
                auto replacement = *project_;
                const auto appended = ve::appendProbedMediaReference(
                    replacement, path, result, {}, backend.toStdString());
                history_.apply(*project_, ve::makeReplaceProjectCommand(
                    "Append probed media", std::move(replacement)));
                selectedClipId_ = !appended.videoClipId.empty()
                    ? std::optional<ve::Id>(appended.videoClipId)
                    : std::optional<ve::Id>(appended.audioClipId);
                changed();
                setStatus(QStringLiteral("Media probed, fingerprinted, and referenced read-only."));
            } catch (const std::exception& error) {
                setStatus(QStringLiteral("Import failed: %1").arg(QString::fromUtf8(error.what())));
            }
        });
}

void ProjectController::refreshMediaIntegrity() {
    if (!project_) return;
    try {
        const bool wasDirty = dirty_;
        const auto report = ve::refreshMediaIntegrity(*project_);
        changed(wasDirty || report.changed > 0U);
        setStatus(integrityStatus(QStringLiteral("Media integrity refreshed."), report));
        probeUnprobedAssets();
    } catch (const std::exception& error) {
        setStatus(QStringLiteral("Media refresh failed: %1").arg(QString::fromUtf8(error.what())));
    }
}

void ProjectController::relinkMedia(const QString& assetId, const QUrl& replacementPath) {
    if (!project_) return;
    try {
        const auto requestedId = assetId.toStdString();
        const auto* before = project_->findAsset(requestedId);
        if (!before) throw std::invalid_argument("asset does not exist");
        if (before->status != ve::AssetStatus::Missing &&
            before->status != ve::AssetStatus::Modified) {
            throw std::invalid_argument("only missing or modified media needs relinking");
        }
        const auto path = localPath(replacementPath);
        runProbe(path, QStringLiteral("Probing replacement media before relinking…"),
            [this, requestedId, path](const ve::MediaProbeResult& result,
                                      const QString& backend) {
                if (!project_) return;
                try {
                    auto replacement = *project_;
                    (void)ve::relinkAsset(replacement, requestedId, path);
                    ve::applyMediaProbe(replacement, requestedId, result, false,
                                        backend.toStdString());
                    const auto* relinked = replacement.findAsset(requestedId);
                    const auto name = relinked ? QString::fromStdString(relinked->displayName)
                                               : QStringLiteral("media");
                    history_.apply(*project_, ve::makeReplaceProjectCommand(
                        "Relink probed media", std::move(replacement)));
                    changed();
                    setStatus(QStringLiteral("Relinked and re-probed %1 without changing source media. Existing edit ranges were preserved.")
                        .arg(name));
                } catch (const std::exception& error) {
                    setStatus(QStringLiteral("Relink failed: %1")
                        .arg(QString::fromUtf8(error.what())));
                }
            });
    } catch (const std::exception& error) {
        setStatus(QStringLiteral("Relink failed: %1").arg(QString::fromUtf8(error.what())));
    }
}

void ProjectController::selectClip(const QString& clipId) {
    const auto requested = clipId.toStdString();
    const auto* sequence = activeSequence();
    if (!sequence) return;
    for (const auto& track : sequence->tracks) {
        if (std::ranges::find(track.clips, requested, &ve::Clip::id) != track.clips.end()) {
            selectedClipId_ = requested;
            emit workspaceChanged();
            return;
        }
    }
}

void ProjectController::clearSelection() {
    if (!selectedClipId_) return;
    selectedClipId_.reset();
    emit workspaceChanged();
}

void ProjectController::splitAtFrame(qint64 timelineFrame) {
    if (!project_ || !project_->activeSequenceId) return;
    const auto* sequence = project_->findSequence(*project_->activeSequenceId);
    if (!sequence) return;
    const auto position = ve::MediaTime::frames(timelineFrame, project_->profile.frameRateNumerator,
                                               project_->profile.frameRateDenominator);
    const ve::Clip* selected = selectedClip();
    if (!selected || position <= selected->timelineStart || position >= selected->timelineEnd()) {
        selected = nullptr;
        for (const auto& track : sequence->tracks) {
            if (track.kind != ve::TrackKind::Video) continue;
            const auto clip = std::ranges::find_if(track.clips, [&](const ve::Clip& candidate) {
                return candidate.timelineStart < position && candidate.timelineEnd() > position;
            });
            if (clip != track.clips.end()) { selected = &*clip; break; }
        }
    }
    if (!selected) {
        setStatus(QStringLiteral("No video clip crosses that frame."));
        return;
    }
    try {
        history_.apply(*project_, ve::makeSplitClipCommand(selected->id, position));
        changed();
        setStatus(QStringLiteral("Linked clip split."));
    } catch (const std::exception& error) {
        setStatus(QStringLiteral("Split failed: %1").arg(QString::fromUtf8(error.what())));
    }
}

void ProjectController::moveSelectedToFrame(qint64 timelineFrame) {
    const auto* clip = selectedClip();
    if (!clip || !project_) { setStatus(QStringLiteral("Select a clip before moving it.")); return; }
    try {
        history_.apply(*project_, ve::makeMoveClipCommand(clip->id,
            ve::MediaTime::frames(snappedFrame(timelineFrame), project_->profile.frameRateNumerator,
                                  project_->profile.frameRateDenominator)));
        changed();
        setStatus(QStringLiteral("Selected linked clip moved."));
    } catch (const std::exception& error) {
        setStatus(QStringLiteral("Move failed: %1").arg(QString::fromUtf8(error.what())));
    }
}

void ProjectController::trimSelectedStartToFrame(qint64 timelineFrame) {
    const auto* clip = selectedClip();
    if (!clip || !project_) { setStatus(QStringLiteral("Select a clip before trimming it.")); return; }
    try {
        history_.apply(*project_, ve::makeTrimClipStartCommand(clip->id,
            ve::MediaTime::frames(snappedFrame(timelineFrame), project_->profile.frameRateNumerator,
                                  project_->profile.frameRateDenominator)));
        changed();
        setStatus(QStringLiteral("Selected linked clip start trimmed."));
    } catch (const std::exception& error) {
        setStatus(QStringLiteral("Trim failed: %1").arg(QString::fromUtf8(error.what())));
    }
}

void ProjectController::trimSelectedEndToFrame(qint64 timelineFrame) {
    const auto* clip = selectedClip();
    if (!clip || !project_) { setStatus(QStringLiteral("Select a clip before trimming it.")); return; }
    try {
        history_.apply(*project_, ve::makeTrimClipEndCommand(clip->id,
            ve::MediaTime::frames(snappedFrame(timelineFrame), project_->profile.frameRateNumerator,
                                  project_->profile.frameRateDenominator)));
        changed();
        setStatus(QStringLiteral("Selected linked clip end trimmed."));
    } catch (const std::exception& error) {
        setStatus(QStringLiteral("Trim failed: %1").arg(QString::fromUtf8(error.what())));
    }
}

void ProjectController::removeSelectedClip() {
    const auto* clip = selectedClip();
    if (!clip || !project_ || !project_->activeSequenceId) {
        setStatus(QStringLiteral("Select a clip before removing it."));
        return;
    }
    const auto start = clip->timelineStart;
    const auto end = clip->timelineEnd();
    const auto linkedGroup = clip->linkedGroupId;
    const auto* sequence = activeSequence();
    if (!sequence) return;
    for (const auto& track : sequence->tracks) {
        if (!track.locked) continue;
        const bool containsSelectedLink = std::ranges::any_of(track.clips, [&](const ve::Clip& item) {
            return item.id == clip->id || (!linkedGroup.empty() && item.linkedGroupId == linkedGroup &&
                item.timelineStart == clip->timelineStart && item.duration == clip->duration);
        });
        if (containsSelectedLink) {
            setStatus(QStringLiteral("Unlock the selected clip's linked tracks before removing it."));
            return;
        }
    }
    try {
        history_.apply(*project_, ve::makeRippleDeleteCommand(*project_->activeSequenceId, start, end));
        selectedClipId_.reset();
        changed();
        setStatus(QStringLiteral("Selected range removed and the rough cut closed."));
    } catch (const std::exception& error) {
        setStatus(QStringLiteral("Remove failed: %1").arg(QString::fromUtf8(error.what())));
    }
}

bool ProjectController::rippleDelete(qint64 startFrame, qint64 endFrame) {
    if (!project_ || !project_->activeSequenceId) return false;
    try {
        history_.apply(*project_, ve::makeRippleDeleteCommand(*project_->activeSequenceId,
            ve::MediaTime::frames(startFrame, project_->profile.frameRateNumerator,
                                  project_->profile.frameRateDenominator),
            ve::MediaTime::frames(endFrame, project_->profile.frameRateNumerator,
                                  project_->profile.frameRateDenominator)));
        changed();
        setStatus(QStringLiteral("Range ripple-deleted across unlocked tracks."));
        return true;
    } catch (const std::exception& error) {
        setStatus(QStringLiteral("Ripple delete failed: %1").arg(QString::fromUtf8(error.what())));
        return false;
    }
}

void ProjectController::rippleDeleteInOut() {
    if (!hasInOutRange()) {
        setStatus(QStringLiteral("Set both an in point and an out point first."));
        return;
    }
    if (rippleDelete(inPointFrame_, outPointFrame_)) clearInOut();
}

void ProjectController::setInPoint(qint64 timelineFrame) {
    if (!project_) return;
    inPointFrame_ = std::clamp(snappedFrame(timelineFrame), qint64{0}, durationFrames());
    if (outPointFrame_ >= 0 && outPointFrame_ <= inPointFrame_) outPointFrame_ = -1;
    emit workspaceChanged();
    setStatus(QStringLiteral("In point set at frame %1.").arg(inPointFrame_));
}

void ProjectController::setOutPoint(qint64 timelineFrame) {
    if (!project_) return;
    outPointFrame_ = std::clamp(snappedFrame(timelineFrame), qint64{0}, durationFrames());
    if (inPointFrame_ < 0) inPointFrame_ = 0;
    if (outPointFrame_ <= inPointFrame_) {
        outPointFrame_ = -1;
        setStatus(QStringLiteral("Out point must be after the in point."));
    } else {
        setStatus(QStringLiteral("Out point set at frame %1.").arg(outPointFrame_));
    }
    emit workspaceChanged();
}

void ProjectController::clearInOut() {
    if (!hasInPoint() && !hasOutPoint()) return;
    inPointFrame_ = -1;
    outPointFrame_ = -1;
    emit workspaceChanged();
    setStatus(QStringLiteral("In and out points cleared."));
}

qint64 ProjectController::snappedFrame(qint64 timelineFrame) const {
    const auto bounded = std::clamp(timelineFrame, qint64{0}, durationFrames());
    if (!snapEnabled_ || !project_) return bounded;
    const auto threshold = std::max<qint64>(1, static_cast<qint64>(
        std::llround(10.0 / std::max(0.1, timelineZoom_))));
    const auto* sequence = activeSequence();
    if (!sequence) return bounded;
    return static_cast<qint64>(ve::snapTimelineFrame(*project_, *sequence, bounded, threshold));
}

void ProjectController::addMarker(qint64 timelineFrame, const QString& label) {
    if (!project_ || !project_->activeSequenceId) return;
    try {
        auto replacement = *project_;
        auto* sequence = replacement.findSequence(*replacement.activeSequenceId);
        if (!sequence) throw std::runtime_error("active sequence does not exist");
        const auto cleanLabel = label.trimmed().isEmpty()
            ? QStringLiteral("Marker %1").arg(sequence->markers.size() + 1) : label.trimmed();
        sequence->markers.push_back({ve::makeId(),
            ve::MediaTime::frames(std::clamp(timelineFrame, qint64{0}, durationFrames()),
                                  replacement.profile.frameRateNumerator,
                                  replacement.profile.frameRateDenominator),
            cleanLabel.toStdString(), "#62d3e8"});
        history_.apply(*project_, ve::makeReplaceProjectCommand("Add marker", std::move(replacement)));
        changed();
        setStatus(QStringLiteral("Marker added."));
    } catch (const std::exception& error) {
        setStatus(QStringLiteral("Marker failed: %1").arg(QString::fromUtf8(error.what())));
    }
}

void ProjectController::removeMarker(const QString& markerId) {
    if (!project_ || !project_->activeSequenceId) return;
    try {
        auto replacement = *project_;
        auto* sequence = replacement.findSequence(*replacement.activeSequenceId);
        if (!sequence) throw std::runtime_error("active sequence does not exist");
        const auto count = sequence->markers.size();
        std::erase_if(sequence->markers, [&](const ve::Marker& marker) {
            return marker.id == markerId.toStdString();
        });
        if (sequence->markers.size() == count) throw std::invalid_argument("marker does not exist");
        history_.apply(*project_, ve::makeReplaceProjectCommand("Remove marker", std::move(replacement)));
        changed();
        setStatus(QStringLiteral("Marker removed."));
    } catch (const std::exception& error) {
        setStatus(QStringLiteral("Remove marker failed: %1").arg(QString::fromUtf8(error.what())));
    }
}

void ProjectController::setTrackLocked(const QString& trackId, bool locked) {
    if (!project_) return;
    try {
        auto replacement = *project_;
        auto* track = replacement.findTrack(trackId.toStdString());
        if (!track) throw std::invalid_argument("track does not exist");
        track->locked = locked;
        history_.apply(*project_, ve::makeReplaceProjectCommand("Change track lock", std::move(replacement)));
        changed();
        setStatus(locked ? QStringLiteral("Track locked.") : QStringLiteral("Track unlocked."));
    } catch (const std::exception& error) {
        setStatus(QStringLiteral("Track change failed: %1").arg(QString::fromUtf8(error.what())));
    }
}

void ProjectController::setTrackMuted(const QString& trackId, bool muted) {
    if (!project_) return;
    try {
        auto replacement = *project_;
        auto* track = replacement.findTrack(trackId.toStdString());
        if (!track) throw std::invalid_argument("track does not exist");
        track->muted = muted;
        history_.apply(*project_, ve::makeReplaceProjectCommand("Change track mute", std::move(replacement)));
        changed();
        setStatus(muted ? QStringLiteral("Track muted.") : QStringLiteral("Track unmuted."));
    } catch (const std::exception& error) {
        setStatus(QStringLiteral("Track change failed: %1").arg(QString::fromUtf8(error.what())));
    }
}

void ProjectController::setTrackVisible(const QString& trackId, bool visible) {
    if (!project_) return;
    try {
        auto replacement = *project_;
        auto* track = replacement.findTrack(trackId.toStdString());
        if (!track) throw std::invalid_argument("track does not exist");
        track->visible = visible;
        history_.apply(*project_, ve::makeReplaceProjectCommand("Change track visibility", std::move(replacement)));
        changed();
        setStatus(visible ? QStringLiteral("Video track shown.") : QStringLiteral("Video track hidden."));
    } catch (const std::exception& error) {
        setStatus(QStringLiteral("Track change failed: %1").arg(QString::fromUtf8(error.what())));
    }
}

void ProjectController::setSnapEnabled(bool enabled) {
    if (snapEnabled_ == enabled) return;
    snapEnabled_ = enabled;
    emit workspaceChanged();
}

void ProjectController::setTimelineZoom(double zoom) {
    const auto bounded = std::clamp(zoom, 0.35, 4.0);
    if (std::abs(timelineZoom_ - bounded) < 0.001) return;
    timelineZoom_ = bounded;
    emit workspaceChanged();
}

void ProjectController::zoomIn() { setTimelineZoom(timelineZoom_ * 1.25); }
void ProjectController::zoomOut() { setTimelineZoom(timelineZoom_ / 1.25); }
void ProjectController::resetZoom() { setTimelineZoom(1.0); }

void ProjectController::invalidatePreview() {
    if (!mediaPlayer_) return;
    mediaPlayer_->stop();
    mediaPlayer_->setSource({});
    previewPlan_.reset();
    previewSegmentIndex_.reset();
    previewSeekPending_ = false;
    previewPlayAfterSeek_ = false;
    if (!project_ || !project_->activeSequenceId) {
        previewDetail_ = QStringLiteral("Import probed media to enable clip preview.");
        emit mediaStateChanged();
        return;
    }
    const auto* sequence = activeSequence();
    try {
        if (!sequence) throw std::runtime_error("active sequence does not exist");
        previewPlan_ = ve::buildSimpleTimelinePlan(*project_, *sequence);
        audioOutput_->setMuted(!previewPlan_->hasAudio);
        previewDetail_ = QStringLiteral("Selected-clip decode preview · time-based Qt seek");
    } catch (const std::exception& error) {
        previewDetail_ = QStringLiteral("Preview unavailable: %1")
            .arg(QString::fromUtf8(error.what()));
    }
    emit mediaStateChanged();
}

bool ProjectController::configurePreview(qint64 timelineFrame, bool playAfterSeek) {
    if (!previewPlan_) {
        invalidatePreview();
        if (!previewPlan_) return false;
    }
    const auto timelineUs = ve::MediaTime::frames(
        timelineFrame, project_->profile.frameRateNumerator,
        project_->profile.frameRateDenominator).rescaled(1'000'000, 1).units;
    const auto iterator = std::ranges::find_if(previewPlan_->segments,
        [timelineUs](const ve::SimpleMediaSegment& segment) {
            return timelineUs >= segment.timelineStartUs &&
                   timelineUs < segment.timelineStartUs + segment.durationUs;
        });
    if (iterator == previewPlan_->segments.end()) {
        mediaPlayer_->pause();
        previewSeekPending_ = false;
        previewSegmentIndex_.reset();
        previewDetail_ = QStringLiteral("The playhead is outside the previewable rough cut.");
        emit mediaStateChanged();
        return false;
    }
    const auto index = static_cast<std::size_t>(std::distance(previewPlan_->segments.begin(), iterator));
    const auto sourcePositionUs = iterator->sourceInUs + timelineUs - iterator->timelineStartUs;
    pendingPreviewPositionMs_ = std::max<qint64>(0, sourcePositionUs / 1000);
    previewSeekPending_ = true;
    previewPlayAfterSeek_ = playAfterSeek;
    previewSegmentIndex_ = index;
    const auto sourceUrl = QUrl::fromLocalFile(displayPath(iterator->sourcePath));
    if (mediaPlayer_->source() != sourceUrl) {
        mediaPlayer_->setSource(sourceUrl);
    } else if (mediaPlayer_->mediaStatus() != QMediaPlayer::NoMedia &&
               mediaPlayer_->mediaStatus() != QMediaPlayer::LoadingMedia &&
               mediaPlayer_->mediaStatus() != QMediaPlayer::InvalidMedia) {
        finishPreviewSeek();
    }
    return true;
}

void ProjectController::finishPreviewSeek() {
    if (!mediaPlayer_ || !previewSegmentIndex_ || !previewSeekPending_) return;
    // BufferedMedia can be emitted repeatedly while playback is active. Mark this request as
    // complete before seeking so those transitions cannot rewind the player to the same frame.
    previewSeekPending_ = false;
    mediaPlayer_->setPosition(pendingPreviewPositionMs_);
    if (previewPlayAfterSeek_) mediaPlayer_->play();
    else mediaPlayer_->pause();
    previewDetail_ = previewPlayAfterSeek_
        ? QStringLiteral("Playing selected-clip preview · edits/export remain frame-based")
        : QStringLiteral("Selected-clip preview parked at the edit playhead");
    emit mediaStateChanged();
}

void ProjectController::advancePreviewSegment() {
    if (!previewPlan_ || !previewSegmentIndex_) return;
    const auto next = *previewSegmentIndex_ + 1U;
    if (next >= previewPlan_->segments.size()) {
        mediaPlayer_->pause();
        previewSeekPending_ = false;
        previewPlayAfterSeek_ = false;
        updatingPlayheadFromPreview_ = true;
        playheadFrame_ = durationFrames();
        updatingPlayheadFromPreview_ = false;
        previewSegmentIndex_.reset();
        previewDetail_ = QStringLiteral("Preview reached the end of the rough cut.");
        emit workspaceChanged();
        emit mediaStateChanged();
        return;
    }
    const auto frame = frameForMicroseconds(previewPlan_->segments[next].timelineStartUs);
    updatingPlayheadFromPreview_ = true;
    playheadFrame_ = frame;
    updatingPlayheadFromPreview_ = false;
    emit workspaceChanged();
    (void)configurePreview(frame, true);
}

void ProjectController::setVideoOutput(QObject* output) {
    if (mediaPlayer_) mediaPlayer_->setVideoOutput(output);
}

void ProjectController::togglePlayback() {
    if (!project_) return;
    if (playing()) {
        pausePlayback();
        return;
    }
    auto start = playheadFrame_;
    if (start >= durationFrames()) start = 0;
    if (!configurePreview(start, true)) setStatus(previewDetail_);
}

void ProjectController::pausePlayback() {
    if (!mediaPlayer_) return;
    mediaPlayer_->pause();
    previewPlayAfterSeek_ = false;
    previewDetail_ = QStringLiteral("Selected-clip preview paused.");
    emit mediaStateChanged();
}

void ProjectController::setPlayheadFrame(qint64 timelineFrame) {
    const auto bounded = std::clamp(timelineFrame, qint64{0}, durationFrames());
    if (playheadFrame_ == bounded) return;
    playheadFrame_ = bounded;
    emit workspaceChanged();
    if (!updatingPlayheadFromPreview_ && previewPlan_) {
        (void)configurePreview(playheadFrame_, playing());
    }
}

void ProjectController::stepPlayhead(qint64 frameDelta) {
    setPlayheadFrame(playheadFrame_ + frameDelta);
}

QString ProjectController::formatTimecode(qint64 timelineFrame) const {
    const auto frame = std::max<qint64>(0, timelineFrame);
    const auto nominalRate = project_ ? std::max<qint64>(1, static_cast<qint64>(std::llround(
        static_cast<double>(project_->profile.frameRateNumerator) /
        static_cast<double>(project_->profile.frameRateDenominator)))) : qint64{30};
    const auto frames = frame % nominalRate;
    const auto totalSeconds = frame / nominalRate;
    const auto seconds = totalSeconds % 60;
    const auto totalMinutes = totalSeconds / 60;
    const auto minutes = totalMinutes % 60;
    const auto hours = totalMinutes / 60;
    return QStringLiteral("%1:%2:%3:%4")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'))
        .arg(frames, 2, 10, QLatin1Char('0'));
}

QString ProjectController::playheadTimecode() const {
    return formatTimecode(playheadFrame_);
}

void ProjectController::undo() {
    if (!project_ || !history_.canUndo()) return;
    history_.undo(*project_); changed(); setStatus(QStringLiteral("Edit undone."));
}
void ProjectController::redo() {
    if (!project_ || !history_.canRedo()) return;
    history_.redo(*project_); changed(); setStatus(QStringLiteral("Edit redone."));
}

void ProjectController::generateMltGraph(const QUrl& outputPath) {
    if (!project_ || !project_->activeSequenceId) return;
    try {
        const auto* sequence = project_->findSequence(*project_->activeSequenceId);
        if (!sequence) throw std::runtime_error("active sequence does not exist");
        const auto graph = ve::buildMltGraph(*project_, *sequence,
                                             {ve::GraphPurpose::Render, {}});
        const auto path = localPath(outputPath);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << graph.xml;
        if (!output) throw std::runtime_error("could not write MLT graph");
        setStatus(QStringLiteral("MLT diagnostic graph written: %1").arg(displayPath(path)));
    } catch (const std::exception& error) {
        setStatus(QStringLiteral("Graph generation failed: %1").arg(QString::fromUtf8(error.what())));
    }
}

void ProjectController::exportSequence(const QUrl& outputPath) {
    if (!project_ || !project_->activeSequenceId) return;
    if (exportProcess_) {
        setStatus(QStringLiteral("An export is already running."));
        return;
    }
    const auto ffmpeg = mediaToolPath(QStringLiteral("ffmpeg"));
    if (ffmpeg.isEmpty()) {
        setStatus(QStringLiteral("FFmpeg is unavailable. Rebuild the Motus bundle or install FFmpeg for this source build."));
        return;
    }
    try {
        auto snapshot = *project_;
        const auto integrity = ve::refreshMediaIntegrity(snapshot);
        if (integrity.changed > 0U) {
            project_ = snapshot;
            changed(true);
        }
        const auto* sequence = snapshot.findSequence(*snapshot.activeSequenceId);
        if (!sequence) throw std::runtime_error("active sequence does not exist");
        const auto plan = ve::buildSimpleTimelinePlan(snapshot, *sequence);
        auto output = localPath(outputPath);
        if (output.extension().empty()) output += ".mp4";
        if (output.extension() != ".mp4") {
            throw std::invalid_argument("the first native export preset writes .mp4 files only");
        }
        std::error_code pathError;
        const auto absoluteOutput = std::filesystem::absolute(output, pathError).lexically_normal();
        if (pathError) throw std::runtime_error("cannot resolve export path: " + pathError.message());
        const auto staged = ve::stagedExportPath(absoluteOutput);
        for (const auto& segment : plan.segments) {
            const auto absoluteSource = std::filesystem::absolute(segment.sourcePath, pathError)
                                            .lexically_normal();
            if (pathError) throw std::runtime_error("cannot resolve source path: " + pathError.message());
            if (sameFilesystemPath(absoluteSource, absoluteOutput)) {
                throw std::invalid_argument("export destination must not overwrite source media");
            }
            if (sameFilesystemPath(absoluteSource, staged)) {
                throw std::invalid_argument(
                    "the export staging path would overwrite source media; choose another destination name");
            }
        }
        const auto parent = absoluteOutput.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        std::error_code removeError;
        std::filesystem::remove(staged, removeError);
        if (removeError) {
            throw std::runtime_error("cannot clear stale partial export: " + removeError.message());
        }

        const auto arguments = ve::buildFfmpegExportArguments(plan, staged);
        QStringList qtArguments;
        qtArguments.reserve(static_cast<qsizetype>(arguments.size()));
        for (const auto& argument : arguments) qtArguments.push_back(QString::fromUtf8(argument));

        exportOutputPath_ = absoluteOutput;
        exportStagedPath_ = staged;
        exportDurationUs_ = plan.durationUs;
        exportProgress_ = 0.0;
        exportCancelRequested_ = false;
        exportProgressBuffer_.clear();
        exportErrorBuffer_.clear();
        exportDetail_ = QStringLiteral("Rendering a snapshot from original media…");
        exportProcess_ = new QProcess(this);
        exportProcess_->setProgram(ffmpeg);
        exportProcess_->setArguments(qtArguments);
        exportProcess_->setProcessChannelMode(QProcess::SeparateChannels);
        auto* const process = exportProcess_;
        connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
            if (process != exportProcess_) return;
            exportProgressBuffer_.append(process->readAllStandardOutput());
            processExportProgress();
        });
        connect(process, &QProcess::readyReadStandardError, this, [this, process] {
            if (process != exportProcess_) return;
            exportErrorBuffer_.append(process->readAllStandardError());
            if (exportErrorBuffer_.size() > 32 * 1024) exportErrorBuffer_ = exportErrorBuffer_.right(32 * 1024);
        });
        connect(process, &QProcess::errorOccurred, this,
                [this, process](QProcess::ProcessError error) {
            if (process != exportProcess_ || error != QProcess::FailedToStart) return;
            exportErrorBuffer_.append(process->errorString().toUtf8());
            finishExport(-1, true);
        });
        connect(process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
                [this, process](int exitCode, QProcess::ExitStatus status) {
            if (process != exportProcess_) return;
            finishExport(exitCode, status != QProcess::NormalExit);
        });
        process->start();
        emit mediaStateChanged();
        setStatus(QStringLiteral("Export started from an immutable rough-cut snapshot."));
    } catch (const std::exception& error) {
        setStatus(QStringLiteral("Export preflight failed: %1").arg(QString::fromUtf8(error.what())));
    }
}

void ProjectController::processExportProgress() {
    while (true) {
        const auto newline = exportProgressBuffer_.indexOf('\n');
        if (newline < 0) break;
        const auto line = exportProgressBuffer_.left(newline + 1);
        exportProgressBuffer_.remove(0, newline + 1);
        const auto progress = ve::parseFfmpegProgressMicroseconds(
            std::string_view(line.constData(), static_cast<std::size_t>(line.size())));
        if (!progress || exportDurationUs_ <= 0) continue;
        exportProgress_ = std::clamp(static_cast<double>(*progress) /
                                        static_cast<double>(exportDurationUs_), 0.0, 1.0);
        exportDetail_ = QStringLiteral("Rendering originals · %1%").arg(
            static_cast<int>(std::lround(exportProgress_ * 100.0)));
        emit mediaStateChanged();
    }
}

void ProjectController::finishExport(int exitCode, bool crashed) {
    if (!exportProcess_) return;
    auto* const process = exportProcess_;
    exportKillTimer_.stop();
    exportProgressBuffer_.append(process->readAllStandardOutput());
    exportErrorBuffer_.append(process->readAllStandardError());
    processExportProgress();
    exportProcess_ = nullptr;
    process->deleteLater();

    if (exportCancelRequested_) {
        std::error_code ignored;
        std::filesystem::remove(exportStagedPath_, ignored);
        exportProgress_ = 0.0;
        exportDetail_ = QStringLiteral("Export cancelled; partial output removed.");
        setStatus(exportDetail_);
        emit mediaStateChanged();
        return;
    }
    if (crashed || exitCode != 0) {
        std::error_code ignored;
        std::filesystem::remove(exportStagedPath_, ignored);
        const auto diagnostic = boundedDiagnostic(exportErrorBuffer_);
        exportProgress_ = 0.0;
        exportDetail_ = QStringLiteral("Export failed (exit %1): %2")
            .arg(exitCode).arg(diagnostic.isEmpty() ? QStringLiteral("no FFmpeg diagnostic") : diagnostic);
        setStatus(exportDetail_);
        emit mediaStateChanged();
        return;
    }
    try {
        if (!std::filesystem::is_regular_file(exportStagedPath_)) {
            throw std::runtime_error("FFmpeg exited successfully but produced no staged output");
        }
        promoteStagedOutput(exportStagedPath_, exportOutputPath_);
        exportProgress_ = 1.0;
        exportDetail_ = QStringLiteral("Export complete: %1").arg(displayPath(exportOutputPath_));
        setStatus(exportDetail_);
    } catch (const std::exception& error) {
        exportProgress_ = 0.0;
        exportDetail_ = QStringLiteral("Export rendered but could not be published: %1. The staged file remains at %2")
            .arg(QString::fromUtf8(error.what()), displayPath(exportStagedPath_));
        setStatus(exportDetail_);
    }
    emit mediaStateChanged();
}

void ProjectController::cancelExport() {
    if (!exportProcess_) return;
    exportCancelRequested_ = true;
    exportDetail_ = QStringLiteral("Cancelling export…");
    setStatus(exportDetail_);
    exportProcess_->terminate();
    exportKillTimer_.start();
    emit mediaStateChanged();
}
