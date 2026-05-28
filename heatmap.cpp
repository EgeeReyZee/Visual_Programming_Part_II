#include "heatmap.h"
#include "map.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>
#include <iostream>

#include <GLFW/glfw3.h>
#ifndef GL_CLAMP_TO_EDGE
#  define GL_CLAMP_TO_EDGE 0x812F
#endif

#include "imgui/imgui.h"
#include "implot/implot.h"

static void gradient_rsrp(float t, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    if (t < 0.18f) { r = g = b = a = 0; return; }
    a = 255;
    if (t < 0.45f) {
        float s = (t - 0.18f) / 0.27f;
        r = (uint8_t)(0   + s * 0);
        g = (uint8_t)(30  + s * 120);
        b = (uint8_t)(200 + s * 55);
    } else if (t < 0.64f) {
        float s = (t - 0.45f) / 0.19f;
        r = (uint8_t)(0   + s * 100);
        g = (uint8_t)(150 + s * 80);
        b = (uint8_t)(255 - s * 200);
    } else if (t < 0.82f) {
        float s = (t - 0.64f) / 0.18f;
        r = (uint8_t)(100 + s * 155);
        g = (uint8_t)(230 - s * 100);
        b = (uint8_t)(55  - s * 55);
    } else {
        float s = (t - 0.82f) / 0.18f;
        r = (uint8_t)(255);
        g = (uint8_t)(130 - s * 130);
        b = 0;
    }
}

static void gradient_generic(float t, uint8_t& r, uint8_t& g, uint8_t& b, uint8_t& a) {
    if (t <= 0.0f) { r = g = b = a = 0; return; }
    t = std::min(t, 1.0f);
    a = 220;
    if (t < 0.25f) {
        float s = t / 0.25f;
        r = 0; g = (uint8_t)(s * 255); b = 255;
    } else if (t < 0.5f) {
        float s = (t - 0.25f) / 0.25f;
        r = 0; g = 255; b = (uint8_t)(255 - s * 255);
    } else if (t < 0.75f) {
        float s = (t - 0.5f) / 0.25f;
        r = (uint8_t)(s * 255); g = 255; b = 0;
    } else {
        float s = (t - 0.75f) / 0.25f;
        r = 255; g = (uint8_t)(255 - s * 255); b = 0;
    }
}

static float normalise(float v, HeatmapMetric metric) {
    switch (metric) {
        case HeatmapMetric::RSRP:
            return (v - (-120.f)) / ((-65.f) - (-120.f));
        case HeatmapMetric::RSRQ:
            return (v - (-20.f)) / ((-3.f) - (-20.f));
        case HeatmapMetric::RSSI:
            return (v - (-110.f)) / ((-50.f) - (-110.f));
        case HeatmapMetric::RSSNR:
            return (v - (-10.f)) / (40.f);
        case HeatmapMetric::Altitude:
            return (v - 100.f) / 300.f;
        default:
            return 0.f;
    }
}

static std::vector<HeatPoint> query_lte_points(DBContext& db,
                                                HeatmapMetric metric,
                                                const char* where_clause = "") {
    if (!db.connected || !db.conn) return {};

    const char* col = "rsrp";
    switch (metric) {
        case HeatmapMetric::RSRQ:    col = "rsrq";  break;
        case HeatmapMetric::RSSI:    col = "rssi";  break;
        case HeatmapMetric::RSSNR:   col = "rssnr"; break;
        default: break;
    }

    char sql[1024];
    if (metric == HeatmapMetric::Altitude) {
        snprintf(sql, sizeof(sql),
            "SELECT latitude, longitude, altitude FROM location_data "
            "WHERE latitude IS NOT NULL AND longitude IS NOT NULL "
            "AND altitude IS NOT NULL %s LIMIT 50000;",
            where_clause);
    } else {
        snprintf(sql, sizeof(sql),
            "SELECT latitude, longitude, %s FROM lte_cells "
            "WHERE latitude IS NOT NULL AND longitude IS NOT NULL "
            "AND %s IS NOT NULL %s LIMIT 50000;",
            col, col, where_clause);
    }

    PGresult* res = PQexec(db.conn, sql);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        std::cerr << "heatmap query failed: " << PQerrorMessage(db.conn) << std::endl;
        PQclear(res);
        return {};
    }

    int rows = PQntuples(res);
    std::vector<HeatPoint> pts;
    pts.reserve(rows);
    for (int i = 0; i < rows; i++) {
        HeatPoint p;
        p.lat   = atof(PQgetvalue(res, i, 0));
        p.lon   = atof(PQgetvalue(res, i, 1));
        p.value = (float)atof(PQgetvalue(res, i, 2));
        pts.push_back(p);
    }
    PQclear(res);
    return pts;
}


static std::vector<uint8_t> build_heatmap_image(
    const std::vector<HeatPoint>& pts,
    double min_lon, double max_lon,
    double min_lat, double max_lat,
    int    grid_px,
    float  idw_radius,
    float  idw_power,
    HeatmapMetric metric,
    HeatmapEmptyFill empty_fill)
{
    int N = grid_px;
    std::vector<uint8_t> img(N * N * 4, 0);

    if (pts.empty() || min_lon >= max_lon || min_lat >= max_lat) return img;

    double lon_span = max_lon - min_lon;
    double lat_span = max_lat - min_lat;

    for (int py = 0; py < N; py++) {
        double lat = max_lat - (py + 0.5) / N * lat_span;

        for (int px = 0; px < N; px++) {
            double lon = min_lon + (px + 0.5) / N * lon_span;

            double cos_lat = std::cos(lat * M_PI / 180.0);

            double wsum = 0.0, vsum = 0.0;
            bool   any  = false;

            for (const auto& p : pts) {
                double dlat = (p.lat - lat);
                double dlon = (p.lon - lon) * cos_lat;
                double dist = std::sqrt(dlat * dlat + dlon * dlon);

                if (dist > (double)idw_radius) continue;
                any = true;

                if (dist < 1e-9) {
                    wsum = 1.0; vsum = p.value; break;
                }
                double w = 1.0 / std::pow(dist, (double)idw_power);
                wsum += w;
                vsum += w * p.value;
            }

            if (!any || wsum < 1e-12) {
                if (empty_fill == HeatmapEmptyFill::Blue) {
                    int idx = (py * N + px) * 4;
                    img[idx + 0] = 0;
                    img[idx + 1] = 0;
                    img[idx + 2] = 200;
                    img[idx + 3] = 80;
                }
                // HeatmapEmptyFill::Transparent: leave pixel as 0,0,0,0 (already zeroed)
                continue;
            }

            float val = (float)(vsum / wsum);
            float t   = std::clamp(normalise(val, metric), 0.f, 1.f);

            uint8_t r, g, b, a;
            if (metric == HeatmapMetric::RSRP)
                gradient_rsrp(t, r, g, b, a);
            else
                gradient_generic(t, r, g, b, a);

            int idx = (py * N + px) * 4;
            img[idx + 0] = r;
            img[idx + 1] = g;
            img[idx + 2] = b;
            img[idx + 3] = a;
        }
    }
    return img;
}

struct WorkerPayload {
    HeatmapState*  hs;
    DBContext*     db;
    MapBounds      bounds;
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

    char where[256] = "";

    std::vector<HeatPoint> pts = query_lte_points(db, hs.metric, where);

    if (pts.empty()) {
        hs.busy = false;
        return;
    }

    float radius = hs.idw_radius;
    {
        float span = (float)std::max(max_lon - min_lon, max_lat - min_lat);
        radius = std::max(radius, span * 0.02f);
    }

    int N = hs.grid_px;
    std::vector<uint8_t> rgba = build_heatmap_image(
        pts, min_lon, max_lon, min_lat, max_lat,
        N, radius, hs.idw_power, hs.metric, hs.empty_fill);

    {
        std::lock_guard<std::mutex> lk(hs.result_mtx);
        hs.result.min_lon = min_lon; hs.result.max_lon = max_lon;
        hs.result.min_lat = min_lat; hs.result.max_lat = max_lat;
        hs.result.ready   = false;
    }

    struct Staging {
        std::vector<uint8_t> rgba;
        int                  w = 0, h = 0;
        bool                 pending = false;
    };
    struct StagingBuf {
        std::vector<uint8_t> rgba;
        int w, h;
        double min_lon, max_lon, min_lat, max_lat;
    };
    StagingBuf* stg = new StagingBuf{
        std::move(rgba), N, N,
        min_lon, max_lon, min_lat, max_lat
    };

    {
        std::lock_guard<std::mutex> lk(hs.result_mtx);
        static_assert(sizeof(uintptr_t) <= sizeof(uint64_t), "ptr size");
        hs.result.tex_id  = 0xFFFFFFFF;
        hs.result.ready   = false;
        uintptr_t ptr = reinterpret_cast<uintptr_t>(stg);
        hs.result.min_lon = (double)(uint32_t)(ptr & 0xFFFFFFFF);
        hs.result.max_lon = (double)(uint32_t)((ptr >> 32) & 0xFFFFFFFF);
    }

    hs.busy = false;
}

HeatmapState::~HeatmapState() {
    heatmap_shutdown(*this);
}

void heatmap_init(HeatmapState& hs) {
    (void)hs;
}

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

    struct StagingBuf {
        std::vector<uint8_t> rgba;
        int w, h;
        double min_lon, max_lon, min_lat, max_lat;
    };
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

void heatmap_request_update(HeatmapState& hs,
                             DBContext& db,
                             const MapBounds& bounds) {
    if (!hs.enabled) return;
    if (hs.busy)     return;

    bool need_regen = hs.dirty.load();

    if (!need_regen) return;

    hs.dirty        = false;
    hs.busy         = true;

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

    ImU32 tint = IM_COL32(255, 255, 255, (int)(hs.alpha * 255));
    dl->AddImage(
        reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(res.tex_id)),
        p0, p1,
        ImVec2(0, 0), ImVec2(1, 1),
        tint);

    ImPlot::PopPlotClipRect();
}

void heatmap_draw_controls(HeatmapState& hs, DBContext& db,
                           const MapBounds& bounds,
                           float dpi_scale) {
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.2f, 0.85f, 0.9f, 1.f), "Heatmap");
    ImGui::Spacing();

    bool changed = false;

    changed |= ImGui::Checkbox("Enable heatmap", &hs.enabled);
    if (!hs.enabled) return;

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

    ImGui::SetNextItemWidth(180 * dpi_scale);
    if (ImGui::SliderFloat("Opacity##hm", &hs.alpha, 0.05f, 1.0f, "%.2f"))
        {}

    ImGui::SetNextItemWidth(180 * dpi_scale);
    if (ImGui::SliderFloat("IDW Radius (deg)##hm", &hs.idw_radius, 0.001f, 0.5f, "%.4f"))
        changed = true;

    ImGui::SetNextItemWidth(180 * dpi_scale);
    if (ImGui::SliderFloat("IDW Power##hm", &hs.idw_power, 0.5f, 5.0f, "%.1f"))
        changed = true;

    int gp = hs.grid_px;
    ImGui::SetNextItemWidth(180 * dpi_scale);
    if (ImGui::SliderInt("Grid resolution##hm", &gp, 128, 1024)) {
        hs.grid_px = gp;
        changed = true;
    }

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