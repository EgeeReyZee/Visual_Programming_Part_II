#include "heatmap.h"
#include "map.h"

#include <cmath>
#include <algorithm>
#include <vector>
#include <iostream>

#include <GLFW/glfw3.h>
#ifndef GL_CLAMP_TO_EDGE
#  define GL_CLAMP_TO_EDGE 0x812F
#endif

#include "imgui/imgui.h"
#include "implot/implot.h"

struct RGB { float r, g, b; };

static RGB hsl2rgb(float h, float s, float l) {
    float c = (1.f - std::abs(2.f * l - 1.f)) * s;
    float x = c * (1.f - std::abs(std::fmod(h * 6.f, 2.f) - 1.f));
    float m = l - c / 2.f;
    float r = 0, g = 0, b = 0;
    int   seg = (int)(h * 6.f);
    switch (seg % 6) {
        case 0: r=c; g=x; b=0; break;
        case 1: r=x; g=c; b=0; break;
        case 2: r=0; g=c; b=x; break;
        case 3: r=0; g=x; b=c; break;
        case 4: r=x; g=0; b=c; break;
        case 5: r=c; g=0; b=x; break;
    }
    return { (r + m) * 255.f, (g + m) * 255.f, (b + m) * 255.f };
}

static void metric_to_color(HeatmapMetric metric, double value,
                             uint8_t& ro, uint8_t& go, uint8_t& bo, uint8_t& ao)
{
    float t = 0.f;
    switch (metric) {
        case HeatmapMetric::RSRP: {
            double linMin = std::pow(10.0, -110.0 / 10.0);
            double linMax = std::pow(10.0, -80.0  / 10.0);
            double lin    = std::pow(10.0, value   / 10.0);
            t = std::clamp((float)((lin - linMin) / (linMax - linMin)), 0.f, 1.f);
            break;
        }
        case HeatmapMetric::RSRQ: {
            double linMin = std::pow(10.0, -20.0 / 10.0);
            double linMax = std::pow(10.0, -3.0  / 10.0);
            double lin    = std::pow(10.0, value  / 10.0);
            t = std::clamp((float)((lin - linMin) / (linMax - linMin)), 0.f, 1.f);
            break;
        }
        case HeatmapMetric::RSSI: {
            if (value > 0) value = -65.6;
            double linMin = std::pow(10.0, -110.0 / 10.0);
            double linMax = std::pow(10.0, -65.0  / 10.0);
            double lin    = std::pow(10.0, value   / 10.0);
            t = std::clamp((float)((lin - linMin) / (linMax - linMin)), 0.f, 1.f);
            break;
        }
        case HeatmapMetric::RSSNR: {
            t = std::clamp((float)((value - (-10.0)) / 40.0), 0.f, 1.f);
            break;
        }
        case HeatmapMetric::Altitude: {
            t = std::clamp((float)(value / 500.0), 0.f, 1.f);
            break;
        }
        default: break;
    }

    RGB rgb = hsl2rgb(0.666f * (1.f - t), 1.f, 0.5f);
    ro = (uint8_t)std::clamp(rgb.r, 0.f, 255.f);
    go = (uint8_t)std::clamp(rgb.g, 0.f, 255.f);
    bo = (uint8_t)std::clamp(rgb.b, 0.f, 255.f);
    ao = 191;
}

static std::vector<HeatPoint> query_lte_points(DBContext& db, HeatmapMetric metric) {
    if (!db.connected || !db.conn) return {};

    const char* col = "rsrp";
    switch (metric) {
        case HeatmapMetric::RSRQ:  col = "rsrq";  break;
        case HeatmapMetric::RSSI:  col = "rssi";  break;
        case HeatmapMetric::RSSNR: col = "rssnr"; break;
        default: break;
    }

    char sql[1024];
    if (metric == HeatmapMetric::Altitude) {
        snprintf(sql, sizeof(sql),
            "SELECT latitude, longitude, altitude FROM location_data "
            "WHERE latitude IS NOT NULL AND longitude IS NOT NULL "
            "AND altitude IS NOT NULL LIMIT 50000;");
    } else {
        snprintf(sql, sizeof(sql),
            "SELECT latitude, longitude, %s FROM lte_cells "
            "WHERE latitude IS NOT NULL AND longitude IS NOT NULL "
            "AND %s IS NOT NULL LIMIT 50000;",
            col, col);
    }

    PGresult* res = PQexec(db.conn, sql);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::cerr << "heatmap query failed: " << PQerrorMessage(db.conn) << "\n";
        PQclear(res);
        return {};
    }

    int rows = PQntuples(res);
    std::vector<HeatPoint> pts;
    pts.reserve(rows);
    for (int i = 0; i < rows; i++) {
        float v = (float)atof(PQgetvalue(res, i, 2));
        if (metric == HeatmapMetric::RSSI && v > 0) continue;
        pts.push_back({ atof(PQgetvalue(res, i, 0)),
                        atof(PQgetvalue(res, i, 1)),
                        v });
    }
    PQclear(res);
    return pts;
}

static constexpr int   HEATMAP_SIZE = 512;
static constexpr double SIGMA       = 0.01;
static constexpr double CUTOFF_SQ   = (3.0 * SIGMA) * (3.0 * SIGMA);
static constexpr double SIGMA2      = SIGMA * SIGMA;

static std::vector<uint8_t> build_heatmap_image(
    const std::vector<HeatPoint>& pts,
    double min_lon, double max_lon,
    double min_lat, double max_lat,
    HeatmapMetric    metric,
    HeatmapEmptyFill empty_fill)
{
    const int N = HEATMAP_SIZE;
    std::vector<uint8_t> img(N * N * 4, 0);

    if (pts.empty() || min_lon >= max_lon || min_lat >= max_lat) return img;

    double lon_span = max_lon - min_lon;
    double lat_span = max_lat - min_lat;

    double sigma_deg = std::max(SIGMA, std::max(lon_span, lat_span) * 0.015);
    double sigma2    = sigma_deg * sigma_deg;
    double cutoffSq  = (3.0 * sigma_deg) * (3.0 * sigma_deg);

    for (int py = 0; py < N; py++) {
        double lat = max_lat - (py + 0.5) / N * lat_span;

        for (int px = 0; px < N; px++) {
            double lon = min_lon + (px + 0.5) / N * lon_span;

            double wsum = 0.0, vsum = 0.0;
            double minDistSq = 1e18;

            float nearestVal = 0.f;

            for (const auto& p : pts) {
                double dlat = p.lat - lat;
                double dlon = p.lon - lon;
                double d2   = dlat * dlat + dlon * dlon;

                if (d2 < minDistSq) {
                    minDistSq = d2;
                    nearestVal = p.value;
                }
                if (d2 > cutoffSq)  continue;

                double w = std::exp(-d2 / (2.0 * sigma2));
                wsum += w;
                vsum += w * p.value;
            }

            int idx = (py * N + px) * 4;

            if (minDistSq >= cutoffSq) {
                if (empty_fill == HeatmapEmptyFill::Blue) {
                    img[idx + 0] = 0;
                    img[idx + 1] = 0;
                    img[idx + 2] = 200;
                    img[idx + 3] = 80;
                }
                continue;
            }
        
            double distRatio = std::sqrt(minDistSq) / std::sqrt(cutoffSq);
            double edgeFade  = 1.0 - distRatio;
            edgeFade = edgeFade * edgeFade;

            if (edgeFade < 1.0 / 255.0) continue;

            float colorVal = (wsum > 1e-12) ? (float)(vsum / wsum) : nearestVal;

            uint8_t r, g, b, a;
            metric_to_color(metric, colorVal, r, g, b, a);

            img[idx + 0] = r;
            img[idx + 1] = g;
            img[idx + 2] = b;
            img[idx + 3] = (uint8_t)(a * edgeFade);
        }
    }
    return img;
}

struct WorkerPayload {
    HeatmapState* hs;
    DBContext*    db;
    MapBounds     bounds;
};

struct StagingBuf {
    std::vector<uint8_t> rgba;
    int w, h;
    double min_lon, max_lon, min_lat, max_lat;
};

static void heatmap_worker(WorkerPayload payload) {
    HeatmapState& hs = *payload.hs;
    DBContext&    db = *payload.db;

    double min_lon, max_lon, min_lat, max_lat;

    if (db.connected && db.conn) {
        const char* table = (hs.metric == HeatmapMetric::Altitude)
                            ? "location_data" : "lte_cells";
        char sql[256];
        snprintf(sql, sizeof(sql),
            "SELECT MIN(longitude), MAX(longitude), MIN(latitude), MAX(latitude) FROM %s "
            "WHERE latitude IS NOT NULL AND longitude IS NOT NULL;", table);
        PGresult* res = PQexec(db.conn, sql);
        if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) == 1 &&
            PQgetvalue(res, 0, 0)[0] != '\0') {
            min_lon = atof(PQgetvalue(res, 0, 0));
            max_lon = atof(PQgetvalue(res, 0, 1));
            min_lat = atof(PQgetvalue(res, 0, 2));
            max_lat = atof(PQgetvalue(res, 0, 3));
        } else {
            min_lon = payload.bounds.min_lon; max_lon = payload.bounds.max_lon;
            min_lat = payload.bounds.min_lat; max_lat = payload.bounds.max_lat;
        }
        PQclear(res);
    } else {
        min_lon = payload.bounds.min_lon; max_lon = payload.bounds.max_lon;
        min_lat = payload.bounds.min_lat; max_lat = payload.bounds.max_lat;
    }

    double pad_lon = (max_lon - min_lon) * 0.03;
    double pad_lat = (max_lat - min_lat) * 0.03;
    min_lon -= pad_lon; max_lon += pad_lon;
    min_lat -= pad_lat; max_lat += pad_lat;

    std::vector<HeatPoint> pts = query_lte_points(db, hs.metric);
    {
        std::lock_guard<std::mutex> lk(hs.points_mtx);
        hs.cached_points = pts;
    }
    if (pts.empty()) {
        hs.busy = false;
        return;
    }

    std::vector<uint8_t> rgba = build_heatmap_image(
        pts, min_lon, max_lon, min_lat, max_lat,
        hs.metric, hs.empty_fill);

    StagingBuf* stg = new StagingBuf{
        std::move(rgba), HEATMAP_SIZE, HEATMAP_SIZE,
        min_lon, max_lon, min_lat, max_lat
    };

    {
        std::lock_guard<std::mutex> lk(hs.result_mtx);
        hs.result.tex_id  = 0xFFFFFFFF;
        hs.result.ready   = false;
        uintptr_t ptr = reinterpret_cast<uintptr_t>(stg);
        hs.result.min_lon = (double)(uint32_t)(ptr & 0xFFFFFFFF);
        hs.result.max_lon = (double)(uint32_t)((ptr >> 32) & 0xFFFFFFFF);
    }

    hs.busy = false;
}

void heatmap_draw_points(HeatmapState& hs, int zoom) {
    if (!hs.enabled) return;
    if (!hs.show_points) return; 

    std::vector<HeatPoint> pts;
    {
        std::lock_guard<std::mutex> lk(hs.points_mtx);
        pts = hs.cached_points;
    }
    if (pts.empty()) return;

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    const float radius = std::clamp(2.0f + (zoom - 8) * 0.6f, 2.0f, 10.0f);

    for (const auto& p : pts) {
        double tx = lon_to_tile_x(p.lon, zoom);
        double ty = lat_to_tile_y(p.lat, zoom);
        ImVec2 screen = ImPlot::PlotToPixels(ImPlotPoint(tx, ty));

        uint8_t r, g, b, a;
        metric_to_color(hs.metric, p.value, r, g, b, a);

        dl->AddCircleFilled(screen, radius,
            IM_COL32(r, g, b, 210));
        dl->AddCircle(screen, radius,
            IM_COL32(0, 0, 0, 120), 12, 1.0f);
    }

    ImPlot::PopPlotClipRect();
}

HeatmapState::~HeatmapState() {
    heatmap_shutdown(*this);
}

void heatmap_init(HeatmapState& hs) { (void)hs; }

void heatmap_shutdown(HeatmapState& hs) {
    hs.shutdown = true;
    if (hs.worker.joinable()) hs.worker.join();
    std::lock_guard<std::mutex> lk(hs.result_mtx);
    if (hs.result.tex_id != 0 && hs.result.tex_id != 0xFFFFFFFF) {
        glDeleteTextures(1, &hs.result.tex_id);
        hs.result.tex_id = 0;
    }
}

void heatmap_upload_pending(HeatmapState& hs) {
    std::lock_guard<std::mutex> lk(hs.result_mtx);
    if (hs.result.tex_id != 0xFFFFFFFF) return;

    uintptr_t lo  = (uintptr_t)(uint32_t)hs.result.min_lon;
    uintptr_t hi  = (uintptr_t)(uint32_t)hs.result.max_lon;
    uintptr_t ptr = lo | (hi << 32);

    StagingBuf* stg = reinterpret_cast<StagingBuf*>(ptr);
    if (!stg) return;

    if (hs.result.tex_id != 0 && hs.result.tex_id != 0xFFFFFFFF) {
        unsigned int old = hs.result.tex_id;
        glDeleteTextures(1, &old);
    }

    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                 stg->w, stg->h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, stg->rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    hs.result.tex_id  = tex;
    hs.result.ready   = true;
    hs.result.min_lon = stg->min_lon;
    hs.result.max_lon = stg->max_lon;
    hs.result.min_lat = stg->min_lat;
    hs.result.max_lat = stg->max_lat;

    delete stg;
}

void heatmap_request_update(HeatmapState& hs, DBContext& db, const MapBounds& bounds) {
    if (!hs.enabled)       return;
    if (hs.busy)           return;
    if (!hs.dirty.load())  return;

    hs.dirty = false;
    hs.busy  = true;

    if (hs.worker.joinable()) hs.worker.join();

    WorkerPayload payload{&hs, &db, bounds};
    hs.worker = std::thread(heatmap_worker, payload);
}

void heatmap_draw(HeatmapState& hs, int zoom) {
    if (!hs.enabled) return;

    HeatmapState::Result res;
    {
        std::lock_guard<std::mutex> lk(hs.result_mtx);
        if (!hs.result.ready) return;
        res = hs.result;
    }

    ImDrawList* dl = ImPlot::GetPlotDrawList();
    ImPlot::PushPlotClipRect();

    double tx0 = lon_to_tile_x(res.min_lon, zoom);
    double tx1 = lon_to_tile_x(res.max_lon, zoom);
    double ty0 = lat_to_tile_y(res.max_lat, zoom);
    double ty1 = lat_to_tile_y(res.min_lat, zoom);

    ImVec2 p0 = ImPlot::PlotToPixels(ImPlotPoint(tx0, ty0));
    ImVec2 p1 = ImPlot::PlotToPixels(ImPlotPoint(tx1, ty1));

    dl->AddImage(
        reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(res.tex_id)),
        p0, p1,
        ImVec2(0, 0), ImVec2(1, 1),
        IM_COL32_WHITE);

    ImPlot::PopPlotClipRect();
}

void heatmap_draw_controls(HeatmapState& hs, DBContext& db,
                           const MapBounds& bounds, float dpi_scale) {
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.2f, 0.85f, 0.9f, 1.f), "Heatmap");
    ImGui::Spacing();

    bool changed = false;

    changed |= ImGui::Checkbox("Enable heatmap", &hs.enabled);
    if (!hs.enabled) return;
    
    ImGui::Checkbox("Show data points", &hs.show_points);

    ImGui::SetNextItemWidth(220 * dpi_scale);
    int metric_idx = (int)hs.metric;
    if (ImGui::Combo("Metric##hm", &metric_idx, HeatmapMetricNames, (int)HeatmapMetric::COUNT)) {
        hs.metric = (HeatmapMetric)metric_idx;
        changed = true;
    }

    ImGui::SetNextItemWidth(220 * dpi_scale);
    int fill_idx = (int)hs.empty_fill;
    if (ImGui::Combo("Empty fill##hm", &fill_idx, HeatmapEmptyFillNames, (int)HeatmapEmptyFill::COUNT)) {
        hs.empty_fill = (HeatmapEmptyFill)fill_idx;
        changed = true;
    }

    ImGui::Spacing();
    if (hs.busy)
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "  Generating...");
    else if (hs.result.ready)
        ImGui::TextColored(ImVec4(0.2f, 1.f, 0.4f, 1.f), "  Ready");
    else
        ImGui::TextDisabled("  Not generated yet");

    if (ImGui::Button("Regenerate##hm") || changed)
        hs.dirty = true;

    ImGui::Spacing();
    if (hs.metric == HeatmapMetric::RSRP) {
        ImGui::TextDisabled("RSRP legend:");
        ImGui::TextColored(ImVec4(1.f, 0.2f, 0.0f, 1.f), "  Excellent  > -80 dBm");
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.0f, 1.f), "  Good       -80 to -90 dBm");
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.3f, 1.f), "  Fair       -90 to -100 dBm");
        ImGui::TextColored(ImVec4(0.1f, 0.4f, 0.9f, 1.f), "  Poor       -100 to -110 dBm");
        ImGui::TextDisabled("  No signal  < -110 dBm (not drawn)");
    }

    heatmap_request_update(hs, db, bounds);
}