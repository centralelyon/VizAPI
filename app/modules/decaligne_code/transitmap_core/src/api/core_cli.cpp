#include "api/core_api.h"
#include <iostream>
#include <string>
int upstreamOctiMain(int argc, char** argv);
int upstreamLoomMain(int argc, char** argv);
int main(int argc, char** argv) {
    // Direct upstream entry points for reproducible source-level comparisons.
    if (argc > 1 && std::string(argv[1]) == "--upstream-octi")
        return upstreamOctiMain(argc - 1, argv + 1);
    if (argc > 1 && std::string(argv[1]) == "--upstream-loom")
        return upstreamLoomMain(argc - 1, argv + 1);
    try {
        nlohmann::json request; std::cin >> request;
        std::cout << transitCoreRequest(request).dump() << '\n';
        return 0;
    } catch(const std::exception& error) {
        std::cout << nlohmann::json{{"error", error.what()}}.dump() << '\n';
        return 1;
    }
}
