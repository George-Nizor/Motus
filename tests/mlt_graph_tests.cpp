#include "test.h"
#include "ve/mlt_graph.h"

TEST("MLT graph keeps exact inclusive clip boundaries") {
    const auto project = test::sampleProject();
    const auto graph = ve::buildMltGraph(project, project.sequences[0], {ve::GraphPurpose::Render, {}});
    CHECK(graph.frameCount == 300);
    CHECK(graph.xml.find("in=\"0\" out=\"299\"") != std::string::npos);
    CHECK(graph.xml.find("out=\"300\"") == std::string::npos);
    CHECK(graph.xml.find("producer=\"playlist_0\" hide=\"audio\"") != std::string::npos);
    CHECK(graph.xml.find("producer=\"playlist_1\" hide=\"video\"") != std::string::npos);
}

TEST("MLT preview may use proxies but render always uses originals") {
    auto project = test::sampleProject();
    project.projectPath = "C:/Projects/show/edit.veproj";
    ve::MltGraphOptions preview{ve::GraphPurpose::Preview, {{"asset", "C:/Cache/proxy.mxf"}}};
    const auto previewGraph = ve::buildMltGraph(project, project.sequences[0], preview);
    const auto renderGraph = ve::buildMltGraph(project, project.sequences[0],
                                                {ve::GraphPurpose::Render, preview.proxyPaths});
    CHECK(previewGraph.xml.find("C:/Cache/proxy.mxf") != std::string::npos);
    CHECK(renderGraph.xml.find("C:/Projects/show/media/talking-head.mp4") != std::string::npos);
    CHECK(renderGraph.xml.find("proxy.mxf") == std::string::npos);
}
