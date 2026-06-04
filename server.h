#pragma once

#include "types.h"
#include <string>
#include <atomic>
#include <vector>

Location parse_location(const std::string& raw);

void save_message(const Location& loc, int counter);

void run_server(const std::string& bind_addr,
                SharedState*       state,
                std::atomic<bool>& stop_flag,
                DBContext&         db);

std::vector<std::string> get_local_ips();