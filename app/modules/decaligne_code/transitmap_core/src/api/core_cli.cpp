#include "api/core_api.h"
#include <iostream>
int main() {
    try {
        nlohmann::json request; std::cin >> request;
        std::cout << transitCoreRequest(request).dump() << '\n';
        return 0;
    } catch(const std::exception& error) {
        std::cout << nlohmann::json{{"error", error.what()}}.dump() << '\n';
        return 1;
    }
}
