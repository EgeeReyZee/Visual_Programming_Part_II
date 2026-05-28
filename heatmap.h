#pragma once

#include "map.h"
#include "db.h"

#include <vector>
#include <string>
#include <atomic>
#include <mutex>
#include <thread>

enum class HeatmapMetric {
    RSRP = 0,
    RSRQ,
    RSSI,
    RSSNR,
    Altitude,
    COUNT
};

static const char* HeatmapMetricNames[] = {
    "RSRP", "RSRQ", "RSSI", "RSSNR", "Altitude"
};

enum class HeatmapEmptyFill { Blue = 0, Transparent, COUNT };
static const char* HeatmapEmptyFillNames[] = { "Blue", "Transparent" };

struct HeatPoint {
    double lat, lon;
    float  value;
};

struct HeatmapState {
    HeatmapMetric    metric      = HeatmapMetric::RSRP;
    HeatmapEmptyFill empty_fill  = HeatmapEmptyFill::Blue;
    bool             enabled     = false;
    float            alpha       = 0.65f;
    int              grid_px     = 512;
    float            idw_radius  = 0.01f;
    float            idw_power   = 2.0f;

    float thr_excellent = -80.f;
    float thr_good      = -90.f;
    float thr_fair      = -100.f;
    float thr_poor      = -110.f;

    std::thread           worker;
    std::atomic<bool>     dirty{true};
    std::atomic<bool>     busy{false};
    std::atomic<bool>     shutdown{false};

    struct Result {
        unsigned int  tex_id   = 0;
        bool          ready    = false;
        double min_lon = 0, max_lon = 0;
        double min_lat = 0, max_lat = 0;
    };
    Result              result;
    std::mutex          result_mtx;

    ~HeatmapState();
};

void heatmap_init(HeatmapState& hs);

void heatmap_upload_pending(HeatmapState& hs);

void heatmap_request_update(HeatmapState& hs,
                             DBContext& db,
                             const MapBounds& bounds);

void heatmap_draw(HeatmapState& hs, int zoom);

void heatmap_draw_controls(HeatmapState& hs, DBContext& db,
                           const MapBounds& bounds,
                           float dpi_scale);

void heatmap_shutdown(HeatmapState& hs);