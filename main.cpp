#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <libpq-fe.h>

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_glfw.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include "implot/implot.h"

#include <GLFW/glfw3.h>
#include <zmq.h>

#include "json.hpp"

// using json = nlohmann::json;
#define json nlohmann::json

struct DBContext {
    PGconn* conn = nullptr;
    bool connected = false;
};

struct LteCellData {
    int  band = 0, cellIdentity = 0, earfcn = 0, mcc = 0, mnc = 0, pci = 0, tac = 0;
    int  asuLevel = 0, cqi = 0, rsrp = 0, rsrq = 0, rssi = 0, rssnr = 0, timingAdvance = 0;
    bool isRegistered = false;
    bool valid = false;
};

struct GsmCellData {
    int  cellIdentity = 0, bsic = 0, arfcn = 0, lac = 0, mcc = 0, mnc = 0, psc = 0;
    int  dbm = 0, rssi = 0, timingAdvance = 0;
    bool isRegistered = false;
    bool valid = false;
};

struct NrCellData {
    int  band = 0, pci = 0, nrArfcn = 0, tac = 0, mcc = 0, mnc = 0;
    long nci  = 0;
    int  ssSinr = 0, ssRsrp = 0, ssRsrq = 0, timingAdvanceMicros = 0;
    bool isRegistered = false;
    bool valid = false;
};

struct NetworkInfo {
    std::string operator_name;
    std::string operator_numeric;
    std::string network_type;
    int         signal_dbm     = 0;
    long        cell_id        = 0;
    int         lac            = 0;
    int         mcc            = 0;
    int         mnc            = 0;
    bool        is_roaming     = false;
    int         visible_towers = 0;
    LteCellData lte;
    GsmCellData gsm;
    NrCellData  nr;
    bool        valid = false;
};

struct Location {
    double      latitude  = 0.0;
    double      longitude = 0.0;
    double      altitude  = 0.0;
    float       accuracy  = 0.0f;
    std::string provider;
    std::string raw;
    std::string timestamp;
    bool        valid     = false;
    NetworkInfo network;
};

static const int PLOT_HISTORY = 120;

struct SignalHistory {
    std::deque<float> signal_dbm;
    std::deque<float> lte_rsrp, lte_rsrq, lte_rssnr, lte_rssi;
    std::deque<float> gsm_dbm;
    std::deque<float> nr_ssRsrp, nr_ssRsrq, nr_ssSinr;

    void push(const NetworkInfo& net) {
        auto push_val = [](std::deque<float>& d, float v) {
            d.push_back(v);
            if ((int)d.size() > PLOT_HISTORY) d.pop_front();
        };
        push_val(signal_dbm,  (float)net.signal_dbm);
        if (net.lte.valid) {
            push_val(lte_rsrp,  (float)net.lte.rsrp);
            push_val(lte_rsrq,  (float)net.lte.rsrq);
            push_val(lte_rssnr, (float)net.lte.rssnr);
            push_val(lte_rssi,  (float)net.lte.rssi);
        }
        if (net.gsm.valid) {
            push_val(gsm_dbm,   (float)net.gsm.dbm);
        }
        if (net.nr.valid) {
            push_val(nr_ssRsrp, (float)net.nr.ssRsrp);
            push_val(nr_ssRsrq, (float)net.nr.ssRsrq);
            push_val(nr_ssSinr, (float)net.nr.ssSinr);
        }
    }
};

struct SharedState {
    std::mutex      mtx;
    Location        loc;
    SignalHistory   sigHist;
    int             msg_count      = 0;
    bool            server_running = false;
    std::string     log;
    std::vector<std::string> history;
};

std::vector<std::string> get_local_ips() {
    std::vector<std::string> ips;
#ifdef _WIN32
    std::system("ipconfig > _ip_tmp.txt 2>&1");
    std::ifstream f("_ip_tmp.txt");
    std::string line;
    while (std::getline(f, line)) {
        if (line.find("IPv4 Address") != std::string::npos) {
            auto pos = line.rfind(':');
            if (pos != std::string::npos) {
                std::string ip = line.substr(pos + 1);
                while (!ip.empty() && (ip.front() == ' ' || ip.front() == '\r')) ip.erase(ip.begin());
                while (!ip.empty() && (ip.back()  == ' ' || ip.back()  == '\r')) ip.pop_back();
                if (!ip.empty()) ips.push_back(ip);
            }
        }
    }
    std::remove("_ip_tmp.txt");
#else
    std::system("ip addr show > _ip_tmp.txt 2>/dev/null || ifconfig > _ip_tmp.txt 2>/dev/null");
    std::ifstream f("_ip_tmp.txt");
    std::string line;
    while (std::getline(f, line)) {
        auto pos = line.find("inet ");
        if (pos != std::string::npos && line.find("inet6") == std::string::npos) {
            std::string rest = line.substr(pos + 5);
            std::istringstream ss(rest);
            std::string addr;
            ss >> addr;
            auto slash = addr.find('/');
            if (slash != std::string::npos) addr = addr.substr(0, slash);
            if (!addr.empty()) ips.push_back(addr);
        }
    }
    std::remove("_ip_tmp.txt");
#endif
    return ips;
}

static const char* DATA_FILE = "location_messages.json";

bool connect_to_db(DBContext& db, const std::string& conninfo) {
    db.conn = PQconnectdb(conninfo.c_str());
    
    if (PQstatus(db.conn) != CONNECTION_OK) {
        std::cerr << "Connection to database failed: " 
                  << PQerrorMessage(db.conn) << std::endl;
        PQfinish(db.conn);
        db.conn = nullptr;
        db.connected = false;
        return false;
    }
    
    db.connected = true;
    std::cout << "Connected to PostgreSQL successfully!" << std::endl;
    return true;
}

std::string escape_string(PGconn* conn, const std::string& str) {
    size_t len = str.length();
    char* escaped = new char[len * 2 + 1];
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
    
    std::string escaped_timestamp = escape_string(db.conn, loc.timestamp);
    std::string escaped_provider = escape_string(db.conn, loc.provider);
    std::string escaped_raw = escape_string(db.conn, loc.raw);
    
    std::string escaped_operator_name = escape_string(db.conn, loc.network.operator_name);
    std::string escaped_operator_numeric = escape_string(db.conn, loc.network.operator_numeric);
    std::string escaped_network_type = escape_string(db.conn, loc.network.network_type);
    
    char sql[4096];
    snprintf(sql, sizeof(sql),
        "INSERT INTO location_data (counter, timestamp, latitude, longitude, altitude, "
        "accuracy, provider, operator_name, operator_numeric, network_type, signal_dbm, "
        "cell_id, lac, mcc, mnc, is_roaming, visible_towers) "
        "VALUES (%d, '%s', %f, %f, %f, %f, '%s', '%s', '%s', '%s', %d, %ld, %d, %d, %d, %s, %d) "
        "RETURNING id",
        counter, escaped_timestamp.c_str(), loc.latitude, loc.longitude, loc.altitude,
        loc.accuracy, escaped_provider.c_str(), escaped_operator_name.c_str(),
        escaped_operator_numeric.c_str(), escaped_network_type.c_str(), loc.network.signal_dbm,
        loc.network.cell_id, loc.network.lac, loc.network.mcc, loc.network.mnc,
        loc.network.is_roaming ? "true" : "false", loc.network.visible_towers);
    
    res = PQexec(db.conn, sql);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::cerr << "INSERT failed: " << PQerrorMessage(db.conn) << std::endl;
        PQclear(res);
        PQexec(db.conn, "ROLLBACK");
        return false;
    }
    
    int location_id = atoi(PQgetvalue(res, 0, 0));
    PQclear(res);
    
    if (loc.network.lte.valid) {
        snprintf(sql, sizeof(sql),
            "INSERT INTO lte_cells (location_id, pci, band, cell_identity, earfcn, tac, "
            "rsrp, rsrq, rssi, rssnr, cqi, asu_level, timing_advance, is_registered) "
            "VALUES (%d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %d, %s)",
            location_id, loc.network.lte.pci, loc.network.lte.band, 
            loc.network.lte.cellIdentity, loc.network.lte.earfcn, loc.network.lte.tac,
            loc.network.lte.rsrp, loc.network.lte.rsrq, loc.network.lte.rssi,
            loc.network.lte.rssnr, loc.network.lte.cqi, loc.network.lte.asuLevel,
            loc.network.lte.timingAdvance, loc.network.lte.isRegistered ? "true" : "false");
        
        res = PQexec(db.conn, sql);
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            std::cerr << "LTE INSERT failed: " << PQerrorMessage(db.conn) << std::endl;
        }
        PQclear(res);
    }
    
    if (loc.network.gsm.valid) {
        snprintf(sql, sizeof(sql),
            "INSERT INTO gsm_cells (location_id, cell_identity, lac, bsic, arfcn, psc, "
            "dbm, rssi, timing_advance, is_registered) "
            "VALUES (%d, %d, %d, %d, %d, %d, %d, %d, %d, %s)",
            location_id, loc.network.gsm.cellIdentity, loc.network.gsm.lac,
            loc.network.gsm.bsic, loc.network.gsm.arfcn, loc.network.gsm.psc,
            loc.network.gsm.dbm, loc.network.gsm.rssi, loc.network.gsm.timingAdvance,
            loc.network.gsm.isRegistered ? "true" : "false");
        
        res = PQexec(db.conn, sql);
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            std::cerr << "GSM INSERT failed: " << PQerrorMessage(db.conn) << std::endl;
        }
        PQclear(res);
    }
    
    if (loc.network.nr.valid) {
        snprintf(sql, sizeof(sql),
            "INSERT INTO nr_cells (location_id, pci, nci, band, nr_arfcn, tac, "
            "ss_rsrp, ss_rsrq, ss_sinr, timing_advance_micros, is_registered) "
            "VALUES (%d, %d, %ld, %d, %d, %d, %d, %d, %d, %d, %s)",
            location_id, loc.network.nr.pci, loc.network.nr.nci, loc.network.nr.band,
            loc.network.nr.nrArfcn, loc.network.nr.tac, loc.network.nr.ssRsrp,
            loc.network.nr.ssRsrq, loc.network.nr.ssSinr, loc.network.nr.timingAdvanceMicros,
            loc.network.nr.isRegistered ? "true" : "false");
        
        res = PQexec(db.conn, sql);
        if (PQresultStatus(res) != PGRES_COMMAND_OK) {
            std::cerr << "NR INSERT failed: " << PQerrorMessage(db.conn) << std::endl;
        }
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

void save_message(const Location& loc, int counter) {
    json msgs = json::array();
    {
        std::ifstream fi(DATA_FILE);
        if (fi.is_open()) {
            try { fi >> msgs; } catch (...) { msgs = json::array(); }
        }
    }
    json entry;
    entry["counter"]   = counter;
    entry["timestamp"] = loc.timestamp;
    entry["latitude"]  = loc.latitude;
    entry["longitude"] = loc.longitude;
    entry["altitude"]  = loc.altitude;
    entry["accuracy"]  = loc.accuracy;
    entry["provider"]  = loc.provider;
    entry["raw"]       = loc.raw;

    if (loc.network.valid) {
        auto& n = entry["network"];
        n["operator_name"]    = loc.network.operator_name;
        n["operator_numeric"] = loc.network.operator_numeric;
        n["network_type"]     = loc.network.network_type;
        n["signal_dbm"]       = loc.network.signal_dbm;
        n["cell_id"]          = loc.network.cell_id;
        n["lac"]              = loc.network.lac;
        n["mcc"]              = loc.network.mcc;
        n["mnc"]              = loc.network.mnc;
        n["is_roaming"]       = loc.network.is_roaming;
        n["visible_towers"]   = loc.network.visible_towers;

        if (loc.network.lte.valid) {
            auto& l = n["lte"];
            l["band"]          = loc.network.lte.band;
            l["cellIdentity"]  = loc.network.lte.cellIdentity;
            l["earfcn"]        = loc.network.lte.earfcn;
            l["mcc"]           = loc.network.lte.mcc;
            l["mnc"]           = loc.network.lte.mnc;
            l["pci"]           = loc.network.lte.pci;
            l["tac"]           = loc.network.lte.tac;
            l["asuLevel"]      = loc.network.lte.asuLevel;
            l["cqi"]           = loc.network.lte.cqi;
            l["rsrp"]          = loc.network.lte.rsrp;
            l["rsrq"]          = loc.network.lte.rsrq;
            l["rssi"]          = loc.network.lte.rssi;
            l["rssnr"]         = loc.network.lte.rssnr;
            l["timingAdvance"] = loc.network.lte.timingAdvance;
        }
        if (loc.network.gsm.valid) {
            auto& g = n["gsm"];
            g["cellIdentity"]  = loc.network.gsm.cellIdentity;
            g["bsic"]          = loc.network.gsm.bsic;
            g["arfcn"]         = loc.network.gsm.arfcn;
            g["lac"]           = loc.network.gsm.lac;
            g["mcc"]           = loc.network.gsm.mcc;
            g["mnc"]           = loc.network.gsm.mnc;
            g["psc"]           = loc.network.gsm.psc;
            g["dbm"]           = loc.network.gsm.dbm;
            g["rssi"]          = loc.network.gsm.rssi;
            g["timingAdvance"] = loc.network.gsm.timingAdvance;
        }
        if (loc.network.nr.valid) {
            auto& r = n["nr"];
            r["band"]                 = loc.network.nr.band;
            r["nci"]                  = loc.network.nr.nci;
            r["pci"]                  = loc.network.nr.pci;
            r["nrArfcn"]              = loc.network.nr.nrArfcn;
            r["tac"]                  = loc.network.nr.tac;
            r["mcc"]                  = loc.network.nr.mcc;
            r["mnc"]                  = loc.network.nr.mnc;
            r["ssSinr"]               = loc.network.nr.ssSinr;
            r["ssRsrp"]               = loc.network.nr.ssRsrp;
            r["ssRsrq"]               = loc.network.nr.ssRsrq;
            r["timingAdvanceMicros"]  = loc.network.nr.timingAdvanceMicros;
        }
    }

    msgs.push_back(entry);
    std::ofstream fo(DATA_FILE);
    fo << msgs.dump(2);
}

template<typename T>
T jget(const json& j, const char* key, T def = T{}) {
    if (!j.contains(key)) return def;
    try { return j[key].get<T>(); } catch (...) { return def; }
}

void run_server(const std::string& bind_addr,
                SharedState*       state,
                std::atomic<bool>& stop_flag,
                DBContext& db)
{
    void* ctx  = zmq_ctx_new();
    void* sock = zmq_socket(ctx, ZMQ_REP);

    if (zmq_bind(sock, bind_addr.c_str()) != 0) {
        std::lock_guard<std::mutex> lk(state->mtx);
        state->log = "ERROR: zmq_bind failed on " + bind_addr;
        state->server_running = false;
        zmq_close(sock); zmq_ctx_destroy(ctx);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(state->mtx);
        state->log = "Listening on " + bind_addr;
        state->server_running = true;
    }

    zmq_pollitem_t items[1];
    items[0].socket = sock;
    items[0].events = ZMQ_POLLIN;

    while (!stop_flag) {
        int rc = zmq_poll(items, 1, 100);
        if (rc <= 0) continue;

        char buf[8192] = {};
        int  len = zmq_recv(sock, buf, sizeof(buf) - 1, 0);
        if (len < 0) continue;
        buf[len] = '\0';

        std::string raw(buf);
        Location loc;
        loc.raw = raw;

        {
            auto now = std::chrono::system_clock::now();
            std::time_t t = std::chrono::system_clock::to_time_t(now);
            char ts[32];
            std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
            loc.timestamp = ts;
        }

        try {
            json j = json::parse(raw);
            loc.latitude  = jget<double>(j, "latitude");
            loc.longitude = jget<double>(j, "longitude");
            loc.altitude  = jget<double>(j, "altitude");
            loc.accuracy  = jget<float> (j, "accuracy");
            loc.provider  = jget<std::string>(j, "provider", "unknown");
            loc.valid     = true;

            if (j.contains("network")) {
                const json& n = j["network"];
                NetworkInfo net;
                net.operator_name    = jget<std::string>(n, "operator_name");
                net.operator_numeric = jget<std::string>(n, "operator_numeric");
                net.network_type     = jget<std::string>(n, "network_type");
                net.signal_dbm       = jget<int> (n, "signal_dbm");
                net.cell_id          = jget<long>(n, "cell_id");
                net.lac              = jget<int> (n, "lac");
                net.mcc              = jget<int> (n, "mcc");
                net.mnc              = jget<int> (n, "mnc");
                net.is_roaming       = jget<bool>(n, "is_roaming");
                net.visible_towers   = jget<int> (n, "visible_towers");
                net.valid = true;

                if (n.contains("lte")) {
                    const json& l = n["lte"];
                    net.lte.band          = jget<int>(l, "band");
                    net.lte.cellIdentity  = jget<int>(l, "cellIdentity");
                    net.lte.earfcn        = jget<int>(l, "earfcn");
                    net.lte.mcc           = jget<int>(l, "mcc");
                    net.lte.mnc           = jget<int>(l, "mnc");
                    net.lte.pci           = jget<int>(l, "pci");
                    net.lte.tac           = jget<int>(l, "tac");
                    net.lte.asuLevel      = jget<int>(l, "asuLevel");
                    net.lte.cqi           = jget<int>(l, "cqi");
                    net.lte.rsrp          = jget<int>(l, "rsrp");
                    net.lte.rsrq          = jget<int>(l, "rsrq");
                    net.lte.rssi          = jget<int>(l, "rssi");
                    net.lte.rssnr         = jget<int>(l, "rssnr");
                    net.lte.timingAdvance = jget<int>(l, "timingAdvance");
                    net.lte.valid = true;
                }
                if (n.contains("gsm")) {
                    const json& g = n["gsm"];
                    net.gsm.cellIdentity  = jget<int>(g, "cellIdentity");
                    net.gsm.bsic          = jget<int>(g, "bsic");
                    net.gsm.arfcn         = jget<int>(g, "arfcn");
                    net.gsm.lac           = jget<int>(g, "lac");
                    net.gsm.mcc           = jget<int>(g, "mcc");
                    net.gsm.mnc           = jget<int>(g, "mnc");
                    net.gsm.psc           = jget<int>(g, "psc");
                    net.gsm.dbm           = jget<int>(g, "dbm");
                    net.gsm.rssi          = jget<int>(g, "rssi");
                    net.gsm.timingAdvance = jget<int>(g, "timingAdvance");
                    net.gsm.valid = true;
                }
                if (n.contains("nr")) {
                    const json& r = n["nr"];
                    net.nr.band                = jget<int> (r, "band");
                    net.nr.nci                 = jget<long>(r, "nci");
                    net.nr.pci                 = jget<int> (r, "pci");
                    net.nr.nrArfcn             = jget<int> (r, "nrArfcn");
                    net.nr.tac                 = jget<int> (r, "tac");
                    net.nr.mcc                 = jget<int> (r, "mcc");
                    net.nr.mnc                 = jget<int> (r, "mnc");
                    net.nr.ssSinr              = jget<int> (r, "ssSinr");
                    net.nr.ssRsrp              = jget<int> (r, "ssRsrp");
                    net.nr.ssRsrq              = jget<int> (r, "ssRsrq");
                    net.nr.timingAdvanceMicros = jget<int> (r, "timingAdvanceMicros");
                    net.nr.valid = true;
                }

                loc.network = net;
            }
        } catch (...) {
            loc.valid    = false;
            loc.provider = "parse_error";
        }
        if (loc.valid) {
            save_location_to_db(db, loc, cnt);
        }
        int cnt;
        {
            std::lock_guard<std::mutex> lk(state->mtx);
            state->msg_count++;
            cnt = state->msg_count;
            state->loc = loc;
            state->sigHist.push(loc.network);
            state->history.push_back("[" + loc.timestamp + "] #" +
                                     std::to_string(cnt) + " | " + raw);
            if (state->history.size() > 50) state->history.erase(state->history.begin());
            state->log = "Last msg #" + std::to_string(cnt);
        }

        save_message(loc, cnt);

        std::string reply = "OK #" + std::to_string(cnt);
        zmq_send(sock, reply.c_str(), reply.size(), 0);
    }

    zmq_close(sock);
    zmq_ctx_destroy(ctx);
    std::lock_guard<std::mutex> lk(state->mtx);
    state->server_running = false;
    state->log = "Server stopped.";
}

static void plot_deque(const char* label, const std::deque<float>& d, ImVec4 col) {
    if (d.empty()) return;
    std::vector<float> xs(d.size()), ys(d.size());
    for (int i = 0; i < (int)d.size(); i++) { xs[i] = (float)i; ys[i] = d[i]; }
    ImPlotSpec spec;
    spec.LineColor  = col;
    spec.LineWeight = 1.5f;
    ImPlot::PlotLine(label, xs.data(), ys.data(), (int)d.size(), spec);
}

void run_gui(SharedState* state) {
    if (!glfwInit()) return;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    float dpi_scale = 1.0f;
    {
        GLFWmonitor* primary = glfwGetPrimaryMonitor();
        if (primary) {
            float xs = 1.0f, ys = 1.0f;
            glfwGetMonitorContentScale(primary, &xs, &ys);
            dpi_scale = (xs > ys) ? xs : ys;
            if (dpi_scale < 1.0f) dpi_scale = 1.0f;
        }
    }

    const int base_w = 920, base_h = 1080;
    GLFWwindow* window = glfwCreateWindow(
        (int)(base_w * dpi_scale), (int)(base_h * dpi_scale),
        "ZMQ Location Server", nullptr, nullptr);
    if (!window) { glfwTerminate(); return; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    io.FontGlobalScale = dpi_scale;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(dpi_scale);
    style.WindowRounding    = 6.0f  * dpi_scale;
    style.FrameRounding     = 4.0f  * dpi_scale;
    style.GrabRounding      = 4.0f  * dpi_scale;
    style.ScrollbarRounding = 6.0f  * dpi_scale;
    style.FramePadding      = ImVec2(8.0f  * dpi_scale, 5.0f * dpi_scale);
    style.ItemSpacing       = ImVec2(10.0f * dpi_scale, 6.0f * dpi_scale);

    ImVec4* c = style.Colors;
    c[ImGuiCol_Header]        = ImVec4(0.15f, 0.55f, 0.60f, 0.65f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.70f, 0.75f, 0.80f);
    c[ImGuiCol_Button]        = ImVec4(0.13f, 0.50f, 0.55f, 0.90f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.65f, 0.70f, 1.00f);
    c[ImGuiCol_ButtonActive]  = ImVec4(0.10f, 0.40f, 0.45f, 1.00f);
    c[ImGuiCol_CheckMark]     = ImVec4(0.20f, 0.85f, 0.90f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.35f, 0.40f, 1.00f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    std::vector<std::string> local_ips;
    int  selected_ip  = -1;
    char port_buf[16] = "5555";
    std::atomic<bool> stop_flag(false);
    std::thread server_thread;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        SignalHistory histCopy;
        Location      locCopy;
        NetworkInfo   netCopy;
        int           msgCount   = 0;
        bool          running_now = false;
        std::string   statusLog;
        std::vector<std::string> historyLines;
        {
            std::lock_guard<std::mutex> lk(state->mtx);
            histCopy     = state->sigHist;
            locCopy      = state->loc;
            netCopy      = state->loc.network;
            msgCount     = state->msg_count;
            running_now  = state->server_running;
            statusLog    = state->log;
            historyLines = state->history;
        }

        ImGui::SetNextWindowPos(ImVec2(10 * dpi_scale, 10 * dpi_scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(340 * dpi_scale, 480 * dpi_scale), ImGuiCond_FirstUseEver);
        ImGui::Begin("Server Control", nullptr);

        ImGui::TextColored(ImVec4(0.20f,0.85f,0.90f,1.f), "ZMQ Location Server");
        ImGui::SameLine(0, 12);
        if (running_now)
            ImGui::TextColored(ImVec4(0.2f,1.0f,0.4f,1.f),  "[RUNNING]");
        else
            ImGui::TextColored(ImVec4(0.8f,0.3f,0.3f,1.f),  "[STOPPED]");
        ImGui::Separator(); ImGui::Spacing();

        if (ImGui::Button("Scan local IPv4")) {
            local_ips   = get_local_ips();
            selected_ip = -1;
        }
        ImGui::Spacing();
        if (!local_ips.empty()) {
            ImGui::Text("Bind address:");
            for (int i = 0; i < (int)local_ips.size(); i++) {
                bool sel = (selected_ip == i);
                if (ImGui::RadioButton(local_ips[i].c_str(), sel)) selected_ip = i;
            }
        } else {
            ImGui::TextDisabled("Press Scan to find local IPs");
        }

        ImGui::Spacing();
        ImGui::Text("Port:"); ImGui::SameLine();
        ImGui::SetNextItemWidth(100 * dpi_scale);
        ImGui::InputText("##port", port_buf, sizeof(port_buf), ImGuiInputTextFlags_CharsDecimal);

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        {
            bool can = (selected_ip >= 0 && selected_ip < (int)local_ips.size());
            if (!running_now) {
                if (!can) ImGui::BeginDisabled();
                if (ImGui::Button("  Start Server  ", ImVec2(-1, 0))) {
                    stop_flag = false;
                    std::string addr = "tcp://" + local_ips[selected_ip] + ":" + port_buf;
                    if (server_thread.joinable()) server_thread.join();
                    server_thread = std::thread(run_server, addr, state, std::ref(stop_flag));
                }
                if (!can) ImGui::EndDisabled();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f,0.15f,0.15f,1.f));
                if (ImGui::Button("  Stop Server   ", ImVec2(-1, 0))) stop_flag = true;
                ImGui::PopStyleColor();
            }
        }

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.7f,0.7f,0.7f,1.f), "Status: %s", statusLog.c_str());

        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(360 * dpi_scale, 10 * dpi_scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(380 * dpi_scale, 560 * dpi_scale), ImGuiCond_FirstUseEver);
        ImGui::Begin("Location & Network", nullptr);

        ImGui::TextColored(ImVec4(0.20f,0.85f,0.90f,1.f), "Location");
        ImGui::Separator(); ImGui::Spacing();

        auto row = [&](const char* label, const char* fmt, ...) {
            ImGui::TextDisabled("  %-14s", label);
            ImGui::SameLine(160 * dpi_scale);
            char tmp[256]; va_list a; va_start(a,fmt);
            vsnprintf(tmp, sizeof(tmp), fmt, a); va_end(a);
            ImGui::TextUnformatted(tmp);
        };

        if (!locCopy.valid && msgCount == 0) {
            ImGui::TextDisabled("  Waiting for data from Android...");
        } else {
            row("Messages:",  "%d",       msgCount);
            row("Timestamp:", "%s",       locCopy.timestamp.c_str());
            row("Latitude:",  "%.7f",     locCopy.latitude);
            row("Longitude:", "%.7f",     locCopy.longitude);
            row("Altitude:",  "%.2f m",   locCopy.altitude);
            row("Accuracy:",  "%.1f m",   (double)locCopy.accuracy);
            row("Provider:",  "%s",       locCopy.provider.c_str());
        }

        ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

        ImGui::TextColored(ImVec4(0.20f,0.85f,0.90f,1.f), "Network");
        ImGui::Spacing();

        if (!netCopy.valid) {
            ImGui::TextDisabled("  No network data yet...");
        } else {
            row("Operator:",  "%s (%s)", netCopy.operator_name.c_str(), netCopy.operator_numeric.c_str());
            row("Type:",      "%s",      netCopy.network_type.c_str());
            row("Signal:",    "%d dBm",  netCopy.signal_dbm);
            row("Cell ID:",   "%ld",     netCopy.cell_id);
            row("LAC/TAC:",   "%d",      netCopy.lac);
            row("MCC/MNC:",   "%d/%d",   netCopy.mcc, netCopy.mnc);
            row("Roaming:",   "%s",      netCopy.is_roaming ? "YES" : "no");
            row("Towers:",    "%d",      netCopy.visible_towers);

            if (netCopy.lte.valid) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.3f,0.9f,0.5f,1.f), "  [LTE]");
                row("  Band:",       "%d",     netCopy.lte.band);
                row("  CI:",         "%d",     netCopy.lte.cellIdentity);
                row("  EARFCN:",     "%d",     netCopy.lte.earfcn);
                row("  PCI:",        "%d",     netCopy.lte.pci);
                row("  TAC:",        "%d",     netCopy.lte.tac);
                row("  ASU:",        "%d",     netCopy.lte.asuLevel);
                row("  CQI:",        "%d",     netCopy.lte.cqi);
                row("  RSRP:",       "%d dBm", netCopy.lte.rsrp);
                row("  RSRQ:",       "%d dB",  netCopy.lte.rsrq);
                row("  RSSI:",       "%d dBm", netCopy.lte.rssi);
                row("  RSSNR:",      "%d dB",  netCopy.lte.rssnr);
                row("  TA:",         "%d",     netCopy.lte.timingAdvance);
            }
            if (netCopy.gsm.valid) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.9f,0.7f,0.2f,1.f), "  [GSM]");
                row("  CI:",         "%d", netCopy.gsm.cellIdentity);
                row("  BSIC:",       "%d", netCopy.gsm.bsic);
                row("  ARFCN:",      "%d", netCopy.gsm.arfcn);
                row("  LAC:",        "%d", netCopy.gsm.lac);
                row("  PSC:",        "%d", netCopy.gsm.psc);
                row("  Dbm:",        "%d", netCopy.gsm.dbm);
                row("  RSSI:",       "%d", netCopy.gsm.rssi);
                row("  TA:",         "%d", netCopy.gsm.timingAdvance);
            }
            if (netCopy.nr.valid) {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.5f,0.6f,1.0f,1.f), "  [NR / 5G]");
                row("  Band:",    "%d",     netCopy.nr.band);
                row("  NCI:",     "%ld",    netCopy.nr.nci);
                row("  PCI:",     "%d",     netCopy.nr.pci);
                row("  NrArfcn:", "%d",     netCopy.nr.nrArfcn);
                row("  TAC:",     "%d",     netCopy.nr.tac);
                row("  SS-RSRP:", "%d dBm", netCopy.nr.ssRsrp);
                row("  SS-RSRQ:", "%d dB",  netCopy.nr.ssRsrq);
                row("  SS-SINR:", "%d dB",  netCopy.nr.ssSinr);
                row("  TA(µs):",  "%d",     netCopy.nr.timingAdvanceMicros);
            }
        }

        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(10 * dpi_scale, 500 * dpi_scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(730 * dpi_scale, 200 * dpi_scale), ImGuiCond_FirstUseEver);
        ImGui::Begin("Message Log", nullptr);
        ImGui::BeginChild("##logscroll", ImVec2(0,0), false,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (int i = (int)historyLines.size()-1; i >= 0; i--)
            ImGui::TextUnformatted(historyLines[i].c_str());
        ImGui::EndChild();
        ImGui::End();

        ImGui::SetNextWindowPos(ImVec2(750 * dpi_scale, 10 * dpi_scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(560 * dpi_scale, 260 * dpi_scale), ImGuiCond_FirstUseEver);
        ImGui::Begin("Signal dBm", nullptr);
        if (ImPlot::BeginPlot("##sig_dbm", ImVec2(-1, -1))) {
            ImPlot::SetupAxes("Sample", "dBm");
            ImPlot::SetupAxisLimits(ImAxis_Y1, -130, -30, ImGuiCond_Always);
            plot_deque("Signal dBm", histCopy.signal_dbm, ImVec4(0.2f,0.85f,0.9f,1.f));
            ImPlot::EndPlot();
        }
        ImGui::End();

        if (!histCopy.lte_rsrp.empty()) {
            ImGui::SetNextWindowPos(ImVec2(750 * dpi_scale, 280 * dpi_scale), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(560 * dpi_scale, 280 * dpi_scale), ImGuiCond_FirstUseEver);
            ImGui::Begin("LTE Signal", nullptr);
            if (ImPlot::BeginPlot("##lte_plot", ImVec2(-1, -1))) {
                ImPlot::SetupAxes("Sample", "dBm / dB");
                ImPlot::SetupAxisLimits(ImAxis_Y1, -150, 0, ImGuiCond_Always);
                plot_deque("RSRP",  histCopy.lte_rsrp,  ImVec4(0.2f,0.8f,0.3f,1.f));
                plot_deque("RSRQ",  histCopy.lte_rsrq,  ImVec4(0.9f,0.6f,0.1f,1.f));
                plot_deque("RSSI",  histCopy.lte_rssi,  ImVec4(0.7f,0.2f,0.9f,1.f));
                plot_deque("RSSNR", histCopy.lte_rssnr, ImVec4(0.2f,0.7f,0.9f,1.f));
                ImPlot::EndPlot();
            }
            ImGui::End();
        }

        if (!histCopy.gsm_dbm.empty()) {
            ImGui::SetNextWindowPos(ImVec2(750 * dpi_scale, 570 * dpi_scale), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(560 * dpi_scale, 260 * dpi_scale), ImGuiCond_FirstUseEver);
            ImGui::Begin("GSM Signal", nullptr);
            if (ImPlot::BeginPlot("##gsm_plot", ImVec2(-1, -1))) {
                ImPlot::SetupAxes("Sample", "dBm");
                ImPlot::SetupAxisLimits(ImAxis_Y1, -115, -50, ImGuiCond_Always);
                plot_deque("GSM dBm", histCopy.gsm_dbm, ImVec4(0.9f,0.7f,0.2f,1.f));
                ImPlot::EndPlot();
            }
            ImGui::End();
        }

        if (!histCopy.nr_ssRsrp.empty()) {
            ImGui::SetNextWindowPos(ImVec2(750 * dpi_scale, 840 * dpi_scale), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(560 * dpi_scale, 280 * dpi_scale), ImGuiCond_FirstUseEver);
            ImGui::Begin("NR (5G) Signal", nullptr);
            if (ImPlot::BeginPlot("##nr_plot", ImVec2(-1, -1))) {
                ImPlot::SetupAxes("Sample", "dBm / dB");
                ImPlot::SetupAxisLimits(ImAxis_Y1, -160, 0, ImGuiCond_Always);
                plot_deque("SS-RSRP", histCopy.nr_ssRsrp, ImVec4(0.4f,0.5f,1.0f,1.f));
                plot_deque("SS-RSRQ", histCopy.nr_ssRsrq, ImVec4(0.7f,0.4f,1.0f,1.f));
                plot_deque("SS-SINR", histCopy.nr_ssSinr, ImVec4(0.3f,0.9f,0.9f,1.f));
                ImPlot::EndPlot();
            }
            ImGui::End();
        }

        ImGui::Render();
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.10f, 0.12f, 0.14f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }
    stop_flag = true;
    if (server_thread.joinable()) server_thread.join();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
}

int main() {
    static SharedState shared;
    DBContext db;
    std::string conninfo = "host=localhost port=5432 dbname=cell_monitor user=cell_user password=cell_password";
    if (!connect_to_db(db, conninfo)) {
        std::cerr << "Failed to connect to database!" << std::endl;
        return 1;
    }
    run_gui(&shared, db);
    if (db.conn) {
        PQfinish(db.conn);
    }
    return 0;
}