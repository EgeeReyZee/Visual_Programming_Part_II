#include "gui.h"
#include "server.h"
#include "map.h"
#include "db.h"
#include "heatmap.h"

#include <cstdarg>
#include <cmath>
#include <thread>
#include <atomic>
#include <string>
#include <vector>
#include <algorithm>

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_glfw.h"
#include "imgui/backends/imgui_impl_opengl3.h"
#include "implot/implot.h"
#include <GLFW/glfw3.h>


static void plot_deque(const char* label, const std::deque<float>& d, ImVec4 col) {
    if (d.empty()) return;
    std::vector<float> xs(d.size()), ys(d.size());
    for (int i = 0; i < (int)d.size(); i++) { xs[i] = (float)i; ys[i] = d[i]; }
    ImPlotSpec spec;
    spec.LineColor  = col;
    spec.LineWeight = 1.5f;
    ImPlot::PlotLine(label, xs.data(), ys.data(), (int)d.size(), spec);
}

static float init_window(const char* title, int base_w, int base_h, GLFWwindow*& out_window) {
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
    out_window = glfwCreateWindow(
        (int)(base_w * dpi_scale), (int)(base_h * dpi_scale),
        title, nullptr, nullptr);
    return dpi_scale;
}

static void apply_style(float dpi_scale) {
    ImGui::StyleColorsDark();
    ImGuiStyle& style        = ImGui::GetStyle();
    style.ScaleAllSizes(dpi_scale);
    style.WindowRounding     = 6.0f  * dpi_scale;
    style.FrameRounding      = 4.0f  * dpi_scale;
    style.GrabRounding       = 4.0f  * dpi_scale;
    style.ScrollbarRounding  = 6.0f  * dpi_scale;
    style.FramePadding       = ImVec2(8.0f  * dpi_scale, 5.0f * dpi_scale);
    style.ItemSpacing        = ImVec2(10.0f * dpi_scale, 6.0f * dpi_scale);

    ImVec4* c = style.Colors;
    c[ImGuiCol_Header]        = ImVec4(0.15f, 0.55f, 0.60f, 0.65f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.70f, 0.75f, 0.80f);
    c[ImGuiCol_Button]        = ImVec4(0.13f, 0.50f, 0.55f, 0.90f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.65f, 0.70f, 1.00f);
    c[ImGuiCol_ButtonActive]  = ImVec4(0.10f, 0.40f, 0.45f, 1.00f);
    c[ImGuiCol_CheckMark]     = ImVec4(0.20f, 0.85f, 0.90f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.35f, 0.40f, 1.00f);
}

static void draw_server_control_panel(
    float dpi_scale,
    bool  running_now,
    const std::string& statusLog,
    std::vector<std::string>& local_ips,
    int&  selected_ip,
    char* port_buf, int port_buf_size,
    std::atomic<bool>& stop_flag,
    std::thread& server_thread,
    SharedState* state,
    DBContext& db)
{
    ImGui::SetNextWindowPos (ImVec2(10 * dpi_scale,  10 * dpi_scale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(340 * dpi_scale, 480 * dpi_scale), ImGuiCond_FirstUseEver);
    ImGui::Begin("Server Control", nullptr);

    ImGui::TextColored(ImVec4(0.20f,0.85f,0.90f,1.f), "ZMQ Location Server");
    ImGui::SameLine(0, 12);
    if (running_now)
        ImGui::TextColored(ImVec4(0.2f,1.0f,0.4f,1.f), "[RUNNING]");
    else
        ImGui::TextColored(ImVec4(0.8f,0.3f,0.3f,1.f), "[STOPPED]");
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
    ImGui::InputText("##port", port_buf, port_buf_size, ImGuiInputTextFlags_CharsDecimal);

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();

    bool can = (selected_ip >= 0 && selected_ip < (int)local_ips.size());
    if (!running_now) {
        if (!can) ImGui::BeginDisabled();
        if (ImGui::Button("  Start Server  ", ImVec2(-1, 0))) {
            stop_flag = false;
            std::string addr = "tcp://" + local_ips[selected_ip] + ":" + port_buf;
            if (server_thread.joinable()) server_thread.join();
            server_thread = std::thread(run_server, addr, state,
                                        std::ref(stop_flag), std::ref(db));
        }
        if (!can) ImGui::EndDisabled();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f,0.15f,0.15f,1.f));
        if (ImGui::Button("  Stop Server   ", ImVec2(-1, 0))) stop_flag = true;
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.7f,0.7f,0.7f,1.f), "Status: %s", statusLog.c_str());

    ImGui::End();
}

static void draw_location_network_panel(
    float dpi_scale,
    const Location&    locCopy,
    const NetworkInfo& netCopy,
    int   msgCount)
{
    ImGui::SetNextWindowPos (ImVec2(360 * dpi_scale,  10 * dpi_scale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(380 * dpi_scale, 560 * dpi_scale), ImGuiCond_FirstUseEver);
    ImGui::Begin("Location & Network", nullptr);

    auto row = [&](const char* label, const char* fmt, ...) {
        ImGui::TextDisabled("  %-14s", label);
        ImGui::SameLine(160 * dpi_scale);
        char tmp[256]; va_list a; va_start(a, fmt);
        vsnprintf(tmp, sizeof(tmp), fmt, a); va_end(a);
        ImGui::TextUnformatted(tmp);
    };

    ImGui::TextColored(ImVec4(0.20f,0.85f,0.90f,1.f), "Location");
    ImGui::Separator(); ImGui::Spacing();

    if (!locCopy.valid && msgCount == 0) {
        ImGui::TextDisabled("  Waiting for data from Android...");
    } else {
        row("Messages:",  "%d",     msgCount);
        row("Timestamp:", "%s",     locCopy.timestamp.c_str());
        row("Latitude:",  "%.7f",   locCopy.latitude);
        row("Longitude:", "%.7f",   locCopy.longitude);
        row("Altitude:",  "%.2f m", locCopy.altitude);
        row("Accuracy:",  "%.1f m", (double)locCopy.accuracy);
        row("Provider:",  "%s",     locCopy.provider.c_str());
    }

    ImGui::Spacing(); ImGui::Separator(); ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.20f,0.85f,0.90f,1.f), "Network");
    ImGui::Spacing();

    if (!netCopy.valid) {
        ImGui::TextDisabled("  No network data yet...");
    } else {
        row("Operator:", "%s (%s)", netCopy.operator_name.c_str(), netCopy.operator_numeric.c_str());
        row("Type:",     "%s",      netCopy.network_type.c_str());
        row("Signal:",   "%d dBm",  netCopy.signal_dbm);
        row("Cell ID:",  "%ld",     netCopy.cell_id);
        row("LAC/TAC:",  "%d",      netCopy.lac);
        row("MCC/MNC:",  "%d/%d",   netCopy.mcc, netCopy.mnc);
        row("Roaming:",  "%s",      netCopy.is_roaming ? "YES" : "no");
        row("Towers:",   "%d",      netCopy.visible_towers);

        if (netCopy.lte.valid) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.3f,0.9f,0.5f,1.f), "  [LTE]");
            row("  Band:",    "%d",     netCopy.lte.band);
            row("  CI:",      "%d",     netCopy.lte.cellIdentity);
            row("  EARFCN:",  "%d",     netCopy.lte.earfcn);
            row("  PCI:",     "%d",     netCopy.lte.pci);
            row("  TAC:",     "%d",     netCopy.lte.tac);
            row("  ASU:",     "%d",     netCopy.lte.asuLevel);
            row("  CQI:",     "%d",     netCopy.lte.cqi);
            row("  RSRP:",    "%d dBm", netCopy.lte.rsrp);
            row("  RSRQ:",    "%d dB",  netCopy.lte.rsrq);
            row("  RSSI:",    "%d dBm", netCopy.lte.rssi);
            row("  RSSNR:",   "%d dB",  netCopy.lte.rssnr);
            row("  TA:",      "%d",     netCopy.lte.timingAdvance);
        }
        if (netCopy.gsm.valid) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.9f,0.7f,0.2f,1.f), "  [GSM]");
            row("  CI:",    "%d", netCopy.gsm.cellIdentity);
            row("  BSIC:",  "%d", netCopy.gsm.bsic);
            row("  ARFCN:", "%d", netCopy.gsm.arfcn);
            row("  LAC:",   "%d", netCopy.gsm.lac);
            row("  PSC:",   "%d", netCopy.gsm.psc);
            row("  Dbm:",   "%d", netCopy.gsm.dbm);
            row("  RSSI:",  "%d", netCopy.gsm.rssi);
            row("  TA:",    "%d", netCopy.gsm.timingAdvance);
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
}

static void draw_log_panel(float dpi_scale, const std::vector<std::string>& lines) {
    ImGui::SetNextWindowPos (ImVec2(10 * dpi_scale,  500 * dpi_scale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(730 * dpi_scale, 200 * dpi_scale), ImGuiCond_FirstUseEver);
    ImGui::Begin("Message Log", nullptr);
    ImGui::BeginChild("##logscroll", ImVec2(0,0), false, ImGuiWindowFlags_HorizontalScrollbar);
    for (int i = (int)lines.size()-1; i >= 0; i--)
        ImGui::TextUnformatted(lines[i].c_str());
    ImGui::EndChild();
    ImGui::End();
}

static void draw_signal_plots(float dpi_scale, const SignalHistory& h) {
    ImGui::SetNextWindowPos (ImVec2(750 * dpi_scale,  10 * dpi_scale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560 * dpi_scale, 260 * dpi_scale), ImGuiCond_FirstUseEver);
    ImGui::Begin("Signal dBm", nullptr);
    if (ImPlot::BeginPlot("##sig_dbm", ImVec2(-1,-1))) {
        ImPlot::SetupAxes("Sample", "dBm");
        ImPlot::SetupAxisLimits(ImAxis_Y1, -130, -30, ImGuiCond_Always);
        plot_deque("Signal dBm", h.signal_dbm, ImVec4(0.2f,0.85f,0.9f,1.f));
        ImPlot::EndPlot();
    }
    ImGui::End();

    if (!h.lte_rsrp.empty()) {
        ImGui::SetNextWindowPos (ImVec2(750 * dpi_scale, 280 * dpi_scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(560 * dpi_scale, 280 * dpi_scale), ImGuiCond_FirstUseEver);
        ImGui::Begin("LTE Signal", nullptr);
        if (ImPlot::BeginPlot("##lte_plot", ImVec2(-1,-1))) {
            ImPlot::SetupAxes("Sample", "dBm / dB");
            ImPlot::SetupAxisLimits(ImAxis_Y1, -150, 0, ImGuiCond_Always);
            plot_deque("RSRP",  h.lte_rsrp,  ImVec4(0.2f,0.8f,0.3f,1.f));
            plot_deque("RSRQ",  h.lte_rsrq,  ImVec4(0.9f,0.6f,0.1f,1.f));
            plot_deque("RSSI",  h.lte_rssi,  ImVec4(0.7f,0.2f,0.9f,1.f));
            plot_deque("RSSNR", h.lte_rssnr, ImVec4(0.2f,0.7f,0.9f,1.f));
            ImPlot::EndPlot();
        }
        ImGui::End();
    }

    if (!h.gsm_dbm.empty()) {
        ImGui::SetNextWindowPos (ImVec2(750 * dpi_scale, 570 * dpi_scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(560 * dpi_scale, 260 * dpi_scale), ImGuiCond_FirstUseEver);
        ImGui::Begin("GSM Signal", nullptr);
        if (ImPlot::BeginPlot("##gsm_plot", ImVec2(-1,-1))) {
            ImPlot::SetupAxes("Sample", "dBm");
            ImPlot::SetupAxisLimits(ImAxis_Y1, -115, -50, ImGuiCond_Always);
            plot_deque("GSM dBm", h.gsm_dbm, ImVec4(0.9f,0.7f,0.2f,1.f));
            ImPlot::EndPlot();
        }
        ImGui::End();
    }

    if (!h.nr_ssRsrp.empty()) {
        ImGui::SetNextWindowPos (ImVec2(750 * dpi_scale, 840 * dpi_scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(560 * dpi_scale, 280 * dpi_scale), ImGuiCond_FirstUseEver);
        ImGui::Begin("NR (5G) Signal", nullptr);
        if (ImPlot::BeginPlot("##nr_plot", ImVec2(-1,-1))) {
            ImPlot::SetupAxes("Sample", "dBm / dB");
            ImPlot::SetupAxisLimits(ImAxis_Y1, -160, 0, ImGuiCond_Always);
            plot_deque("SS-RSRP", h.nr_ssRsrp, ImVec4(0.4f,0.5f,1.0f,1.f));
            plot_deque("SS-RSRQ", h.nr_ssRsrq, ImVec4(0.7f,0.4f,1.0f,1.f));
            plot_deque("SS-SINR", h.nr_ssSinr, ImVec4(0.3f,0.9f,0.9f,1.f));
            ImPlot::EndPlot();
        }
        ImGui::End();
    }
}

static void draw_osm_map(float dpi_scale, MapState& ms,
                         double& center_lat, double& center_lon, int& zoom,
                         HeatmapState& hs, DBContext& db) {
    ImGui::SetNextWindowPos (ImVec2(10 * dpi_scale, 710 * dpi_scale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(730 * dpi_scale, 500 * dpi_scale), ImGuiCond_FirstUseEver);
    ImGui::Begin("OSM Map", nullptr);

    ImGui::Text("Zoom: %d", zoom);
    ImGui::SameLine();
    if (ImGui::Button(" - ") && zoom > 0)  --zoom;
    ImGui::SameLine();
    if (ImGui::Button(" + ") && zoom < 19) ++zoom;
    ImGui::SameLine();
    ImGui::TextDisabled("(scroll to zoom, drag to pan)");

    ImVec2 plot_size = ImGui::GetContentRegionAvail();

    double cx       = lon_to_tile_x(center_lon, zoom);
    double cy       = lat_to_tile_y(center_lat, zoom);
    double half_tx  = plot_size.x / (2.0 * 256.0);
    double half_ty  = plot_size.y / (2.0 * 256.0);
    int    tc       = 1 << zoom;

    double ay_min = std::max(0.0,        cy - half_ty);
    double ay_max = std::min((double)tc, cy + half_ty);

    ImPlot::SetNextAxesLimits(cx - half_tx, cx + half_tx, ay_min, ay_max, ImGuiCond_Always);

    if (ImPlot::BeginPlot("##osm", plot_size,
                          ImPlotFlags_NoLegend | ImPlotFlags_NoMenus |
                          ImPlotFlags_NoBoxSelect | ImPlotFlags_NoFrame))
    {
        ImPlot::SetupAxes(nullptr, nullptr,
                          ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickMarks,
                          ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoGridLines | ImPlotAxisFlags_NoTickMarks | ImPlotAxisFlags_Invert);

        map_draw(ms, center_lat, center_lon, zoom, plot_size.x, plot_size.y);

        heatmap_upload_pending(hs);
        MapBounds hm_bounds = ms.get_bounds_copy();
        heatmap_request_update(hs, db, hm_bounds);
        heatmap_draw(hs, zoom);

        if (ImPlot::IsPlotHovered() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);

            double new_cx = cx - delta.x / 256.0;
            double new_cy = std::clamp(cy - delta.y / 256.0, 0.0, (double)tc);
            center_lon = tile_x_to_lon_f(new_cx, zoom);
            center_lat = std::clamp(tile_y_to_lat_f(new_cy, zoom), -85.0511, 85.0511);
        }

        if (ImPlot::IsPlotHovered()) {
            float wheel = ImGui::GetIO().MouseWheel;
            if (wheel >  0.0f && zoom < 19) ++zoom;
            if (wheel < -0.0f && zoom >  0) --zoom;
        }

        ImPlot::EndPlot();
    }

    ImGui::End();
}

static void draw_sql_panel(float dpi_scale, DBContext& db, MapState& ms) {
    ImGui::SetNextWindowPos (ImVec2(750 * dpi_scale,  570 * dpi_scale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(560 * dpi_scale, 400 * dpi_scale), ImGuiCond_FirstUseEver); 
    ImGui::Begin("Database Control", nullptr);
    
    static char buffer[4096];
    static char result_buffer[32768];
    static bool has_result = false;
    
    ImGui::Text("Query:");
    ImGui::SetNextItemWidth(540 * dpi_scale);
    if (ImGui::InputTextMultiline("##query", buffer, 4096, 
        ImVec2(540 * dpi_scale, 80 * dpi_scale),
        ImGuiInputTextFlags_CtrlEnterForNewLine)) {
    }
    
    if (ImGui::Button("RUN")) {
        has_result = db_control(db, buffer, result_buffer, sizeof(result_buffer), ms);
    }
    
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        buffer[0] = '\0';
        result_buffer[0] = '\0';
        has_result = false;
    }
    if (has_result && result_buffer[0] != '\0') {
        ImGui::Separator();
        ImGui::Text("Result:");
        ImGui::SetNextItemWidth(540 * dpi_scale);
        ImGui::InputTextMultiline("##result", result_buffer, sizeof(result_buffer),
            ImVec2(540 * dpi_scale, 200 * dpi_scale),
            ImGuiInputTextFlags_ReadOnly);
    }
    
    ImGui::End();
}

void run_gui(SharedState* state, DBContext& db) {
    if (!glfwInit()) return;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* window   = nullptr;
    float       dpi_scale = init_window("ZMQ Location Server", 920, 1080, window);
    if (!window) { glfwTerminate(); return; }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags       |= ImGuiConfigFlags_NavEnableKeyboard;
    io.FontGlobalScale    = dpi_scale;

    apply_style(dpi_scale);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    std::vector<std::string> local_ips;
    int               selected_ip = -1;
    char              port_buf[16] = "5555";
    std::atomic<bool> stop_flag(false);
    std::thread       server_thread;

    MapState map_state;
    map_init(map_state, 2);
    HeatmapState heatmap_state;
    heatmap_init(heatmap_state);
    double map_lat  = 55.01;
    double map_lon  = 82.95;
    int    map_zoom = 1;
    bool   map_follow = false;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        SignalHistory            histCopy;
        Location                 locCopy;
        NetworkInfo              netCopy;
        int                      msgCount    = 0;
        bool                     running_now = false;
        std::string              statusLog;
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

        draw_server_control_panel(dpi_scale, running_now, statusLog,
                                  local_ips, selected_ip,
                                  port_buf, sizeof(port_buf),
                                  stop_flag, server_thread, state, db);

        draw_location_network_panel(dpi_scale, locCopy, netCopy, msgCount);
        draw_log_panel(dpi_scale, historyLines);
        draw_signal_plots(dpi_scale, histCopy);
        draw_sql_panel(dpi_scale, db, map_state);

        if (map_follow && locCopy.valid) {
            map_lat = locCopy.latitude;
            map_lon = locCopy.longitude;
        }

        map_upload_pending(map_state);

        ImGui::SetNextWindowPos(ImVec2(10 * dpi_scale, 670 * dpi_scale), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(200 * dpi_scale, 300 * dpi_scale), ImGuiCond_FirstUseEver);
        ImGui::Begin("##mapctrl", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize);
        ImGui::Checkbox("Follow GPS", &map_follow);
        ImGui::SameLine(0, 20);
        if (ImGui::Button("Center on GPS") && locCopy.valid) {
            map_lat = locCopy.latitude;
            map_lon = locCopy.longitude;
        }
        heatmap_draw_controls(heatmap_state, db,
                      map_state.get_bounds_copy(),
                      dpi_scale);
        ImGui::End();

        draw_osm_map(dpi_scale, map_state, map_lat, map_lon, map_zoom, heatmap_state, db);

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

    map_shutdown(map_state);
    heatmap_shutdown(heatmap_state);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
}