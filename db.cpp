#include "db.h"
#include "map.h"

#include <iostream>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

bool connect_to_db(DBContext& db, const std::string& conninfo) {
    db.conn = PQconnectdb(conninfo.c_str());

    if (PQstatus(db.conn) != CONNECTION_OK) {
        std::cerr << "Connection to database failed: "
                  << PQerrorMessage(db.conn) << std::endl;
        PQfinish(db.conn);
        db.conn      = nullptr;
        db.connected = false;
        return false;
    }

    db.connected = true;
    std::cout << "Connected to PostgreSQL successfully!" << std::endl;
    return true;
}

std::string escape_string(PGconn* conn, const std::string& str) {
    size_t len     = str.length();
    char*  escaped = new char[len * 2 + 1];
    PQescapeStringConn(conn, escaped, str.c_str(), len, nullptr);
    std::string result(escaped);
    delete[] escaped;
    return result;
}

bool save_location_to_db(DBContext& db, const Location& loc, int counter) {
    if (!db.connected || !db.conn) return false;

    PGresult* res = PQexec(db.conn, "BEGIN");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::cerr << "BEGIN failed: " << PQerrorMessage(db.conn) << std::endl;
        PQclear(res);
        return false;
    }
    PQclear(res);

    std::string esc_timestamp       = escape_string(db.conn, loc.timestamp);
    std::string esc_provider        = escape_string(db.conn, loc.provider);
    std::string esc_raw             = escape_string(db.conn, loc.raw);
    std::string esc_operator_name   = escape_string(db.conn, loc.network.operator_name);
    std::string esc_operator_num    = escape_string(db.conn, loc.network.operator_numeric);
    std::string esc_network_type    = escape_string(db.conn, loc.network.network_type);

    char sql[4096];
    snprintf(sql, sizeof(sql),
        "INSERT INTO location_data "
        "(counter, timestamp, latitude, longitude, altitude, accuracy, provider, "
        " operator_name, operator_numeric, network_type, signal_dbm, "
        " cell_id, lac, mcc, mnc, is_roaming, visible_towers) "
        "VALUES (%d, '%s', %f, %f, %f, %f, '%s', '%s', '%s', '%s', %d, %ld, %d, %d, %d, %s, %d) "
        "RETURNING id",
        counter,
        esc_timestamp.c_str(), loc.latitude, loc.longitude, loc.altitude, loc.accuracy,
        esc_provider.c_str(), esc_operator_name.c_str(), esc_operator_num.c_str(),
        esc_network_type.c_str(), loc.network.signal_dbm, loc.network.cell_id,
        loc.network.lac, loc.network.mcc, loc.network.mnc,
        loc.network.is_roaming ? "true" : "false", loc.network.visible_towers);

    res = PQexec(db.conn, sql);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::cerr << "INSERT location_data failed: " << PQerrorMessage(db.conn) << std::endl;
        PQclear(res);
        PQexec(db.conn, "ROLLBACK");
        return false;
    }

    int location_id = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);

    if (loc.network.lte.valid) {
        snprintf(sql, sizeof(sql),
            "INSERT INTO lte_cells "
            "(pci, band, cell_identity, earfcn, tac, "
            " rsrp, rsrq, rssi, rssnr, cqi, asu_level, timing_advance, is_registered) "
            "VALUES (%d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %s)",
            location_id,
            loc.network.lte.pci, loc.network.lte.band, loc.network.lte.cellIdentity,
            loc.network.lte.earfcn, loc.network.lte.tac,
            loc.network.lte.rsrp, loc.network.lte.rsrq, loc.network.lte.rssi,
            loc.network.lte.rssnr, loc.network.lte.cqi, loc.network.lte.asuLevel,
            loc.network.lte.timingAdvance,
            loc.network.lte.isRegistered ? "true" : "false");

        res = PQexec(db.conn, sql);
        if (PQresultStatus(res) != PGRES_COMMAND_OK)
            std::cerr << "LTE INSERT failed: " << PQerrorMessage(db.conn) << std::endl;
        PQclear(res);
    }

    if (loc.network.gsm.valid) {
        snprintf(sql, sizeof(sql),
            "INSERT INTO gsm_cells "
            "(location_id, cell_identity, lac, bsic, arfcn, psc, "
            " dbm, rssi, timing_advance, is_registered) "
            "VALUES (%d, %d, %d, %d, %d, %d, %d, %d, %d, %s)",
            location_id,
            loc.network.gsm.cellIdentity, loc.network.gsm.lac,
            loc.network.gsm.bsic, loc.network.gsm.arfcn, loc.network.gsm.psc,
            loc.network.gsm.dbm, loc.network.gsm.rssi, loc.network.gsm.timingAdvance,
            loc.network.gsm.isRegistered ? "true" : "false");

        res = PQexec(db.conn, sql);
        if (PQresultStatus(res) != PGRES_COMMAND_OK)
            std::cerr << "GSM INSERT failed: " << PQerrorMessage(db.conn) << std::endl;
        PQclear(res);
    }

    if (loc.network.nr.valid) {
        snprintf(sql, sizeof(sql),
            "INSERT INTO nr_cells "
            "(location_id, pci, nci, band, nr_arfcn, tac, "
            " ss_rsrp, ss_rsrq, ss_sinr, timing_advance_micros, is_registered) "
            "VALUES (%d, %d, %ld, %d, %d, %d, %d, %d, %d, %d, %s)",
            location_id,
            loc.network.nr.pci, loc.network.nr.nci, loc.network.nr.band,
            loc.network.nr.nrArfcn, loc.network.nr.tac,
            loc.network.nr.ssRsrp, loc.network.nr.ssRsrq, loc.network.nr.ssSinr,
            loc.network.nr.timingAdvanceMicros,
            loc.network.nr.isRegistered ? "true" : "false");

        res = PQexec(db.conn, sql);
        if (PQresultStatus(res) != PGRES_COMMAND_OK)
            std::cerr << "NR INSERT failed: " << PQerrorMessage(db.conn) << std::endl;
        PQclear(res);
    }

    res = PQexec(db.conn, "COMMIT");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        std::cerr << "COMMIT failed: " << PQerrorMessage(db.conn) << std::endl;
        PQclear(res);
        return false;
    }
    PQclear(res);
    return true;
}

bool db_control(DBContext& db, char* query, char* result_buffer, size_t buffer_size, MapState& ms) {
    if (!db.connected || !db.conn) {
        snprintf(result_buffer, buffer_size, "ERROR: DB CONNECTION FAILED");
        return false;
    }
    std::string str(query);
    std::stringstream ss(str);
    std::string word;
    std::vector<std::string> words;
    while (ss >> word) {
        words.push_back(word);
    }
    if (!words.empty() && words[0] == "GENERATE") {
        if (words.size() < 3) {
            snprintf(result_buffer, buffer_size, "ERROR: GENERATE requires table_name and count\nUsage: GENERATE <table_name> <count>");
            return false;
        }
        bool ok = generate_data_for_db(db, words[1].c_str(), std::stoi(words[2]), ms);
        snprintf(result_buffer, buffer_size, ok ? "Generated %s rows into %s successfully." : "GENERATE failed.",
                 words[2].c_str(), words[1].c_str());
        return ok;
    }
    PGresult* res = PQexec(db.conn, query);
    ExecStatusType status = PQresultStatus(res);
    
    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
        snprintf(result_buffer, buffer_size, "Query failed: %s\n%s", 
                 query, PQerrorMessage(db.conn));
        PQclear(res);
        return false;
    }
    result_buffer[0] = '\0';
    if (status == PGRES_TUPLES_OK) {
        int rows = PQntuples(res);
        int cols = PQnfields(res);
        for (int i = 0; i < cols; i++) {
            if (i > 0) strncat(result_buffer, "\t", buffer_size - strlen(result_buffer) - 1);
            strncat(result_buffer, PQfname(res, i), buffer_size - strlen(result_buffer) - 1);
        }
        strncat(result_buffer, "\n", buffer_size - strlen(result_buffer) - 1);
        for (int i = 0; i < cols; i++) {
            if (i > 0) strncat(result_buffer, "\t", buffer_size - strlen(result_buffer) - 1);
            strncat(result_buffer, "-------", buffer_size - strlen(result_buffer) - 1);
        }
        strncat(result_buffer, "\n", buffer_size - strlen(result_buffer) - 1);
        for (int i = 0; i < rows && i < 100; i++) {
            for (int j = 0; j < cols; j++) {
                if (j > 0) strncat(result_buffer, "\t", buffer_size - strlen(result_buffer) - 1);
                char* value = PQgetvalue(res, i, j);
                if (value) {
                    strncat(result_buffer, value, buffer_size - strlen(result_buffer) - 1);
                } else {
                    strncat(result_buffer, "NULL", buffer_size - strlen(result_buffer) - 1);
                }
            }
            strncat(result_buffer, "\n", buffer_size - strlen(result_buffer) - 1);
        }
        
        char row_count[256];
        snprintf(row_count, sizeof(row_count), "\n%d row(s) returned", rows);
        strncat(result_buffer, row_count, buffer_size - strlen(result_buffer) - 1);
    } 
    else if (status == PGRES_COMMAND_OK) {
        char cmd_status[256];
        snprintf(cmd_status, sizeof(cmd_status), "Query executed successfully.\n%s", 
                 PQcmdStatus(res));
        strncat(result_buffer, cmd_status, buffer_size - strlen(result_buffer) - 1);
        char* affected = PQcmdTuples(res);
        if (affected && affected[0] != '\0') {
            char affected_msg[256];
            snprintf(affected_msg, sizeof(affected_msg), "\nAffected rows: %s", affected);
            strncat(result_buffer, affected_msg, buffer_size - strlen(result_buffer) - 1);
        }
    }
    
    PQclear(res);
    return true;
}

static float rand_float(float lo, float hi) {
    return lo + (hi - lo) * (rand() / (float)RAND_MAX);
}

static int rand_int(int lo, int hi) {
    return lo + rand() % (hi - lo + 1);
}

static int rand_rsrp() { return rand_int(-120, -65); }
static int rand_rsrq() { return rand_int(-20,  -3);  }
static int rand_rssi() { return rand_int(-110, -50); }
static int rand_rssnr(){ return rand_int(-10,  30);  }

bool generate_data_for_db(DBContext& db, const char* table_name, int data_count, MapState& ms) {
    if (!db.connected || !db.conn) return false;

    srand((unsigned)time(nullptr));

    MapBounds bounds = ms.get_bounds_copy();
    if (bounds.max_lat == bounds.min_lat) {
        bounds.min_lat = 54.80; bounds.max_lat = 55.20;
        bounds.min_lon = 82.75; bounds.max_lon = 83.15;
    }

    const int   bands[]      = {1, 3, 7, 20, 28, 38, 40};
    const int   n_bands      = 7;
    const int   earfcns[]    = {100, 300, 1300, 2850, 9410, 38050, 39150};
    const int   tacs[]       = {1001, 2002, 3003, 4004, 5005};
    const int   n_tacs       = 5;

    std::string tname(table_name);

    if (tname == "lte_cells") {
        for (int i = 0; i < data_count; i++) {
            int   band_idx    = rand() % n_bands;
            int   band        = bands[band_idx];
            int   earfcn      = earfcns[band_idx] + rand_int(0, 50);
            int   pci         = rand_int(0, 503);
            int   tac         = tacs[rand() % n_tacs];
            int   cell_id     = rand_int(0, 268435455);
            int   rsrp        = rand_rsrp();
            int   rsrq        = rand_rsrq();
            int   rssi        = rand_rssi();
            int   rssnr       = rand_rssnr();
            int   cqi         = rand_int(1, 15);
            int   asu         = rand_int(0, 97);
            int   ta          = rand_int(0, 1282);
            float lat         = rand_float((float)bounds.min_lat, (float)bounds.max_lat);
            float lon         = rand_float((float)bounds.min_lon, (float)bounds.max_lon);
            int   is_reg      = (rand() % 4 == 0) ? 1 : 0;

            char query[512];
            snprintf(query, sizeof(query),
                "INSERT INTO lte_cells "
                "(pci, band, cell_identity, earfcn, tac, "
                " rsrp, rsrq, rssi, rssnr, cqi, asu_level, timing_advance, "
                " is_registered, latitude, longitude) "
                "VALUES (%d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %s, %f, %f)",
                pci, band, cell_id, earfcn, tac,
                rsrp, rsrq, rssi, rssnr, cqi, asu, ta,
                is_reg ? "true" : "false", lat, lon);

            PGresult* res = PQexec(db.conn, query);
            if (PQresultStatus(res) != PGRES_COMMAND_OK)
                std::cerr << "lte_cells INSERT failed: " << PQerrorMessage(db.conn) << std::endl;
            PQclear(res);
        }
        return true;
    }

    if (tname == "gsm_cells") {
        for (int i = 0; i < data_count; i++) {
            int   cell_id  = rand_int(0, 65535);
            int   lac      = rand_int(1, 65534);
            int   bsic     = rand_int(0, 63);
            int   arfcn    = rand_int(1, 124);
            int   psc      = rand_int(0, 511);
            int   dbm      = rand_int(-113, -51);
            int   rssi     = rand_int(0, 31);
            int   ta       = rand_int(0, 63);
            float lat      = rand_float((float)bounds.min_lat, (float)bounds.max_lat);
            float lon      = rand_float((float)bounds.min_lon, (float)bounds.max_lon);
            int   is_reg   = (rand() % 3 == 0) ? 1 : 0;

            char query[512];
            snprintf(query, sizeof(query),
                "INSERT INTO gsm_cells "
                "(cell_identity, lac, bsic, arfcn, psc, "
                " dbm, rssi, timing_advance, is_registered, latitude, longitude) "
                "VALUES (%d, %d, %d, %d, %d, %d, %d, %d, %s, %f, %f)",
                cell_id, lac, bsic, arfcn, psc,
                dbm, rssi, ta,
                is_reg ? "true" : "false", lat, lon);

            PGresult* res = PQexec(db.conn, query);
            if (PQresultStatus(res) != PGRES_COMMAND_OK)
                std::cerr << "gsm_cells INSERT failed: " << PQerrorMessage(db.conn) << std::endl;
            PQclear(res);
        }
        return true;
    }

    if (tname == "nr_cells") {
        const int nr_band_v[] = {77, 78, 79, 257, 258};
        const int nr_arfcns[] = {620000, 630000, 640000, 2054166, 2016667};
        const int n_nr        = 5;

        for (int i = 0; i < data_count; i++) {
            int  bidx   = rand() % n_nr;
            int  pci    = rand_int(0, 1007);
            long nci    = (long)rand_int(0, 68719476735 & 0x7FFFFFFF);
            int  band   = nr_band_v[bidx];
            int  nrarfcn= nr_arfcns[bidx] + rand_int(0, 200);
            int  tac    = tacs[rand() % n_tacs];
            int  ssrsrp = rand_int(-156, -31);
            int  ssrsrq = rand_int(-43,  -3);
            int  sssinr = rand_int(-23,  40);
            int  ta_us  = rand_int(0,    1500);
            float lat   = rand_float((float)bounds.min_lat, (float)bounds.max_lat);
            float lon   = rand_float((float)bounds.min_lon, (float)bounds.max_lon);
            int  is_reg = (rand() % 4 == 0) ? 1 : 0;

            char query[512];
            snprintf(query, sizeof(query),
                "INSERT INTO nr_cells "
                "(pci, nci, band, nr_arfcn, tac, "
                " ss_rsrp, ss_rsrq, ss_sinr, timing_advance_micros, "
                " is_registered, latitude, longitude) "
                "VALUES (%d, %ld, %d, %d, %d, %d, %d, %d, %d, %s, %f, %f)",
                pci, nci, band, nrarfcn, tac,
                ssrsrp, ssrsrq, sssinr, ta_us,
                is_reg ? "true" : "false", lat, lon);

            PGresult* res = PQexec(db.conn, query);
            if (PQresultStatus(res) != PGRES_COMMAND_OK)
                std::cerr << "nr_cells INSERT failed: " << PQerrorMessage(db.conn) << std::endl;
            PQclear(res);
        }
        return true;
    }

    if (tname == "location_data") {
        const char* providers[] = {"gps", "network", "fused"};
        const int   mccs[]      = {250};
        const int   mncs[]      = {1, 2, 10, 11, 20, 99};
        const int   n_mnc       = 6;

        for (int i = 0; i < data_count; i++) {
            float lat      = rand_float((float)bounds.min_lat, (float)bounds.max_lat);
            float lon      = rand_float((float)bounds.min_lon, (float)bounds.max_lon);
            float alt      = rand_float(100.0f, 400.0f);
            float accuracy = rand_float(3.0f, 50.0f);
            int   sig_dbm  = rand_int(-115, -55);
            int   lac      = rand_int(1, 65534);
            int   mnc      = mncs[rand() % n_mnc];
            int   towers   = rand_int(1, 8);

            char query[1024];
            snprintf(query, sizeof(query),
                "INSERT INTO location_data "
                "(timestamp, latitude, longitude, altitude, accuracy, provider, "
                " operator_name, operator_numeric, network_type, signal_dbm, "
                " cell_id, lac, mcc, mnc, is_roaming, visible_towers) "
                "VALUES (NOW(), %f, %f, %f, %f, '%s', 'MTS', '25001', 'LTE', %d, "
                "        %d, %d, 250, %d, false, %d)",
                lat, lon, alt, accuracy,
                providers[rand() % 3], sig_dbm,
                rand_int(1000, 9999999), lac, mnc, towers);

            PGresult* res = PQexec(db.conn, query);
            if (PQresultStatus(res) != PGRES_COMMAND_OK)
                std::cerr << "location_data INSERT failed: " << PQerrorMessage(db.conn) << std::endl;
            PQclear(res);
        }
        return true;
    }

    std::cerr << "generate_data_for_db: unknown table '" << table_name << "'" << std::endl;
    return false;
}