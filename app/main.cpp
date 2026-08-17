#include "project_controller.h"

#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickStyle>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string_view>

// Proves that this copy of Motus can actually open on this computer before
// Instrumenta shows it as ready. Loading the executable is not enough: the Qt
// platform plugin and the QML modules are opened later through the plugin and
// import trees, so a bundle can start and still fail to draw a window. This
// initialises the real GUI stack and compiles the root QML component, then
// exits before any window is created.
static int runLaunchCheck(int argc, char* argv[], const char* markerPath) {
    QGuiApplication application(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Fusion"));

    QQmlEngine engine;
    QQmlComponent component(&engine, "Motus", "Main");
    if (component.isError()) {
        for (const QQmlError& error : component.errors()) {
            std::fputs(qUtf8Printable(error.toString()), stderr);
            std::fputs("\n", stderr);
        }
        return 1;
    }

    std::ofstream marker(markerPath, std::ios::trunc);
    if (!marker) return 1;
    marker << "MOTUS_LAUNCH_OK " << MOTUS_VERSION << '\n';
    return marker ? 0 : 1;
}

static int visibleIconPixels(QQuickWindow* window, const QImage& image,
                             const QString& iconName) {
    const auto icons = window->findChildren<QQuickItem*>(QStringLiteral("motus-icon-") + iconName);
    int best = 0;
    const auto ratio = image.devicePixelRatio();
    for (const auto* icon : icons) {
        if (!icon->isVisible() || icon->width() <= 0 || icon->height() <= 0) continue;
        const auto origin = icon->mapToScene(QPointF(0, 0));
        const QRect bounds(
            static_cast<int>(std::floor(origin.x() * ratio)),
            static_cast<int>(std::floor(origin.y() * ratio)),
            static_cast<int>(std::ceil(icon->width() * ratio)),
            static_cast<int>(std::ceil(icon->height() * ratio)));
        int pixels = 0;
        const auto clipped = bounds.intersected(image.rect());
        for (int y = clipped.top(); y <= clipped.bottom(); ++y) {
            for (int x = clipped.left(); x <= clipped.right(); ++x) {
                const auto color = image.pixelColor(x, y);
                if (color.alpha() > 0 && color.red() >= 120 &&
                    color.green() >= 120 && color.blue() >= 120) {
                    ++pixels;
                }
            }
        }
        best = std::max(best, pixels);
    }
    return best;
}

// Loads the exact production QML, rasterises a real window, and verifies pixels
// inside representative toolbar icon bounds. This catches missing SVG resources
// and rendering regressions that a QML syntax-only handshake cannot see.
static int runIconCheck(int argc, char* argv[], const char* markerPath,
                        const char* screenshotPath) {
    QGuiApplication application(argc, argv);
    QQuickStyle::setStyle(QStringLiteral("Fusion"));
    ProjectController controller;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("projectController"), &controller);
    engine.loadFromModule(QStringLiteral("Motus"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) return 1;
    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
    if (!window) return 1;

    int outcome = 1;
    QTimer::singleShot(900, &application, [&] {
        const auto image = window->grabWindow();
        const std::array<QString, 3> names{
            QStringLiteral("folder"), QStringLiteral("magnet"), QStringLiteral("zoomIn")};
        std::array<int, 3> counts{};
        bool valid = !image.isNull();
        for (std::size_t index = 0; index < names.size(); ++index) {
            counts[index] = visibleIconPixels(window, image, names[index]);
            valid = valid && counts[index] >= 6;
        }
        if (!image.save(QString::fromLocal8Bit(screenshotPath), "PNG")) valid = false;
        std::ofstream marker(markerPath, std::ios::trunc);
        if (valid && marker) {
            marker << "MOTUS_ICONS_OK folder=" << counts[0]
                   << " magnet=" << counts[1] << " zoomIn=" << counts[2] << '\n';
            valid = static_cast<bool>(marker);
        }
        outcome = valid ? 0 : 1;
        application.exit(outcome);
    });
    (void)application.exec();
    return outcome;
}

int main(int argc, char* argv[]) {
    for (int index = 1; index + 2 < argc; ++index) {
        if (std::string_view(argv[index]) != "--instrumenta-icon-check") continue;
        return runIconCheck(argc, argv, argv[index + 1], argv[index + 2]);
    }
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string_view(argv[index]) != "--instrumenta-launch-check") continue;
        return runLaunchCheck(argc, argv, argv[index + 1]);
    }

    QGuiApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("Instrumenta"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("instrumenta.local"));
    QCoreApplication::setApplicationName(QStringLiteral("Motus"));
    QQuickStyle::setStyle(QStringLiteral("Fusion"));
    application.setWindowIcon(QIcon(QStringLiteral(":/qt/qml/Motus/app/assets/motus-mark.svg")));

    ProjectController projectController;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("projectController"), &projectController);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &application,
                     [] { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("Motus"), QStringLiteral("Main"));
    return application.exec();
}
