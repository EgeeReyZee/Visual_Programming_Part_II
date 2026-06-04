#include <iostream>

#include "types.h"
#include "db.h"
#include "gui.h"

int main() {
    static SharedState shared;
    DBContext db;

    const std::string conninfo =
        "host=localhost port=5432 dbname=cell_monitor "
        "user=cell_user password=cell_password";

    if (!connect_to_db(db, conninfo)) {
        std::cerr << "Failed to connect to database!" << std::endl;
        return 1;
    }

    run_gui(&shared, db);

    if (db.conn) PQfinish(db.conn);
    return 0;
}