#include "native_layout.h"
#include <sstream>

// Reuse upstream's preprocessing, grid setup and drawComp implementation.
// Its CLI remains callable from transitmap-core --upstream-octi.
#define main upstreamOctiMain
#include "octi/OctiMain.cpp"
#undef main

std::string nativeOctiLayout(const std::string& input, bool planarize) {
    // The previous API ran octi directly. Do not insert a LOOM round-trip.
    config::Config cfg;
    char program[] = "octi";
    char* defaults[] = {program, nullptr};
    config::ConfigReader().read(&cfg, 1, defaults);
    cfg.preserveShortEdges = !planarize;
    std::istringstream stream(input);
    LineGraph graph;
    graph.readFromJson(&stream);
    if (planarize) graph.topologizeIsects();
    auto components = graph.distConnectedComponents(10000, false);
    util::json::Array scores;
    TotalScore total;
    // Upstream emits pointer IDs: keep every result alive until all are written.
    struct Results {
        std::vector<LineGraph*> graphs;
        std::vector<BaseGraph*> grids;
        ~Results() {
            for (auto* g : graphs) delete g;
            for (auto* g : grids) delete g;
        }
    } results;
    for (auto& component : components) {
        auto componentCfg = cfg;
        // Existing cycle fix uses upstream's --no-deg2-heur setting.
        bool cycle = component.getNds().size() > 2;
        for (auto* node : component.getNds())
            if (node->getDeg() != 2) cycle = false;
        componentCfg.deg2Heur = !cycle;
        const double distance = avgStatDist(component);
        if (!(distance > 0)) {
            results.graphs.push_back(nullptr);
            continue;
        }
        drawComp(component, distance, scores,
                 results.graphs, results.grids, total, componentCfg);
    }
    std::ostringstream streamOut;
    util::geo::output::GeoJsonOutput output(streamOut);
    util::geo::output::GeoGraphJsonOutput writer;
    for (size_t i = 0; i < components.size(); ++i)
        writer.printLatLng(results.graphs[i] ? *results.graphs[i] : components[i], &output);
    output.flush();
    return streamOut.str();
}
