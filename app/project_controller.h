#pragma once

#include "ve/commands.h"
#include "ve/native_media.h"
#include "ve/project.h"

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include <optional>
#include <functional>
#include <memory>

class QAudioOutput;
class QMediaPlayer;
class QProcess;

class ProjectController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hasProject READ hasProject NOTIFY projectChanged)
    Q_PROPERTY(bool dirty READ dirty NOTIFY projectChanged)
    Q_PROPERTY(QString projectName READ projectName NOTIFY projectChanged)
    Q_PROPERTY(QString projectPath READ projectPath NOTIFY projectChanged)
    Q_PROPERTY(QString sequenceName READ sequenceName NOTIFY projectChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QStringList assetNames READ assetNames NOTIFY projectChanged)
    Q_PROPERTY(QVariantList assetItems READ assetItems NOTIFY projectChanged)
    Q_PROPERTY(QVariantList timelineClips READ timelineClips NOTIFY projectChanged)
    Q_PROPERTY(QVariantList trackItems READ trackItems NOTIFY projectChanged)
    Q_PROPERTY(QVariantList markerItems READ markerItems NOTIFY projectChanged)
    Q_PROPERTY(qint64 durationFrames READ durationFrames NOTIFY projectChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY projectChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY projectChanged)
    Q_PROPERTY(bool hasSelectedClip READ hasSelectedClip NOTIFY workspaceChanged)
    Q_PROPERTY(QString selectedClipId READ selectedClipId NOTIFY workspaceChanged)
    Q_PROPERTY(QString selectedLinkedGroupId READ selectedLinkedGroupId NOTIFY workspaceChanged)
    Q_PROPERTY(QString selectedClipName READ selectedClipName NOTIFY workspaceChanged)
    Q_PROPERTY(QString selectedClipKind READ selectedClipKind NOTIFY workspaceChanged)
    Q_PROPERTY(qint64 selectedClipStartFrame READ selectedClipStartFrame NOTIFY workspaceChanged)
    Q_PROPERTY(qint64 selectedClipEndFrame READ selectedClipEndFrame NOTIFY workspaceChanged)
    Q_PROPERTY(qint64 selectedClipDurationFrames READ selectedClipDurationFrames NOTIFY workspaceChanged)
    Q_PROPERTY(bool hasInPoint READ hasInPoint NOTIFY workspaceChanged)
    Q_PROPERTY(bool hasOutPoint READ hasOutPoint NOTIFY workspaceChanged)
    Q_PROPERTY(bool hasInOutRange READ hasInOutRange NOTIFY workspaceChanged)
    Q_PROPERTY(qint64 inPointFrame READ inPointFrame NOTIFY workspaceChanged)
    Q_PROPERTY(qint64 outPointFrame READ outPointFrame NOTIFY workspaceChanged)
    Q_PROPERTY(bool snapEnabled READ snapEnabled WRITE setSnapEnabled NOTIFY workspaceChanged)
    Q_PROPERTY(double timelineZoom READ timelineZoom WRITE setTimelineZoom NOTIFY workspaceChanged)
    Q_PROPERTY(qint64 playheadFrame READ playheadFrame WRITE setPlayheadFrame NOTIFY workspaceChanged)
    Q_PROPERTY(QString playheadTimecode READ playheadTimecode NOTIFY workspaceChanged)
    Q_PROPERTY(bool probing READ probing NOTIFY mediaStateChanged)
    Q_PROPERTY(int activeProbeCount READ activeProbeCount NOTIFY mediaStateChanged)
    Q_PROPERTY(bool mediaToolsAvailable READ mediaToolsAvailable NOTIFY mediaStateChanged)
    Q_PROPERTY(bool canPreview READ canPreview NOTIFY mediaStateChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY mediaStateChanged)
    Q_PROPERTY(QString previewDetail READ previewDetail NOTIFY mediaStateChanged)
    Q_PROPERTY(bool exporting READ exporting NOTIFY mediaStateChanged)
    Q_PROPERTY(double exportProgress READ exportProgress NOTIFY mediaStateChanged)
    Q_PROPERTY(QString exportDetail READ exportDetail NOTIFY mediaStateChanged)

public:
    explicit ProjectController(QObject* parent = nullptr);

    [[nodiscard]] bool hasProject() const { return project_.has_value(); }
    [[nodiscard]] bool dirty() const { return dirty_; }
    [[nodiscard]] QString projectName() const;
    [[nodiscard]] QString projectPath() const;
    [[nodiscard]] QString sequenceName() const;
    [[nodiscard]] QString status() const { return status_; }
    [[nodiscard]] QStringList assetNames() const;
    [[nodiscard]] QVariantList assetItems() const;
    [[nodiscard]] QVariantList timelineClips() const;
    [[nodiscard]] QVariantList trackItems() const;
    [[nodiscard]] QVariantList markerItems() const;
    [[nodiscard]] qint64 durationFrames() const;
    [[nodiscard]] bool canUndo() const { return history_.canUndo(); }
    [[nodiscard]] bool canRedo() const { return history_.canRedo(); }
    [[nodiscard]] bool hasSelectedClip() const;
    [[nodiscard]] QString selectedClipId() const;
    [[nodiscard]] QString selectedLinkedGroupId() const;
    [[nodiscard]] QString selectedClipName() const;
    [[nodiscard]] QString selectedClipKind() const;
    [[nodiscard]] qint64 selectedClipStartFrame() const;
    [[nodiscard]] qint64 selectedClipEndFrame() const;
    [[nodiscard]] qint64 selectedClipDurationFrames() const;
    [[nodiscard]] bool hasInPoint() const { return inPointFrame_ >= 0; }
    [[nodiscard]] bool hasOutPoint() const { return outPointFrame_ >= 0; }
    [[nodiscard]] bool hasInOutRange() const {
        return hasInPoint() && hasOutPoint() && outPointFrame_ > inPointFrame_;
    }
    [[nodiscard]] qint64 inPointFrame() const { return inPointFrame_; }
    [[nodiscard]] qint64 outPointFrame() const { return outPointFrame_; }
    [[nodiscard]] bool snapEnabled() const { return snapEnabled_; }
    [[nodiscard]] double timelineZoom() const { return timelineZoom_; }
    [[nodiscard]] qint64 playheadFrame() const { return playheadFrame_; }
    [[nodiscard]] QString playheadTimecode() const;
    [[nodiscard]] bool probing() const { return activeProbeCount_ > 0; }
    [[nodiscard]] int activeProbeCount() const { return activeProbeCount_; }
    [[nodiscard]] bool mediaToolsAvailable() const;
    [[nodiscard]] bool canPreview() const;
    [[nodiscard]] bool playing() const;
    [[nodiscard]] QString previewDetail() const { return previewDetail_; }
    [[nodiscard]] bool exporting() const { return exportProcess_ != nullptr; }
    [[nodiscard]] double exportProgress() const { return exportProgress_; }
    [[nodiscard]] QString exportDetail() const { return exportDetail_; }

    Q_INVOKABLE void newProject(const QString& name = QStringLiteral("Untitled"));
    Q_INVOKABLE void openProject(const QUrl& path);
    Q_INVOKABLE void save();
    Q_INVOKABLE void saveAs(const QUrl& path);
    Q_INVOKABLE void appendMediaReference(const QUrl& mediaPath, qint64 provisionalFrames = 300);
    Q_INVOKABLE void refreshMediaIntegrity();
    Q_INVOKABLE void relinkMedia(const QString& assetId, const QUrl& replacementPath);
    Q_INVOKABLE void selectClip(const QString& clipId);
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE void splitAtFrame(qint64 timelineFrame);
    Q_INVOKABLE void moveSelectedToFrame(qint64 timelineFrame);
    Q_INVOKABLE void trimSelectedStartToFrame(qint64 timelineFrame);
    Q_INVOKABLE void trimSelectedEndToFrame(qint64 timelineFrame);
    Q_INVOKABLE void removeSelectedClip();
    Q_INVOKABLE bool rippleDelete(qint64 startFrame, qint64 endFrame);
    Q_INVOKABLE void rippleDeleteInOut();
    Q_INVOKABLE void setInPoint(qint64 timelineFrame);
    Q_INVOKABLE void setOutPoint(qint64 timelineFrame);
    Q_INVOKABLE void clearInOut();
    Q_INVOKABLE qint64 snappedFrame(qint64 timelineFrame) const;
    Q_INVOKABLE void addMarker(qint64 timelineFrame, const QString& label);
    Q_INVOKABLE void removeMarker(const QString& markerId);
    Q_INVOKABLE void setTrackLocked(const QString& trackId, bool locked);
    Q_INVOKABLE void setTrackMuted(const QString& trackId, bool muted);
    Q_INVOKABLE void setTrackVisible(const QString& trackId, bool visible);
    Q_INVOKABLE void zoomIn();
    Q_INVOKABLE void zoomOut();
    Q_INVOKABLE void resetZoom();
    Q_INVOKABLE void stepPlayhead(qint64 frameDelta);
    Q_INVOKABLE QString formatTimecode(qint64 timelineFrame) const;
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE void generateMltGraph(const QUrl& outputPath);
    Q_INVOKABLE void setVideoOutput(QObject* output);
    Q_INVOKABLE void togglePlayback();
    Q_INVOKABLE void pausePlayback();
    Q_INVOKABLE void exportSequence(const QUrl& outputPath);
    Q_INVOKABLE void cancelExport();

    void setSnapEnabled(bool enabled);
    void setTimelineZoom(double zoom);
    void setPlayheadFrame(qint64 timelineFrame);

signals:
    void projectChanged();
    void statusChanged();
    void workspaceChanged();
    void mediaStateChanged();
    void savePathRequired();

private:
    void setStatus(QString status);
    void changed(bool dirty = true);
    void resetWorkspaceState();
    [[nodiscard]] ve::Project& project();
    [[nodiscard]] const ve::Project& project() const;
    [[nodiscard]] const ve::Sequence* activeSequence() const;
    [[nodiscard]] const ve::Clip* selectedClip() const;
    [[nodiscard]] const ve::Track* selectedTrack() const;
    [[nodiscard]] qint64 frameFor(const ve::MediaTime& time) const;
    [[nodiscard]] qint64 frameForMicroseconds(std::int64_t microseconds) const;
    [[nodiscard]] QString mediaToolPath(const QString& baseName) const;
    void runProbe(const std::filesystem::path& path, QString activity,
                  std::function<void(const ve::MediaProbeResult&, const QString&)> onSuccess);
    void probeUnprobedAssets();
    void invalidatePreview();
    bool configurePreview(qint64 timelineFrame, bool playAfterSeek);
    void finishPreviewSeek();
    void advancePreviewSegment();
    void processExportProgress();
    void finishExport(int exitCode, bool crashed);

    std::optional<ve::Project> project_;
    ve::UndoStack history_;
    bool dirty_{false};
    QString status_{QStringLiteral("Create or open a Motus project.")};
    QTimer recoveryTimer_;
    std::optional<ve::Id> selectedClipId_;
    qint64 inPointFrame_{-1};
    qint64 outPointFrame_{-1};
    bool snapEnabled_{true};
    double timelineZoom_{1.0};
    qint64 playheadFrame_{0};
    QMediaPlayer* mediaPlayer_{nullptr};
    QAudioOutput* audioOutput_{nullptr};
    std::optional<ve::SimpleTimelinePlan> previewPlan_;
    std::optional<std::size_t> previewSegmentIndex_;
    qint64 pendingPreviewPositionMs_{0};
    bool previewSeekPending_{false};
    bool previewPlayAfterSeek_{false};
    bool updatingPlayheadFromPreview_{false};
    QString previewDetail_{QStringLiteral("Import probed media to enable clip preview.")};
    int activeProbeCount_{0};
    quint64 projectEpoch_{0};
    QProcess* exportProcess_{nullptr};
    QTimer exportKillTimer_;
    QByteArray exportProgressBuffer_;
    QByteArray exportErrorBuffer_;
    std::filesystem::path exportOutputPath_;
    std::filesystem::path exportStagedPath_;
    std::int64_t exportDurationUs_{0};
    double exportProgress_{0.0};
    bool exportCancelRequested_{false};
    QString exportDetail_;
};
