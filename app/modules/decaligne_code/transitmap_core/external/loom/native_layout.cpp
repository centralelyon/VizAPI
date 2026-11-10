#include "native_layout.h"
#include <memory>
#include <sstream>
#include <stdexcept>
#include "octi/Octilinearizer.h"
#include "loom/optim/CombNoILPOptimizer.h"
#include "shared/rendergraph/RenderGraph.h"
#include "util/geo/output/GeoGraphJsonOutput.h"

std::string nativeOctiLayout(const std::string& input) {
    // The same heuristic defaults as upstream octi; tune these here.
    octi::config::Config cfg;
    cfg.baseGraphType = octi::basegraph::BaseGraphType::OCTIGRID;
    cfg.orderMethod = octi::config::OrderMethod::ALL;
    std::istringstream stream(input);
    shared::linegraph::LineGraph graph;
    graph.readFromJson(&stream);
    graph.topologizeIsects();
    auto components = graph.distConnectedComponents(10000, false);
    std::ostringstream schematic;
    {
        util::geo::output::GeoJsonOutput output(schematic);
        util::geo::output::GeoGraphJsonOutput writer;
        for (auto& component : components) {
            double distance = 0;
            size_t count = 0;
            for (auto node : component.getNds()) {
                if (!node->getDeg()) continue;
                double local = 0;
                for (auto edge : node->getAdjList())
                    local += util::geo::dist(*node->pl().getGeom(), *edge->getOtherNd(node)->pl().getGeom());
                distance += local / node->getDeg();
                ++count;
            }
            if (!count || distance == 0) { writer.printLatLng(component, &output); continue; }
            const double gridSize = distance / count;
            octi::Octilinearizer optimizer(cfg.baseGraphType);
            component.contractStrayNds();
            component.contractEdges(gridSize / 2);
            component.splitNodes(optimizer.maxNodeDeg());
            octi::combgraph::CombGraph combined(&component, cfg.deg2Heur);
            auto box = util::geo::pad(component.getBBox(), gridSize + 1);
            shared::linegraph::LineGraph result;
            octi::basegraph::BaseGraph* grid = nullptr;
            octi::combgraph::Drawing drawing;
            optimizer.draw(combined, box, &result, &grid, &drawing, cfg.pens,
                gridSize, cfg.borderRad, cfg.maxGrDist, cfg.orderMethod,
                cfg.restrLocSearch, cfg.enfGeoPen, cfg.hananIters,
                cfg.obstacles, cfg.heurLocSearchIters, cfg.abortAfter);
            std::unique_ptr<octi::basegraph::BaseGraph> gridOwner(grid);
            writer.printLatLng(result, &output);
        }
        output.flush();
    }
    // LOOM minimizes line crossings/separations on the schematic graph.
    loom::config::Config lineCfg;
    shared::rendergraph::RenderGraph lines(5, 1, 5);
    std::istringstream layout(schematic.str());
    lines.readFromJson(&layout);
    const double cross = lines.maxDeg() * std::max({lineCfg.crossPenMultiSameSeg,
        lineCfg.crossPenMultiDiffSeg, lineCfg.stationCrossWeightSameSeg, lineCfg.stationCrossWeightDiffSeg});
    const double separation = lines.maxDeg() * std::max(lineCfg.separationPenWeight, lineCfg.stationSeparationWeight);
    shared::rendergraph::Penalties penalties{cross, separation,
        lineCfg.crossPenMultiSameSeg, lineCfg.crossPenMultiDiffSeg,
        lineCfg.separationPenWeight, lineCfg.stationCrossWeightSameSeg,
        lineCfg.stationCrossWeightDiffSeg, lineCfg.stationSeparationWeight, true, true};
    loom::optim::CombNoILPOptimizer lineOptimizer(&lineCfg, penalties);
    lineOptimizer.optimize(&lines);
    std::ostringstream result;
    util::geo::output::GeoGraphJsonOutput writer;
    writer.printLatLng(lines, result);
    return result.str();
}
