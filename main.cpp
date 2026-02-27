#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstring>
#include <cstdlib>

#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_glfw.h"
#include "imgui/backends/imgui_impl_opengl3.h"

#include <GLFW/glfw3.h>


#include <zmq.h>


#include "json.hpp"

using json = nlohmann::json;

struct Location {
    double  latitude  = 0.0;
    double  longitude = 0.0;
    double  altitude  = 0.0;
    float   accuracy  = 0.0f;
    std::string provider;
    std::string raw;
    std::string timestamp;
    bool    valid     = false;
};

struct SharedState {
    std::mutex      mtx;
    Location        loc;
    int             msg_count   = 0;
    bool            server_running = false;
    std::string     log;
    std::vector<std::string> history;
};


std::vector<std::string> get_local_ips() {
    std::vector<std::string> ips;

#ifdef _APPLE_
    std::system("powershell.exe -Command 'ipconfig' > _ip_tmp.txt 2>&1");
    std::ifstream f("_ip_tmp.txt");
    std::string line, prev;
    while (std::getline(f, line)) {
        if (!line.empty() && (std::isalpha(line[0]))) prev = line;
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

    // std::system("powershell.exe -Command 'ipconfig' > _ip_tmp.txt 2>&1");
    // std::ifstream f("_ip_tmp.txt");
    // std::string line, prev;
    // while (std::getline(f, line)) {
    //     if (!line.empty() && (std::isalpha(line[0]))) prev = line;
    //     if (line.find("IPv4 Address") != std::string::npos) {
    //         auto pos = line.rfind(':');
    //         if (pos != std::string::npos) {
    //             std::string ip = line.substr(pos + 1);

    //             while (!ip.empty() && (ip.front() == ' ' || ip.front() == '\r')) ip.erase(ip.begin());
    //             while (!ip.empty() && (ip.back()  == ' ' || ip.back()  == '\r')) ip.pop_back();
    //             if (!ip.empty()) ips.push_back(ip);
    //         }
    //     }
    // }
    // std::remove("_ip_tmp.txt");


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
    msgs.push_back(entry);

    std::ofstream fo(DATA_FILE);
    fo << msgs.dump(2);
}

void run_server(const std::string& bind_addr,
                SharedState*       state,
                std::atomic<bool>& stop_flag)
{
    void* ctx  = zmq_ctx_new();
    void* sock = zmq_socket(ctx, ZMQ_REP);

    if (zmq_bind(sock, bind_addr.c_str()) != 0) {
        std::lock_guard<std::mutex> lk(state->mtx);
        state->log = "ERROR: zmq_bind failed on " + bind_addr;
        state->server_running = false;
        zmq_close(sock);
        zmq_ctx_destroy(ctx);
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

        char buf[4096] = {};
        int  len = zmq_recv(sock, buf, sizeof(buf) - 1, 0);
        if (len < 0) continue;
        buf[len] = '\0';

        std::string raw(buf);

        Location loc;
        loc.raw = raw;

        {
            auto now  = std::chrono::system_clock::now();
            std::time_t t = std::chrono::system_clock::to_time_t(now);
            char ts[32];
            std::strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
            loc.timestamp = ts;
        }

        try {
            json j = json::parse(raw);
            if (j.contains("latitude"))  loc.latitude  = j["latitude"].get<double>();
            if (j.contains("longitude")) loc.longitude = j["longitude"].get<double>();
            if (j.contains("altitude"))  loc.altitude  = j["altitude"].get<double>();
            if (j.contains("accuracy"))  loc.accuracy  = j["accuracy"].get<float>();
            if (j.contains("provider"))  loc.provider  = j["provider"].get<std::string>();
            loc.valid = true;
        } catch (...) {

            loc.valid    = false;
            loc.provider = "unknown";
        }

        int cnt;
        {
            std::lock_guard<std::mutex> lk(state->mtx);
            state->msg_count++;
            cnt = state->msg_count;
            state->loc = loc;

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

    {
        std::lock_guard<std::mutex> lk(state->mtx);
        state->server_running = false;
        state->log = "Server stopped.";
    }
}


void run_gui(SharedState* state) {
    if (!glfwInit()) return;

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    GLFWwindow* window = glfwCreateWindow(820, 640, "ZMQ Location Server", nullptr, nullptr);
    if (!window) { glfwTerminate(); return; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding  = 6.0f;
    style.FrameRounding   = 4.0f;
    style.GrabRounding    = 4.0f;
    style.ScrollbarRounding = 6.0f;
    style.FramePadding    = ImVec2(8, 5);
    style.ItemSpacing     = ImVec2(10, 6);

    ImVec4* c = style.Colors;
    c[ImGuiCol_Header]        = ImVec4(0.15f, 0.55f, 0.60f, 0.65f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.70f, 0.75f, 0.80f);
    c[ImGuiCol_Button]        = ImVec4(0.13f, 0.50f, 0.55f, 0.90f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.18f, 0.65f, 0.70f, 1.00f);
    c[ImGuiCol_ButtonActive]  = ImVec4(0.10f, 0.40f, 0.45f, 1.00f);
    c[ImGuiCol_CheckMark]     = ImVec4(0.20f, 0.85f, 0.90f, 1.00f);
    c[ImGuiCol_SliderGrab]    = ImVec4(0.20f, 0.75f, 0.80f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.35f, 0.40f, 1.00f);

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    std::vector<std::string> local_ips;
    int  selected_ip  = -1;
    char port_buf[16] = "5555";
    bool server_was_running = false;
    std::atomic<bool> stop_flag(false);
    std::thread server_thread;

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::SetNextWindowBgAlpha(1.0f);
        ImGui::Begin("##root", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::TextColored(ImVec4(0.20f, 0.85f, 0.90f, 1.0f),
                           "  ZMQ Location Server");
        ImGui::SameLine(0, 20);

        {
            std::lock_guard<std::mutex> lk(state->mtx);
            if (state->server_running)
                ImGui::TextColored(ImVec4(0.2f,1.0f,0.4f,1.0f), "[RUNNING]");
            else
                ImGui::TextColored(ImVec4(0.8f,0.3f,0.3f,1.0f), "[STOPPED]");
        }
        ImGui::Separator();

        ImGui::Spacing();
        if (ImGui::Button("Scan local IPv4 addresses")) {
            local_ips  = get_local_ips();
            selected_ip = -1;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(click to refresh)");
        ImGui::Spacing();

        if (!local_ips.empty()) {
            ImGui::Text("Select IP to bind:");
            for (int i = 0; i < (int)local_ips.size(); i++) {
                bool sel = (selected_ip == i);
                if (ImGui::RadioButton(local_ips[i].c_str(), sel))
                    selected_ip = i;
            }
        } else {
            ImGui::TextDisabled("No IPs found. Press Scan.");
        }

        ImGui::Spacing();
        ImGui::Text("Port:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputText("##port", port_buf, sizeof(port_buf),
                         ImGuiInputTextFlags_CharsDecimal);

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool running_now;
        {
            std::lock_guard<std::mutex> lk(state->mtx);
            running_now = state->server_running;
        }

        if (!running_now) {
            bool can_start = (selected_ip >= 0 && selected_ip < (int)local_ips.size());
            if (!can_start) ImGui::BeginDisabled();

            if (ImGui::Button("  Start Server  ")) {
                stop_flag = false;
                std::string addr = "tcp://" + local_ips[selected_ip] +
                                   ":" + std::string(port_buf);

                if (server_thread.joinable()) server_thread.join();
                server_thread = std::thread(run_server, addr, state,
                                            std::ref(stop_flag));
            }
            if (!can_start) ImGui::EndDisabled();
        } else {
            if (ImGui::Button("  Stop Server   ")) {
                stop_flag = true;
            }
        }

        ImGui::Spacing();

        {
            std::lock_guard<std::mutex> lk(state->mtx);
            ImGui::TextColored(ImVec4(0.7f,0.7f,0.7f,1.0f),
                               "Status: %s", state->log.c_str());
        }

        ImGui::Spacing();
        ImGui::Separator();

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.20f, 0.85f, 0.90f, 1.0f),
                           "Last received location:");
        ImGui::Spacing();

        {
            std::lock_guard<std::mutex> lk(state->mtx);
            const Location& loc = state->loc;

            auto row = [&](const char* label, const char* fmt, ...) {
                ImGui::TextDisabled("  %-14s", label);
                ImGui::SameLine(170);
                char tmp[256];
                va_list args;
                va_start(args, fmt);
                vsnprintf(tmp, sizeof(tmp), fmt, args);
                va_end(args);
                ImGui::Text("%s", tmp);
            };

            if (!loc.valid && state->msg_count == 0) {
                ImGui::TextDisabled("  Waiting for data from Android...");
            } else {
                row("Messages:",  "%d", state->msg_count);
                row("Timestamp:", "%s", loc.timestamp.c_str());
                row("Latitude:",  "%.7f", loc.latitude);
                row("Longitude:", "%.7f", loc.longitude);
                row("Altitude:",  "%.2f m", loc.altitude);
                row("Accuracy:",  "%.1f m", (double)loc.accuracy);
                row("Provider:",  "%s", loc.provider.c_str());
            }
        }

        ImGui::Spacing();
        ImGui::Separator();

        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.20f, 0.85f, 0.90f, 1.0f), "Message log:");

        float avail = ImGui::GetContentRegionAvail().y - 8;
        ImGui::BeginChild("##log", ImVec2(0, avail), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        {
            std::lock_guard<std::mutex> lk(state->mtx);
            for (int i = (int)state->history.size() - 1; i >= 0; i--) {
                ImGui::TextUnformatted(state->history[i].c_str());
            }
        }
        ImGui::EndChild();

        ImGui::End();

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
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
}

int main() {
    static SharedState shared;

    run_gui(&shared);

    return 0;
}