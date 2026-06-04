#include "map.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <filesystem>
#include <fstream>
#include <vector>
#include <algorithm>

#include <GLFW/glfw3.h>

#ifndef GL_CLAMP_TO_EDGE
#  define GL_CLAMP_TO_EDGE 0x812F
#endif

#include "imgui/imgui.h"
#include "implot/implot.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

static constexpr int    TILE_PX      = 256;
static constexpr int    ZOOM_MIN     = 0;
static constexpr int    ZOOM_MAX     = 19;
static constexpr double PI           = 3.14159265358979323846;
static constexpr double MERCATOR_MAX_LAT = 85.0511287798;

static const char* TILE_URL_FMT = "https://tile.openstreetmap.org/%d/%d/%d.png";
static const char* CACHE_DIR    = "tiles";

double lat_to_tile_y(double lat, int zoom) {
    lat = std::clamp(lat, -MERCATOR_MAX_LAT, MERCATOR_MAX_LAT);
    double lat_r = lat * PI / 180.0;
    double merc  = std::log(std::tan(lat_r) + 1.0 / std::cos(lat_r));
    return (1.0 - merc / PI) / 2.0 * (1 << zoom);
}

double lon_to_tile_x(double lon, int zoom) {
    return (lon + 180.0) / 360.0 * (1 << zoom);
}

double tile_x_to_lon(int x, int zoom) {
    return (x / static_cast<double>(1 << zoom)) * 360.0 - 180.0;
}

double tile_y_to_lat(int y, int zoom) {
    double n = PI - 2.0 * PI * y / (1 << zoom);
    return 180.0 / PI * std::atan(std::sinh(n));
}

double tile_x_to_lon_f(double x, int zoom) {
    return x / static_cast<double>(1 << zoom) * 360.0 - 180.0;
}

double tile_y_to_lat_f(double y, int zoom) {
    double n = PI - 2.0 * PI * y / (1 << zoom);
    return 180.0 / PI * std::atan(std::sinh(n));
}

int lat_lon_to_zoom(double lon_span_deg) {
    if (lon_span_deg <= 0.0) return ZOOM_MAX;
    int z = static_cast<int>(std::round(std::log2(360.0 / lon_span_deg)));
    return std::max(ZOOM_MIN, std::min(ZOOM_MAX, z));
}

MapBounds get_current_map_bounds(const MapState& ms, 
                                  double center_lat, 
                                  double center_lon, 
                                  int zoom, 
                                  float plot_w, 
                                  float plot_h) {
    MapBounds bounds;
    
    int tile_count = 1 << zoom;
    double cx = lon_to_tile_x(center_lon, zoom);
    double cy = lat_to_tile_y(center_lat, zoom);
    double half_tx = plot_w / (2.0 * TILE_PX);
    double half_ty = plot_h / (2.0 * TILE_PX);
    
    double left_tile   = cx - half_tx;
    double right_tile  = cx + half_tx;
    double bottom_tile = cy - half_ty;
    double top_tile    = cy + half_ty;
    
    bounds.min_lon = tile_x_to_lon_f(left_tile, zoom);
    bounds.max_lon = tile_x_to_lon_f(right_tile, zoom);
    bounds.min_lat = tile_y_to_lat_f(bottom_tile, zoom);
    bounds.max_lat = tile_y_to_lat_f(top_tile, zoom);
    
    return bounds;
}

static std::string tile_cache_path(int zoom, int x, int y) {
    return std::string(CACHE_DIR) + "/" +
           std::to_string(zoom) + "/" +
           std::to_string(x)    + "/" +
           std::to_string(y)    + ".png";
}

static bool download_tile_curl(int zoom, int x, int y) {
    std::string path = tile_cache_path(zoom, x, y);
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    
    char url[256];
    std::snprintf(url, sizeof(url), TILE_URL_FMT, zoom, x, y);
    
    std::ofstream ofs(path, std::ios::binary);
    if (!ofs.is_open()) return false;
    
    char cmd[512];
    std::snprintf(cmd, sizeof(cmd),
        "curl -s --connect-timeout 5 --max-time 10 -A \"ZMQLocationServer/1.0\" -o \"%s\" \"%s\"",
        path.c_str(), url);
    
    int ret = std::system(cmd);
    return (ret == 0 && std::filesystem::file_size(path) > 0);
}

static bool load_png_to_rgba(const std::string& path,
                              std::vector<unsigned char>& out,
                              int& w, int& h)
{
    int channels = 0;
    unsigned char* data = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!data) return false;

    out.assign(data, data + w * h * 4);
    stbi_image_free(data);
    return true;
}

static unsigned int create_gl_texture(const unsigned char* rgba, int w, int h) {
    unsigned int tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

static void worker_thread(MapState* ms) {
    while (!ms->shutdown) {
        TileKey key{0, 0, 0};
        bool got_job = false;

        {
            std::lock_guard<std::mutex> lk(ms->queue_mtx);
            if (!ms->download_queue.empty()) {
                key = ms->download_queue.front();
                ms->download_queue.pop();
                got_job = true;
            }
        }

        if (!got_job) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        {
            std::lock_guard<std::mutex> lk(ms->cache.mtx);
            if (ms->cache.tiles.count(key)) continue;
        }

        std::string path = tile_cache_path(key.zoom, key.x, key.y);
        bool file_exists = std::filesystem::exists(path);

        if (!file_exists) {
            if (!download_tile_curl(key.zoom, key.x, key.y)) continue;
        }

        std::vector<unsigned char> rgba;
        int w = 0, h = 0;
        if (!load_png_to_rgba(path, rgba, w, h)) continue;

        {
            std::lock_guard<std::mutex> lk(ms->cache.upload_mtx);
            ms->cache.upload_queue.push({key, std::move(rgba), w, h});
        }
    }
}

void map_init(MapState& ms, int num_workers) {
    ms.shutdown = false;
    ms.last_zoom = -1;
    for (int i = 0; i < num_workers; ++i)
        ms.workers.emplace_back(worker_thread, &ms);
}

void map_shutdown(MapState& ms) {
    ms.shutdown = true;
    for (auto& t : ms.workers)
        if (t.joinable()) t.join();
    ms.workers.clear();
}

void map_request_tile(MapState& ms, int zoom, int x, int y) {
    TileKey key{zoom, x, y};

    {
        std::lock_guard<std::mutex> lk(ms.cache.mtx);
        if (ms.cache.tiles.count(key)) return;
    }

    {
        std::lock_guard<std::mutex> lk(ms.queue_mtx);
        std::queue<TileKey> temp_queue = ms.download_queue;
        bool found = false;
        while (!temp_queue.empty()) {
            if (temp_queue.front() == key) {
                found = true;
                break;
            }
            temp_queue.pop();
        }
        if (found) return;
    }

    {
        std::lock_guard<std::mutex> lk(ms.queue_mtx);
        ms.download_queue.push(key);
    }
}

void map_upload_pending(MapState& ms) {
    int budget = 8;
    while (budget-- > 0) {
        TileCache::PendingUpload up;
        {
            std::lock_guard<std::mutex> lk(ms.cache.upload_mtx);
            if (ms.cache.upload_queue.empty()) break;
            up = std::move(ms.cache.upload_queue.front());
            ms.cache.upload_queue.pop();
        }

        unsigned int tex = create_gl_texture(up.rgba.data(), up.w, up.h);
        {
            std::lock_guard<std::mutex> lk(ms.cache.mtx);
            Tile& tile = ms.cache.tiles[up.key];
            if (tile.tex_id != 0) {
                glDeleteTextures(1, &tile.tex_id);
            }
            tile.tex_id = tex;
            tile.width  = up.w;
            tile.height = up.h;
            tile.ready  = true;
        }
    }
}

void map_draw(MapState& ms, double center_lat, double center_lon,
              int zoom, float plot_w, float plot_h)
{
    if (zoom != ms.last_zoom) {
        ms.last_zoom = zoom;
        std::lock_guard<std::mutex> lk(ms.queue_mtx);
        std::queue<TileKey> empty;
        std::swap(ms.download_queue, empty);
    }

    int tile_count = 1 << zoom;
    double cx = lon_to_tile_x(center_lon, zoom);
    double cy = lat_to_tile_y(center_lat, zoom);
    double half_tx = plot_w / (2.0 * TILE_PX);
    double half_ty = plot_h / (2.0 * TILE_PX);

    int tile_min_x = (int)std::floor(cx - half_tx);
    int tile_max_x = (int)std::floor(cx + half_tx);
    int tile_min_y = (int)std::max(0.0,                       std::floor(cy - half_ty));
    int tile_max_y = (int)std::min((double)tile_count - 1.0,  std::floor(cy + half_ty));

    ImDrawList* dl = ImPlot::GetPlotDrawList();

    ImPlot::PushPlotClipRect();

    for (int ty = tile_min_y; ty <= tile_max_y; ++ty) {
        for (int tx = tile_min_x; tx <= tile_max_x; ++tx) {
            int wrapped_x = ((tx % tile_count) + tile_count) % tile_count;

            map_request_tile(ms, zoom, wrapped_x, ty);

            ImVec2 p0 = ImPlot::PlotToPixels(ImPlotPoint((double)tx,       (double)ty));
            ImVec2 p1 = ImPlot::PlotToPixels(ImPlotPoint((double)(tx + 1), (double)(ty + 1)));

            TileKey key{zoom, wrapped_x, ty};
            unsigned int tex_id = 0;
            {
                std::lock_guard<std::mutex> lk(ms.cache.mtx);
                auto it = ms.cache.tiles.find(key);
                if (it != ms.cache.tiles.end() && it->second.ready)
                    tex_id = it->second.tex_id;
            }

            if (tex_id == 0) {
                dl->AddRectFilled(p0, p1, IM_COL32(45, 45, 55, 255));
            } else {
                dl->AddImage(
                    reinterpret_cast<ImTextureID>(static_cast<uintptr_t>(tex_id)),
                    p0, p1,
                    ImVec2(0, 0), ImVec2(1, 1),
                    IM_COL32_WHITE
                );
            }
        }
    }

    MapBounds bounds = get_current_map_bounds(ms, center_lat, center_lon, 
                                               zoom, plot_w, plot_h);
    ms.update_bounds(bounds.min_lon, bounds.max_lon, 
                     bounds.min_lat, bounds.max_lat, zoom);

    double pos_tx = lon_to_tile_x(center_lon, zoom);
    double pos_ty = lat_to_tile_y(center_lat, zoom);
    ImVec2 screen_pos = ImPlot::PlotToPixels(ImPlotPoint(pos_tx, pos_ty));
    const float R   = 10.0f;
    const float W   = 2.5f;
    const ImU32 COL = IM_COL32(255, 50, 50, 255);
    dl->AddLine(ImVec2(screen_pos.x - R, screen_pos.y), ImVec2(screen_pos.x + R, screen_pos.y), COL, W);
    dl->AddLine(ImVec2(screen_pos.x, screen_pos.y - R), ImVec2(screen_pos.x, screen_pos.y + R), COL, W);
    dl->AddCircle(screen_pos, 3.5f, COL, 12, W);

    ImPlot::PopPlotClipRect();
}