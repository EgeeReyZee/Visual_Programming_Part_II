#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <queue>
#include <functional>

struct TileKey {
    int zoom, x, y;
    bool operator==(const TileKey& o) const {
        return zoom == o.zoom && x == o.x && y == o.y;
    }
};

struct TileKeyHash {
    size_t operator()(const TileKey& k) const {
        size_t h = std::hash<int>{}(k.zoom);
        h ^= std::hash<int>{}(k.x)    + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(k.y)    + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

struct Tile {
    unsigned int tex_id  = 0;
    bool         ready   = false;
    int          width   = 0;
    int          height  = 0;
};

struct TileCache {
    std::unordered_map<TileKey, Tile, TileKeyHash> tiles;
    std::mutex                                      mtx;

    struct PendingUpload {
        TileKey          key;
        std::vector<unsigned char> rgba;
        int              w, h;
    };
    std::queue<PendingUpload> upload_queue;
    std::mutex                upload_mtx;
};

struct MapBounds {
    double min_lon;
    double max_lon;
    double min_lat;
    double max_lat;
    
    double width() const { return max_lon - min_lon; }
    double height() const { return max_lat - min_lat; }
    double center_lon() const { return (min_lon + max_lon) / 2.0; }
    double center_lat() const { return (min_lat + max_lat) / 2.0; }
};

struct MapState {
    double center_lat  =  55.01;
    double center_lon  =  82.95;
    int    zoom        =  1;
    int    last_zoom   =  1;

    std::vector<std::thread>          workers;
    std::queue<TileKey>               download_queue;
    std::mutex                        queue_mtx;
    std::atomic<bool>                 shutdown{false};

    TileCache cache;
    
    struct {
        double min_lon = 0;
        double max_lon = 0;
        double min_lat = 0;
        double max_lat = 0;
        int zoom = 0;
        mutable std::mutex mtx;   // mutable — чтобы lock в const-методе работал
    } current_bounds;
    
    void update_bounds(double min_lon_, double max_lon_, 
                       double min_lat_, double max_lat_, int zoom_) {
        std::lock_guard<std::mutex> lk(current_bounds.mtx);
        current_bounds.min_lon = min_lon_;
        current_bounds.max_lon = max_lon_;
        current_bounds.min_lat = min_lat_;
        current_bounds.max_lat = max_lat_;
        current_bounds.zoom = zoom_;
    }
    
    MapBounds get_bounds_copy() const {
        std::lock_guard<std::mutex> lk(current_bounds.mtx);
        return {current_bounds.min_lon, current_bounds.max_lon,
                current_bounds.min_lat, current_bounds.max_lat};
    }
};

MapBounds get_current_map_bounds(const MapState& ms, 
                                  double center_lat, 
                                  double center_lon, 
                                  int zoom, 
                                  float plot_w, 
                                  float plot_h);

void map_init(MapState& ms, int num_workers = 2);

void map_shutdown(MapState& ms);

void map_request_tile(MapState& ms, int zoom, int x, int y);

void map_upload_pending(MapState& ms);

void map_draw(MapState& ms, double center_lat, double center_lon,
              int zoom, float plot_w, float plot_h);

int    lat_lon_to_zoom    (double lat_span_deg);
double lon_to_tile_x      (double lon, int zoom);
double lat_to_tile_y      (double lat, int zoom);
double tile_x_to_lon      (int x, int zoom);
double tile_y_to_lat      (int y, int zoom);
double tile_x_to_lon_f    (double x, int zoom);
double tile_y_to_lat_f    (double y, int zoom);