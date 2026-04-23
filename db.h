#pragma once

#include "types.h"
#include <string>

bool        connect_to_db       (DBContext& db, const std::string& conninfo);
bool        save_location_to_db (DBContext& db, const Location& loc, int counter);
std::string escape_string       (PGconn* conn,  const std::string& str);