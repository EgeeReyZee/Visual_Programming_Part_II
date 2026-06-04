#include "server.h"
#include "db.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <ctime>
#include <cstring>

#include <zmq.h>

#include "json.hpp"

using json = nlohmann::json;

static const char* DATA_FILE = "location_messages.json";


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

template<typename T>
static T jget(const json& j, const char* key, T def = T{}) {
    if (!j.contains(key)) return def;
    try { return j[key].get<T>(); } catch (...) { return def; }
}

Location parse_location(const std::string& raw) {
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
            net.valid            = true;

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
                net.lte.valid         = true;
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
                net.gsm.valid         = true;
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
                net.nr.valid               = true;
            }

            loc.network = net;
        }
    } catch (...) {
        loc.valid    = false;
        loc.provider = "parse_error";
    }

    return loc;
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
            r["band"]                = loc.network.nr.band;
            r["nci"]                 = loc.network.nr.nci;
            r["pci"]                 = loc.network.nr.pci;
            r["nrArfcn"]             = loc.network.nr.nrArfcn;
            r["tac"]                 = loc.network.nr.tac;
            r["mcc"]                 = loc.network.nr.mcc;
            r["mnc"]                 = loc.network.nr.mnc;
            r["ssSinr"]              = loc.network.nr.ssSinr;
            r["ssRsrp"]              = loc.network.nr.ssRsrp;
            r["ssRsrq"]              = loc.network.nr.ssRsrq;
            r["timingAdvanceMicros"] = loc.network.nr.timingAdvanceMicros;
        }
    }

    msgs.push_back(entry);
    std::ofstream fo(DATA_FILE);
    fo << msgs.dump(2);
}

void run_server(const std::string& bind_addr,
                SharedState*       state,
                std::atomic<bool>& stop_flag,
                DBContext&         db)
{
    void* ctx  = zmq_ctx_new();
    void* sock = zmq_socket(ctx, ZMQ_REP);

    if (zmq_bind(sock, bind_addr.c_str()) != 0) {
        std::lock_guard<std::mutex> lk(state->mtx);
        state->log            = "ERROR: zmq_bind failed on " + bind_addr;
        state->server_running = false;
        zmq_close(sock);
        zmq_ctx_destroy(ctx);
        return;
    }

    {
        std::lock_guard<std::mutex> lk(state->mtx);
        state->log            = "Listening on " + bind_addr;
        state->server_running = true;
    }

    zmq_pollitem_t items[1];
    items[0].socket = sock;
    items[0].events = ZMQ_POLLIN;

    while (!stop_flag) {
        if (zmq_poll(items, 1, 100) <= 0) continue;

        char buf[8192] = {};
        int  len = zmq_recv(sock, buf, sizeof(buf) - 1, 0);
        if (len < 0) continue;
        buf[len] = '\0';

        Location loc = parse_location(std::string(buf));

        int cnt;
        {
            std::lock_guard<std::mutex> lk(state->mtx);
            state->msg_count++;
            cnt = state->msg_count;
            state->loc = loc;
            state->sigHist.push(loc.network);
            state->history.push_back(
                "[" + loc.timestamp + "] #" + std::to_string(cnt) + " | " + loc.raw);
            if (state->history.size() > 50)
                state->history.erase(state->history.begin());
            state->log = "Last msg #" + std::to_string(cnt);
        }

        if (loc.valid) save_location_to_db(db, loc, cnt);
        save_message(loc, cnt);

        std::string reply = "OK #" + std::to_string(cnt);
        zmq_send(sock, reply.c_str(), reply.size(), 0);
    }

    zmq_close(sock);
    zmq_ctx_destroy(ctx);

    std::lock_guard<std::mutex> lk(state->mtx);
    state->server_running = false;
    state->log            = "Server stopped.";
}