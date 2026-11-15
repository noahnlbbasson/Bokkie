#include "Agent.h"

#include <exception>
#include <iostream>

int main() {
    try {
        Agent agent;
        agent.run();
    } catch (const std::exception& error) {
        std::cerr << "Fatal: " << error.what() << '\n';
        return 1;
    }
    return 0;
}