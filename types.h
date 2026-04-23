#pragma once

#include <string>
#include <vector>
#include <deque>
#include <mutex>

struct LteCellData {
    int  band = 0, cellIdentity = 0, earfcn = 0, mcc = 0, mnc = 0, pci = 0, tac = 0;
    int  asuLevel = 0, cqi = 0, rsrp = 0, rsrq = 0, rssi = 0, rssnr = 0, timingAdvance = 0;
    bool isRegistered = false;
    bool valid        = false;
};

struct GsmCellData {
    int  cellIdentity = 0, bsic = 0, arfcn = 0, lac = 0, mcc = 0, mnc = 0, psc = 0;
    int  dbm = 0, rssi = 0, timingAdvance = 0;
    bool isRegistered = false;
    bool valid        = false;
};

struct NrCellData {
    int  band = 0, pci = 0, nrArfcn = 0, tac = 0, mcc = 0, mnc = 0;
    long nci  = 0;
    int  ssSinr = 0, ssRsrp = 0, ssRsrq = 0, timingAdvanceMicros = 0;
    bool isRegistered = false;
    bool valid        = false;
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
    bool        valid          = false;
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
        push_val(signal_dbm, (float)net.signal_dbm);
        if (net.lte.valid) {
            push_val(lte_rsrp,  (float)net.lte.rsrp);
            push_val(lte_rsrq,  (float)net.lte.rsrq);
            push_val(lte_rssnr, (float)net.lte.rssnr);
            push_val(lte_rssi,  (float)net.lte.rssi);
        }
        if (net.gsm.valid) {
            push_val(gsm_dbm, (float)net.gsm.dbm);
        }
        if (net.nr.valid) {
            push_val(nr_ssRsrp, (float)net.nr.ssRsrp);
            push_val(nr_ssRsrq, (float)net.nr.ssRsrq);
            push_val(nr_ssSinr, (float)net.nr.ssSinr);
        }
    }
};

struct SharedState {
    std::mutex               mtx;
    Location                 loc;
    SignalHistory             sigHist;
    int                      msg_count      = 0;
    bool                     server_running = false;
    std::string              log;
    std::vector<std::string> history;
};

#include <libpq-fe.h>

struct DBContext {
    PGconn* conn      = nullptr;
    bool    connected = false;
};