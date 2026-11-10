#ifndef BOKKIE_CLIENT_H
#define BOKKIE_CLIENT_H

#include <string>

#include "ToolRegistry.h"

class Client {

public:
    static std::ostream& send_payload(const std::string& prompt, ToolRegistry& registry, bool debug = false, bool stream = true);
};

#endif
