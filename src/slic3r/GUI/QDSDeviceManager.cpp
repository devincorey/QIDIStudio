#include "QDSDeviceManager.hpp"
#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/asio.hpp>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#pragma comment(lib, "Iphlpapi.lib")
#endif
#include <random>
#include "libslic3r/Utils.hpp"
#include "GUI_App.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/Udp.hpp"
#include "DownloadManager.hpp"
#include "GUI_Utils.hpp"
#include "DeviceCore/DevDefs.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <wx/datetime.h>



//cj_2
#if QDT_RELEASE_TO_PUBLIC
#include "../QIDI/QIDINetwork.hpp"
#include "../QIDI/P2PManager.hpp"
#endif

//cj_2
#include <wx/image.h>


namespace Slic3r {
namespace GUI {

//cj_3
namespace {

static void apply_gcode_move_speed_percent(QDSDevice& dev, const json& status, bool* out_update = nullptr)
{
    try {
        if (!status.contains("gcode_move") || !status["gcode_move"].is_object())
            return;
        const auto& gm = status["gcode_move"];
        if (!gm.contains("speed_factor") || !gm["speed_factor"].is_number())
            return;
        const double sf = gm["speed_factor"].get<double>();
        const int    pct = Slic3r::dev_speed_factor_to_snapped_percent(sf);
        if (dev.m_print_speed_display_percent != pct) {
            dev.m_print_speed_display_percent = pct;
            if (out_update)
                *out_update = true;
            else
                dev.is_update = true;
        }
    } catch (...) {
    }
}

std::string format_timelapse_file_size_b_kb_mb(std::uint64_t bytes)
{
    constexpr std::uint64_t k_kb = 1024;
    constexpr std::uint64_t k_mb = 1024ULL * 1024ULL;
    std::ostringstream oss;
    oss << std::fixed;
    if (bytes >= k_mb) {
        oss << std::setprecision(2) << (static_cast<double>(bytes) / static_cast<double>(k_mb)) << "MB";
        return oss.str();
    }
    if (bytes >= k_kb) {
        oss << std::setprecision(2) << (static_cast<double>(bytes) / static_cast<double>(k_kb)) << "KB";
        return oss.str();
    }
    oss << std::setprecision(0) << bytes << "B";
    return oss.str();
}
} // namespace

namespace pt = boost::property_tree;

//cj_5
LocalDeviceDiscovery::Snapshot LocalDeviceDiscovery::snapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Snapshot devices;
    devices.reserve(m_by_ip.size());
    for (const auto& item : m_by_ip) {
        devices.push_back(item.second);
    }
    return devices;
}

//cj_5
bool LocalDeviceDiscovery::findBySerial(const std::string& serial, LocalDiscoveredDevice& out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_by_serial.find(serial);
    if (it == m_by_serial.end()) {
        return false;
    }
    out = it->second;
    return true;
}

//cj_5
bool LocalDeviceDiscovery::isCacheFresh(std::chrono::seconds ttl) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_last_refresh == std::chrono::steady_clock::time_point{}) {
        return false;
    }
    return std::chrono::steady_clock::now() - m_last_refresh <= ttl;
}

//cj_5
void LocalDeviceDiscovery::refresh(bool force, RefreshCallback callback)
{
    Snapshot cached_devices;
    bool use_cache = false;
    bool start_lookup = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const bool fresh = m_last_refresh != std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now() - m_last_refresh <= m_cache_ttl;

        if (!force && fresh && !m_refreshing) {
            cached_devices.reserve(m_by_ip.size());
            for (const auto& item : m_by_ip) {
                cached_devices.push_back(item.second);
            }
            use_cache = true;
        }
        else {
            if (callback) {
                m_pending_callbacks.push_back(std::move(callback));
            }
            if (!m_refreshing) {
                m_refreshing = true;
                start_lookup = true;
                m_by_serial.clear();
                m_by_ip.clear();
            }
        }
    }

    if (use_cache) {
        if (callback) {
            callback(std::move(cached_devices));
        }
        return;
    }

    if (!start_lookup) {
        return;
    }

    Udp::TxtKeys udp_txt_keys{ "version", "model" };
    Udp::Ptr udp = Udp("octoprint")
        .set_txt_keys(std::move(udp_txt_keys))
        .set_retries(3)
        .set_timeout(4)
        .on_udp_reply([this](UdpReply&& reply) {
            LocalDiscoveredDevice device;
            device.serial_number = reply.serial_number;
            device.ip = reply.service_name;
            device.name = reply.hostname;
            device.model = reply.model_name;
            device.raw_payload = reply.raw_payload;
            device.legacy_device = reply.legacy_device;
            device.last_seen = std::chrono::steady_clock::now();

            mergeDevice(std::move(device));
        })
        .on_complete([this]() {
            finishRefresh();
        })
        .lookup();

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_udp = std::move(udp);
    }
}

//cj_5
void LocalDeviceDiscovery::mergeDevice(LocalDiscoveredDevice device)
{
    if (device.ip.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    device.last_seen = std::chrono::steady_clock::now();
    if (!device.serial_number.empty()) {
        m_by_serial[device.serial_number] = device;
    }
    m_by_ip[device.ip] = std::move(device);
}

//cj_5
void LocalDeviceDiscovery::finishRefresh()
{
    std::vector<RefreshCallback> callbacks;
    Snapshot devices;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_refreshing = false;
        m_last_refresh = std::chrono::steady_clock::now();
        callbacks = std::move(m_pending_callbacks);
        m_pending_callbacks.clear();

        devices.reserve(m_by_ip.size());
        for (const auto& item : m_by_ip) {
            devices.push_back(item.second);
        }
    }

    for (auto& callback : callbacks) {
        if (callback) {
            callback(devices);
        }
    }
}

// ─────────────────────────────────────────────────────────────────
// SSDPDiscovery implementation                                     //cj_5
// ─────────────────────────────────────────────────────────────────
namespace asio = boost::asio;
using asio::ip::udp;

namespace {

const char*            SSDP_MCAST_ADDR   = "239.255.255.250";
const unsigned short   SSDP_MCAST_PORT   = 5863;
const int              SSDP_TIMEOUT_SEC  = 10;
const int              SSDP_RETRIES      = 2;

std::string build_msearch()
{
    std::ostringstream oss;
    oss << "M-SEARCH * HTTP/1.1\r\n"
        << "HOST: " << SSDP_MCAST_ADDR << ":" << SSDP_MCAST_PORT << "\r\n"
        << "MAN: \"ssdp:discover\"\r\n"
        << "ST: ssdp:all\r\n"
        << "MX: " << SSDP_TIMEOUT_SEC << "\r\n"
        << "\r\n";
    return oss.str();
}

bool parse_ssdp_notify(const std::string& raw, LocalDiscoveredDevice& out)
{
    std::istringstream stream(raw);
    std::string line;
    std::unordered_map<std::string, std::string> headers;

    if (!std::getline(stream, line))
        return false;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty()) continue;

        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = line.substr(0, colon);
        std::string val = line.substr(colon + 1);
        if (!val.empty() && val.front() == ' ') val.erase(0, 1);
        if (!val.empty() && val.back() == '\r') val.pop_back();
        headers[key] = val;
    }

    auto usn_it = headers.find("USN");
    auto loc_it = headers.find("Location");
    if (usn_it == headers.end() || loc_it == headers.end())
        return false;

    out.serial_number = usn_it->second;

    std::string loc = loc_it->second;
    if (loc.find("http://") == 0)      loc = loc.substr(7);
    else if (loc.find("https://") == 0) loc = loc.substr(8);
    const size_t slash = loc.find('/');
    if (slash != std::string::npos) loc = loc.substr(0, slash);
    const size_t colon2 = loc.find(':');
    if (colon2 != std::string::npos) loc = loc.substr(0, colon2);
    out.ip = loc;

    auto model_it = headers.find("DevModel.qidi.com");
    if (model_it != headers.end()) out.model = model_it->second;

    auto name_it = headers.find("DevName.qidi.com");
    if (name_it != headers.end()) out.name = name_it->second;

    out.raw_payload = raw;
    out.last_seen    = std::chrono::steady_clock::now();
    out.legacy_device = false;
    return true;
}

} // anonymous namespace

struct SSDPDiscovery::priv
{
    std::shared_ptr<asio::io_context> io_ctx;
    std::unique_ptr<std::thread>       io_thread;
    std::atomic<bool>                  stopping{ false };

    priv() : io_ctx(std::make_shared<asio::io_context>()) {}
};

SSDPDiscovery::SSDPDiscovery()
    : p(std::make_unique<priv>())
{
}

SSDPDiscovery::~SSDPDiscovery()
{
    stop();
}

void SSDPDiscovery::stop()
{
    p->stopping = true;
    if (p->io_ctx) p->io_ctx->stop();
    if (p->io_thread && p->io_thread->joinable())
        p->io_thread->join();
}

bool SSDPDiscovery::isCacheFresh(std::chrono::seconds ttl) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_last_refresh == std::chrono::steady_clock::time_point{})
        return false;
    return std::chrono::steady_clock::now() - m_last_refresh <= ttl;
}

SSDPDiscovery::Snapshot SSDPDiscovery::snapshot() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Snapshot devices;
    devices.reserve(m_by_ip.size());
    for (const auto& item : m_by_ip)
        devices.push_back(item.second);
    return devices;
}

bool SSDPDiscovery::findBySerial(const std::string& serial,
                                 LocalDiscoveredDevice& out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_by_serial.find(serial);
    if (it == m_by_serial.end()) return false;
    out = it->second;
    return true;
}

//cj_5
bool SSDPDiscovery::findByIP(const std::string& ip,
                              LocalDiscoveredDevice& out) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto it = m_by_ip.find(ip);
    if (it != m_by_ip.end()) {
        out = it->second;
        return true;
    }
    
    return false;
}

void SSDPDiscovery::mergeDevice(LocalDiscoveredDevice device)
{
    if (device.ip.empty()) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    device.last_seen = std::chrono::steady_clock::now();
    if (!device.serial_number.empty())
        m_by_serial[device.serial_number] = device;
    m_by_ip[device.ip] = std::move(device);
}

void SSDPDiscovery::finishRefresh()
{
    std::vector<RefreshCallback> callbacks;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_refreshing = false;
        m_last_refresh = std::chrono::steady_clock::now();
        callbacks = std::move(m_pending_callbacks);
        m_pending_callbacks.clear();
    }
    Snapshot devices = snapshot();
    BOOST_LOG_TRIVIAL(trace)
        << "[SSDP] discovery finished, found " << devices.size() << " device(s)";
    for (auto& cb : callbacks) {
        if (cb) cb(devices);
    }
}

void SSDPDiscovery::refresh(bool force, RefreshCallback callback)
{
    
    Snapshot cached_devices;
    bool use_cache = false;
    bool start_lookup = false;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const bool fresh = m_last_refresh != std::chrono::steady_clock::time_point{} &&
            std::chrono::steady_clock::now() - m_last_refresh <= m_cache_ttl;

        if (!force && fresh && !m_refreshing) {
            cached_devices.reserve(m_by_ip.size());
            for (const auto& item : m_by_ip)
                cached_devices.push_back(item.second);
            use_cache = true;
        } else {
            if (callback) m_pending_callbacks.push_back(std::move(callback));
            if (!m_refreshing) {
                m_refreshing  = true;
                start_lookup  = true;
                m_by_serial.clear();
                m_by_ip.clear();
            }
        }
    }

    if (use_cache) {
        if (callback) callback(std::move(cached_devices));
        return;
    }
    if (!start_lookup) return;

    //cj_5 Stop previous lookup thread before starting a new one, otherwise
    // std::thread destructor will call std::terminate on a joinable thread.
    stop();
    p->stopping = false;

    p->io_thread = std::make_unique<std::thread>([this]() {
        try {
            //cj_5 Re-create io_context each refresh so previous async ops are
            // fully cancelled after stop().
            p->io_ctx = std::make_shared<asio::io_context>();
            auto& io = *p->io_ctx;

            auto recv_buf = std::make_shared<std::vector<char>>(8192);
            const std::string msearch = build_msearch();

            // Receive loop – re-arms until error or stopping.
            std::function<void(udp::socket*)> start_receive;
            start_receive = [this, recv_buf, &start_receive](udp::socket* sock) {
                sock->async_receive(
                    asio::buffer(*recv_buf),
                    [this, recv_buf, sock, &start_receive](const boost::system::error_code& err, size_t bytes) {
                        if (err || bytes == 0 || p->stopping) {
                            if (!p->stopping) start_receive(sock);
                            return;
                        }
                        LocalDiscoveredDevice dev;
                        if (parse_ssdp_notify(std::string(recv_buf->data(), bytes), dev)) {
                            BOOST_LOG_TRIVIAL(trace)
                                << "[SSDP] discovered device: serial="
                                << dev.serial_number << " ip=" << dev.ip
                                << " model=" << dev.model
                                << " name=" << dev.name;
                            mergeDevice(std::move(dev));
                        }
                        start_receive(sock);
                    });
            };

            //cj_5 Enumerate local IPv4 addresses and create a socket per interface.
            // Binding to 0.0.0.0 causes the OS to pick a single default interface,
            // which is wrong on multi-homed machines (WiFi+Ethernet+VPN).
            std::vector<asio::ip::address_v4> local_addrs;
#ifdef _WIN32
            {
                ULONG bufLen = 0;
                GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, nullptr, &bufLen);
                if (bufLen > 0) {
                    std::vector<BYTE> buf(bufLen);
                    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
                    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &bufLen) == NO_ERROR) {
                        for (auto* a = adapters; a; a = a->Next) {
                            if (a->OperStatus != IfOperStatusUp) continue;
                            for (auto* ua = a->FirstUnicastAddress; ua; ua = ua->Next) {
                                if (ua->Address.lpSockaddr->sa_family == AF_INET) {
                                    auto* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
                                    asio::ip::address_v4 ip(ntohl(sin->sin_addr.s_addr));
                                    if (!ip.is_loopback()) {
                                        bool dup = false;
                                        for (auto& ex : local_addrs) { if (ex == ip) { dup = true; break; } }
                                        if (!dup) local_addrs.push_back(ip);
                                    }
                                }
                            }
                        }
                    }
                }
            }
#endif
            if (local_addrs.empty())
                local_addrs.push_back(asio::ip::address_v4::any());

            std::vector<std::unique_ptr<udp::socket>> socks;
            boost::system::error_code ec;
            const auto mcast_addr = asio::ip::make_address(SSDP_MCAST_ADDR).to_v4();
            for (const auto& iface_addr : local_addrs) {
                auto sock = std::make_unique<udp::socket>(io);
                sock->open(udp::v4(), ec);
                if (ec) continue;
                sock->set_option(asio::socket_base::reuse_address(true), ec);
                sock->bind(udp::endpoint(iface_addr, SSDP_MCAST_PORT), ec);
                if (ec) {
                    BOOST_LOG_TRIVIAL(info) << "[SSDP] bind " << iface_addr.to_string()
                                            << " failed: " << ec.message() << " (continuing)";
                    continue;
                }
                try {
                    sock->set_option(asio::ip::multicast::join_group(mcast_addr, iface_addr));
                } catch (const std::exception& e) {
                    BOOST_LOG_TRIVIAL(info) << "[SSDP] join_group " << iface_addr.to_string()
                                            << " failed: " << e.what() << " (continuing)";
                    continue;
                }
                BOOST_LOG_TRIVIAL(info) << "[SSDP] listening on " << iface_addr.to_string()
                                        << ":" << SSDP_MCAST_PORT;

                start_receive(sock.get());
                sock->send_to(asio::buffer(msearch),
                    udp::endpoint(asio::ip::make_address(SSDP_MCAST_ADDR), SSDP_MCAST_PORT), 0, ec);
                socks.push_back(std::move(sock));
            }

            // Retry loop — send M-SEARCH on all sockets
            for (int retry = 0; retry < SSDP_RETRIES && !p->stopping; ++retry) {
                if (retry > 0) {
                    for (auto& s : socks) {
                        s->send_to(asio::buffer(msearch),
                            udp::endpoint(asio::ip::make_address(SSDP_MCAST_ADDR), SSDP_MCAST_PORT), 0, ec);
                    }
                }
                asio::steady_timer timer(io);
                timer.expires_after(std::chrono::seconds(SSDP_TIMEOUT_SEC));
                timer.async_wait([&io](const boost::system::error_code&) { io.stop(); });
                io.run();
                if (p->stopping) break;
                io.restart();
            }
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "[SSDP] lookup exception: " << e.what();
        }
        finishRefresh();
    });
}

template<typename T>
bool is_json_type(const json& j)
{
	if constexpr (std::is_same_v<T, int> ||
		std::is_same_v<T, long> ||
		std::is_same_v<T, short>) {
		return j.is_number_integer();
	}
	else if constexpr (std::is_same_v<T, double> ||
		std::is_same_v<T, float>) {
		return j.is_number_float();
	}
	else if constexpr (std::is_same_v<T, bool>) {
		return j.is_boolean();
	}
	else if constexpr (std::is_same_v<T, std::string> ||
		std::is_same_v<T, const char*>) {
		return j.is_string();
	}
	else if constexpr (std::is_same_v<T, json>) {
		return true;  // 任何 JSON 对象都匹配
	}
	else {
		// 用户自定义类型，需要特殊处理
		return false;
	}
}

template<typename T>
void twoStageParse1(const json& status, T& target, std::string first, std::string second, bool& is_update)
{
	if (status.contains(first) && status[first].is_object()
		&& status[first].contains(second) && is_json_type<T>(status[first][second])) {
		if (target != status[first][second].get<T>()) {
			target = status[first][second].get<T>();
            is_update = true;
		}
	}
}


QDSDevice::QDSDevice(const std::string dev_id, const std::string& dev_name, const std::string& dev_ip, const std::string& dev_url, const std::string& dev_type)
    : m_id(dev_id), m_name(dev_name), m_ip(dev_ip), m_type(dev_type)
    , m_boxData(17), m_boxTemperature(4, 0.0), m_boxHumidity(4, 0)
{
    //y79
    m_url = "ws://" + dev_url + ":7125/websocket";

    last_update = std::chrono::steady_clock::now();
}

void QDSDevice::updateByJsonData(const json& status)
{
    if (status.contains("print_stats_manager")) {
//          BOOST_LOG_TRIVIAL(trace) << "-------------------------------------------------";
//          BOOST_LOG_TRIVIAL(trace) << status;
//  		BOOST_LOG_TRIVIAL(trace) << "************************************************" <<endl;

    }
    // cj_5  When 'main_status' exists, use 'main_status'; otherwise, use 'sub_status'.
	parseJsonForPath(status, m_print_msg, "/print_stats_manager/sub_status");
    std::string main_status;
	parseJsonForPath(status, main_status, "/print_stats_manager/main_status");
    if (main_status == "printing") {
        m_print_msg = "Printing";
    }


	if (status.contains("print_stats") && status["print_stats"].contains("state")) {

		if (m_status != status["print_stats"]["state"].get<std::string>()) {
			is_update = true;
			m_status = status["print_stats"]["state"].get<std::string>();

            // 处于未打印状态需要自己将打印信息恢复默认值
            if (m_status == "standby") {
                m_print_progress = "N/A";
                m_print_filename = "";
                m_print_png_url = "";
                m_print_cur_layer = 0;
                m_print_total_layer = 0;
                m_print_progress_float = 0.0;
                m_print_duration = "";
                m_print_total_time = "";
                m_filament_weight = "";
                m_print_msg = "";
            }
		}

	}

	if (status.contains("print_stats") && status["print_stats"].contains("info")
		&& status["print_stats"]["info"].contains("total_layer") && status["print_stats"]["info"]["total_layer"].is_number_integer())
	{
		if (m_print_total_layer != status["print_stats"]["info"]["total_layer"].get<int>()) {
			is_update = true;
			m_print_total_layer = status["print_stats"]["info"]["total_layer"].get<int>();
		}
	}
	if (status.contains("print_stats") && status["print_stats"].contains("info")
		&& status["print_stats"]["info"].contains("current_layer") && status["print_stats"]["info"]["current_layer"].is_number_integer())
	{
		if (m_print_cur_layer != status["print_stats"]["info"]["current_layer"].get<int>()) {
			is_update = true;
			m_print_cur_layer = status["print_stats"]["info"]["current_layer"].get<int>();
		}
	}

	//cj_4
	if (status.contains("print_stats") && status["print_stats"].contains("plateindex")) {
		int plate_idx = std::stoi(status["print_stats"]["plateindex"].get<std::string>());
		if (m_plate_index != plate_idx) {
			is_update = true;
			m_plate_index = plate_idx;
		}
	}
	if (status.contains("print_stats") && status["print_stats"].contains("filename")) {

		if (m_print_filename != status["print_stats"]["filename"].get<std::string>()) {
			is_update = true;
			m_print_filename = status["print_stats"]["filename"].get<std::string>();
            
		}

	}

	twoStageParseIntToString(status, m_print_total_duration, "print_stats", "total_duration");
	twoStageParseIntToString(status, m_print_duration, "print_stats", "print_duration");

	twoStageParseIntToString(status, m_bed_temperature, "heater_bed", "temperature");
	twoStageParseIntToString(status, m_target_bed, "heater_bed", "target");
	twoStageParseIntToString(status, m_extruder_temperature, "extruder", "temperature");
	twoStageParseIntToString(status, m_target_extruder, "extruder", "target");
	twoStageParseIntToString(status, m_chamber_temperature, "heater_generic chamber", "temperature");
	twoStageParseIntToString(status, m_target_chamber, "heater_generic chamber", "target");



	if (status.contains("display_status") && status["display_status"].contains("progress")) {

		if (m_print_progress_float != status["display_status"]["progress"].get<float>()) {
			is_update = true;
			m_print_progress_float = status["display_status"]["progress"].get<float>();
		}
			//cj_4
			std::string progress_str = std::to_string(status["display_status"]["progress"].get<int>());
			if (m_print_progress != progress_str) {
				is_update = true;
				m_print_progress = progress_str;
			}
	}
    const bool has_save_variables = status.contains("save_variables") && status["save_variables"].is_object();
    bool has_box_stepper = false;
    if (status.is_object()) {
        for (auto entry = status.begin(); entry != status.end(); ++entry) {
            if (entry.key().rfind("box_stepper ", 0) == 0 && entry.value().is_object() && entry.value().contains("runout_button")) {
                has_box_stepper = true;
                break;
            }
        }
    }
    if (has_save_variables || has_box_stepper) {
        updateBoxDataByJson(status);
    }

	if (status.contains("output_pin caselight") && status["output_pin caselight"].contains("value")) {

		if (m_case_light != bool(status["output_pin caselight"]["value"].get<float>())) {
			is_update = true;
            m_case_light = bool(status["output_pin caselight"]["value"].get<float>());
		}
	}

	//cj_3
	if (status.contains("output_pin polar_cooler") && status["output_pin polar_cooler"].contains("value")) {
        const bool pin_on = bool(status["output_pin polar_cooler"]["value"].get<float>());
        if (m_polar_cooler.load() != pin_on) {
			is_update = true;
			m_polar_cooler = pin_on;
			//cj_4
			m_polar_cooler_dirty_for_ui = true;
		}
	}
	twoStageParse(status, m_auxiliary_fan_speed, "fan_generic auxiliary_cooling_fan", "speed");
	twoStageParse(status, m_chamber_fan_speed, "fan_generic chamber_circulation_fan", "speed");
	twoStageParse(status, m_cooling_fan_speed, "fan_generic cooling_fan", "speed");
	twoStageParse(status, m_home_axes, "toolhead", "homed_axes");
	twoStageParse(status, m_extruder_filament, "filament_switch_sensor filament_switch_sensor", "filament_detected");

    apply_gcode_move_speed_percent(*this, status, nullptr);

    for (int i = 0; i < 4; ++i) {
        std::string key = "aht20_f heater_box" + std::to_string(i + 1);
		if (status.contains(key) ) {
            if (status[key].contains("temperature")) {
                if (m_boxTemperature[i] != int(status[key]["temperature"].get<float>())) {
                    m_is_update_box_temp = true;
                    m_boxTemperature[i] = int(status[key]["temperature"].get<float>());
                }
            }
			if (status[key].contains("humidity")) {
				if (m_boxHumidity[i] != status[key]["humidity"].get<int>()) {
                    m_is_update_box_temp = true;
                    m_boxHumidity[i] = status[key]["humidity"].get<int>();

				}
			}
		}
    }
	//cj_4
	if (status.contains("exclude_object") && status["exclude_object"].is_object()) {
		const auto& eo = status["exclude_object"];
		if (eo.contains("excluded_objects") && eo["excluded_objects"].is_array()) {
			std::vector<std::string> new_list;
			for (const auto& obj : eo["excluded_objects"]) {
				if (obj.is_string())
					new_list.push_back(obj.get<std::string>());
			}
			if (m_excluded_objects != new_list) {
				is_update = true;
				m_excluded_objects = std::move(new_list);
			}
		}
	}
}

void QDSDevice::updateBoxDataByJson(const json &status)
{
	std::lock_guard<std::mutex> config_lock(m_config_mtx);
	if (m_filamentConfig.empty()) {
		// Preserve the latest status until the catalog is ready. The catalog
		// loader drains this payload after releasing m_config_mtx.
		m_pending_save_variables = status;
		m_has_pending_box_update = true;
		return;
	}
	const json empty_save_variables = json::object();
	const json &save_variables = status.contains("save_variables") && status["save_variables"].is_object()
		? status["save_variables"] : empty_save_variables;
	if (m_boxData.size() < 17)
        m_boxData.resize(17);

	QDSBoxSync::BoxSnapshotPatch snapshot_patch;
	bool external_touched = false;

	for (int i = 0; i < 17; ++i) {
		const std::string serial = "slot" + std::to_string(i);
		QDSBoxSync::RawSlotPatch slot_patch;
		slot_patch.slot_index = i;
		bool slot_touched = false;

		const std::string filament_key = "filament_" + serial;
		if (save_variables.contains(filament_key) && save_variables[filament_key].is_number_integer()) {
			const int filament_index = save_variables[filament_key].get<int>();
			m_boxData[i].filament_idex = -1;
			m_boxData[i].name.clear();
			m_boxData[i].type.clear();
			if (QDSBoxSync::valid_catalog_index(filament_index, m_filamentConfig.size())) {
                m_boxData[i].filament_idex = filament_index;
				m_boxData[i].name = m_filamentConfig[filament_index].name;
				m_boxData[i].type = m_filamentConfig[filament_index].type;
			} else {
				BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": ignored out-of-range filament index for slot " << i;
			}
			slot_patch.filament_index = m_boxData[i].filament_idex;
			slot_patch.material_name = m_boxData[i].name;
			slot_patch.material_type = m_boxData[i].type;
			slot_touched = true;
		}

		const std::string vendor_key = "vendor_" + serial;
		if (save_variables.contains(vendor_key) && save_variables[vendor_key].is_number_integer()) {
			const int vendor_index = save_variables[vendor_key].get<int>();
			m_boxData[i].vendor_index = -1;
			m_boxData[i].vendor.clear();
			if (QDSBoxSync::valid_catalog_index(vendor_index, m_filamentConfig.size())) {
				m_boxData[i].vendor_index = vendor_index;
				m_boxData[i].vendor = m_filamentConfig[vendor_index].vendor;
			} else {
				BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": ignored out-of-range vendor index for slot " << i;
			}
			slot_patch.vendor_index = m_boxData[i].vendor_index;
			slot_touched = true;
		}

		const std::string colour_key = "color_" + serial;
		if (save_variables.contains(colour_key) && save_variables[colour_key].is_number_integer()) {
			const int colour_index = save_variables[colour_key].get<int>();
			m_boxData[i].colour_index = -1;
			m_boxData[i].colorHexCode.clear();
			if (QDSBoxSync::valid_catalog_index(colour_index, m_filamentConfig.size())) {
				m_boxData[i].colour_index = colour_index;
				m_boxData[i].colorHexCode = m_filamentConfig[colour_index].colorHexCode;
			} else {
				BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": ignored out-of-range colour index for slot " << i;
			}
			slot_patch.colour_present = true;
			if (!m_boxData[i].colorHexCode.empty())
				slot_patch.colour = m_boxData[i].colorHexCode;
			slot_touched = true;
		}

		if (i < QDSBoxSync::max_box_slots) {
            const std::string box_stepper = "box_stepper " + serial;
			if (status.contains(box_stepper) && status[box_stepper].is_object() &&
				status[box_stepper].contains("runout_button") && status[box_stepper]["runout_button"].is_number_integer()) {
				m_boxData[i].hasMaterial = status[box_stepper]["runout_button"].get<int>() == 0;
				slot_patch.occupied = m_boxData[i].hasMaterial;
				slot_touched = true;
			}
		}

		if (i < QDSBoxSync::max_box_slots && slot_touched)
			snapshot_patch.slots.emplace_back(std::move(slot_patch));
		if (i == 16 && slot_touched)
			external_touched = true;
	}

	// Slot 16 is the external spool. Firmware commonly sends an empty record;
	// only expose it when an update carries an actual filament identity.
	if (external_touched) {
		m_boxData[16].hasMaterial = m_boxData[16].filament_idex > 0 &&
		                            (!m_boxData[16].type.empty() || !m_boxData[16].name.empty());
	}

	if (save_variables.contains("box_count") && save_variables["box_count"].is_number_integer()) {
		m_box_count = std::clamp(save_variables["box_count"].get<int>(), 0, QDSBoxSync::max_box_count);
		snapshot_patch.box_count = m_box_count;
	}

	if (save_variables.contains("last_load_slot") && save_variables["last_load_slot"].is_string()) {
        m_cur_slot = save_variables["last_load_slot"].get<std::string>();
		snapshot_patch.loaded_slot_present = true;
		snapshot_patch.loaded_slot.reset();
		if (m_cur_slot.rfind("slot", 0) == 0) {
			try {
				const std::string slot_suffix = m_cur_slot.substr(4);
				size_t parsed = 0;
				const int loaded_slot = std::stoi(slot_suffix, &parsed);
				if (parsed != slot_suffix.size())
					throw std::invalid_argument("loaded slot contains trailing characters");
				snapshot_patch.loaded_slot = loaded_slot;
			} catch (const std::exception &) {
				BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": invalid loaded-slot value";
			}
		}
	}

    const int auto_read = getJsonCurStageToInt(save_variables, "auto_read_rfid");
    if (auto_read != -1)
        m_auto_read_rfid = bool(auto_read);

    const int init_detect = getJsonCurStageToInt(save_variables, "auto_init_detect");
    if (init_detect != -1)
        m_init_detect = bool(init_detect);

    const int auto_reload = getJsonCurStageToInt(save_variables, "auto_reload_detect");
    if (auto_reload != -1)
        m_auto_reload_detect = bool(auto_reload);

    m_box_snapshot_input = QDSBoxSync::merge_snapshot_patch(std::move(m_box_snapshot_input), snapshot_patch);
	if (external_touched && m_boxData[16].hasMaterial) {
		QDSBoxSync::RawSlot external;
		external.slot_index     = 16;
		external.occupied       = true;
		external.vendor_index   = m_boxData[16].vendor_index;
		external.filament_index = m_boxData[16].filament_idex;
		external.material_name  = m_boxData[16].name;
		external.material_type  = m_boxData[16].type;
		if (!m_boxData[16].colorHexCode.empty())
			external.colour = m_boxData[16].colorHexCode;
		m_box_snapshot_input.external_spool = std::move(external);
	} else if (external_touched) {
		m_box_snapshot_input.external_spool.reset();
	}
    //y78
    std::vector<int> slot_state(17);
    std::vector<int> slot_id(17);
    std::vector<std::string> filament_id(17);
    std::vector<std::string> filament_colors(17);
    std::vector<std::string> filament_type(17);

    //y83
    std::set<std::pair<std::string, std::string>> mapping;
    auto vendor_presets = wxGetApp().preset_bundle->printers.get_presets();
    for(auto preset : vendor_presets){
        std::string printer_model = preset.config.opt_string("printer_model");
        std::string box_id = preset.config.opt_string("box_id");

        if (!printer_model.empty() && !box_id.empty()) {
            mapping.emplace(printer_model, box_id);
        }
    }
    
    for(int i = 0; i < 17; ++i){
        if(m_boxData[i].hasMaterial){
            slot_state[i] = m_boxData[i].hasMaterial;
            slot_id[i] = i;
            filament_type[i] = m_boxData[i].type;
            filament_colors[i] = m_boxData[i].colorHexCode;

            std::string slot_vendor = m_boxData[i].vendor;

            //y83
            std::string test_type = "";
            auto it = std::find_if(mapping.begin(), mapping.end(),
                [&](const std::pair<std::string, std::string>& pair) {
                    return pair.first == m_type;
                });

            if (it != mapping.end()) {
                test_type = it->second;
            }

            std::string test_vendor = slot_vendor == "QIDI" ? "1" : "0";
            std::string tset_idx = std::to_string(m_boxData[i].filament_idex);
            std::string test_id = "QD_" + test_type + "_" +  test_vendor + "_" + tset_idx;
            filament_id[i] = test_id;
        }
    }
    m_filament_colors = filament_colors;
    m_filament_type = filament_type;
    m_filament_id = filament_id;
    m_slot_id = slot_id;
    m_slot_state = slot_state;

	if (snapshot_patch.slots.empty() && !m_box_snapshot_input.slots.empty())
		BOOST_LOG_TRIVIAL(debug) << __FUNCTION__ << ": partial save_variables update preserved the prior Box slot snapshot";

    //y83
    std::string sig;
    sig.reserve(384);
    for (int i = 0; i < 17; ++i) {
        const Filament& f = m_boxData[i];
        sig += (f.hasMaterial ? '1' : '0');
        sig += ':';
        sig += std::to_string(f.filament_idex);
        sig += ':';
        sig += f.name;
        sig += ':';
        sig += f.type;
        sig += ':';
        sig += f.vendor;
        sig += ':';
        sig += f.colorHexCode;
        sig += ';';
    }
    sig += std::to_string(m_box_count);
    sig += '|';
    sig += m_cur_slot;
    sig += '|';
    sig += (m_auto_read_rfid ? '1' : '0');
    sig += (m_init_detect ? '1' : '0');
    sig += (m_auto_reload_detect ? '1' : '0');
    if (sig != m_box_signature) {
        m_box_signature = std::move(sig);
        box_is_update = true;
    }
}

void QDSDevice::updateFilamentConfig()
{
    {
        std::lock_guard<std::mutex> lock(m_config_mtx);
        if (m_is_init_filamentConfig)
            return;
    }

    auto apply_filament_catalog = [this](const json &catalog) -> bool {
        if (!catalog.is_object()) {
            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": filament catalog was not an object";
            return false;
        }

        constexpr size_t catalog_size = 100;
        std::vector<Filament> updated_catalog(catalog_size);
        auto apply_catalog_field = [&catalog, &updated_catalog](const char *name, auto apply_value) {
            if (!catalog.contains(name) || !catalog[name].is_object()) {
                BOOST_LOG_TRIVIAL(warning) << "QDS filament catalog omitted field " << name;
                return false;
            }
            try {
                for (const auto &element : catalog[name].items()) {
                    size_t parsed = 0;
                    const long index = std::stol(element.key(), &parsed);
                    if (parsed != element.key().size() || index < 0 ||
                        static_cast<size_t>(index) >= updated_catalog.size()) {
                        BOOST_LOG_TRIVIAL(warning) << "QDS filament catalog ignored out-of-range "
                                                   << name << " index " << element.key();
                        return false;
                    }
                    apply_value(updated_catalog[static_cast<size_t>(index)], element.value());
                }
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(warning) << "QDS filament catalog rejected field "
                                           << name << ": " << e.what();
                return false;
            }
            return true;
        };

        bool valid = true;
        valid &= apply_catalog_field("filament", [](Filament &item, const json &value) { item.name = value.get<std::string>(); });
        valid &= apply_catalog_field("type", [](Filament &item, const json &value) { item.type = value.get<std::string>(); });
        valid &= apply_catalog_field("colordict", [](Filament &item, const json &value) { item.colorHexCode = value.get<std::string>(); });
        valid &= apply_catalog_field("vendor_list", [](Filament &item, const json &value) { item.vendor = value.get<std::string>(); });
        valid &= apply_catalog_field("min_temp", [](Filament &item, const json &value) { item.minTemp = value.get<int>(); });
        valid &= apply_catalog_field("max_temp", [](Filament &item, const json &value) { item.maxTemp = value.get<int>(); });
        valid &= apply_catalog_field("box_min_temp", [](Filament &item, const json &value) { item.boxMinTemp = value.get<int>(); });
        valid &= apply_catalog_field("box_max_temp", [](Filament &item, const json &value) { item.boxMaxTemp = value.get<int>(); });
        if (!valid)
            return false;

        json pending_status;
        bool has_pending_status = false;
        {
            std::lock_guard<std::mutex> lock(m_config_mtx);
            if (m_is_init_filamentConfig)
                return true;

            m_filamentConfig = std::move(updated_catalog);
            m_is_init_filamentConfig = true;
            if (m_has_pending_box_update.exchange(false)) {
                pending_status = std::move(m_pending_save_variables);
                m_pending_save_variables = json();
                has_pending_status = true;
            }
        }

        // updateBoxDataByJson takes m_config_mtx, so drain the deferred payload
        // only after the catalog commit lock has been released.
        if (has_pending_status)
            updateBoxDataByJson(pending_status);
        return true;
    };

    auto future1 = std::async(std::launch::async, [this, apply_filament_catalog]() {
        std::string result_body;

        if (active_p2p) {
#if QDT_RELEASE_TO_PUBLIC
            auto &qds_p2p = P2PManager::instance();
            if (!qds_p2p.isConnected())
                return;

            std::mutex sync_mutex;
            std::condition_variable sync_cv;
            bool received = false;
            const int text_token = qds_p2p.onText(
                [&](uint8_t, int64_t, int32_t, const uint8_t *data, size_t len) {
                    {
                        std::lock_guard<std::mutex> lock(sync_mutex);
                        result_body.assign(reinterpret_cast<const char *>(data), len);
                        received = true;
                    }
                    sync_cv.notify_one();
                });

            const int64_t request_id =
                static_cast<int64_t>(std::chrono::system_clock::now().time_since_epoch().count());
            bool sent = false;
            for (int retry = 0; retry < 5; ++retry) {
                if (qds_p2p.sendTextCommand(R"({"method":"fetch_offical_filament_list"})", request_id) >= 0) {
                    sent = true;
                    break;
                }
                std::this_thread::sleep_for(500ms);
            }
            if (!sent) {
                BOOST_LOG_TRIVIAL(error) << "QDSDevice: failed to request the filament catalog over P2P";
                qds_p2p.off(text_token);
                return;
            }

            {
                std::unique_lock<std::mutex> lock(sync_mutex);
                if (!sync_cv.wait_for(lock, std::chrono::seconds(30), [&] { return received; })) {
                    BOOST_LOG_TRIVIAL(error) << "QDSDevice: filament catalog P2P request timed out";
                    qds_p2p.off(text_token);
                    return;
                }
            }
            qds_p2p.off(text_token);

            try {
                if (!result_body.empty())
                    apply_filament_catalog(json::parse(result_body));
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(warning) << "QDS filament catalog P2P response was invalid: " << e.what();
            }
#endif
            return;
        }

        if (is_net_device) {
#if QDT_RELEASE_TO_PUBLIC
            HttpData http_data;
            json request;
            request["serialNumber"] = m_id;
            http_data.body = request.dump();
            http_data.env = wxGetApp().app_config->get("region") == "China" ? PRODUCTIONENV : FOREIGNENV;
            http_data.target = PRINTERTYPE;
            http_data.taskPath = "/get/filament/config/all";

            bool succeeded = false;
            result_body = MakerHttpHandle::getInstance().httpPostTask(http_data, succeeded);
            if (!succeeded) {
                BOOST_LOG_TRIVIAL(error) << "QDSDevice: cloud filament catalog request failed";
                return;
            }
            try {
                const json response = json::parse(result_body);
                if (!response.contains("data") || !apply_filament_catalog(response["data"]))
                    BOOST_LOG_TRIVIAL(warning) << "QDSDevice: cloud filament catalog response omitted valid data";
            } catch (const std::exception &e) {
                BOOST_LOG_TRIVIAL(warning) << "QDS cloud filament catalog response was invalid: " << e.what();
            }
#endif
            return;
        }

        const std::string catalog_url = m_frp_url + "/api/qidiclient/config/offical_filament_list";
        Slic3r::Http::get(catalog_url)
            .timeout_max(5)
            .header("accept", "application/json")
            .header("Content-Type", "application/json")
            .on_complete([&result_body](std::string body, unsigned) { result_body = std::move(body); })
            .on_error([](std::string, std::string error, unsigned status) {
                BOOST_LOG_TRIVIAL(warning) << "QDS filament catalog request failed: HTTP "
                                           << status << ", " << error;
            })
            .perform_sync();

        try {
            const json response = json::parse(result_body);
            if (!response.contains("result") || !apply_filament_catalog(response["result"]))
                BOOST_LOG_TRIVIAL(warning) << "QDSDevice: local filament catalog response omitted a valid result";
        } catch (const std::exception &e) {
            BOOST_LOG_TRIVIAL(warning) << "QDS local filament catalog response was invalid: " << e.what();
        }
    });
}
bool QDSDevice::is_online(){
    return m_status!= "offline";
}

void QDSDevice::twoStageParseIntToString(const json& status, std::string& target, std::string first, std::string second)
{

	if (status.contains(first) && status[first].contains(second)) {
		if (target != std::to_string(status[first][second].get<int>())) {
			target = std::to_string(status[first][second].get<int>());
			is_update = true;
		}
	}
}

void QDSDevice::twoStageParseStringToString(const json& status, std::string& target, std::string first, std::string second)
{
	if (status.contains(first) && status[first].contains(second)) {
		if (target != std::to_string(status[first][second].get<int>())) {
			target = status[first][second].get<std::string>();
			is_update = true;
		}
	}
}

template<typename T>
void QDSDevice::twoStageParse(const json& status, T& target, std::string first, std::string second)
{
    bool temp_is_update = false;
    twoStageParse1(status, target, first, second, temp_is_update);
    if (temp_is_update) {
        is_update = temp_is_update;
    }
}

template<typename T>
bool Slic3r::GUI::QDSDevice::parseJsonForPath(const json& jsonData, T& target, std::string path)
{
    // cj_5: manually walk the path to avoid exception noise from value(json_pointer)
    // json_pointer in 3.10.4 is not iterable and find() only does top-level lookup
    const json* ref = &jsonData;
    size_t pos = 1; // skip leading '/'
    while (pos < path.size()) {
        size_t next = path.find('/', pos);
        std::string token = (next == std::string::npos)
            ? path.substr(pos) : path.substr(pos, next - pos);
        // unescape ~1→/ and ~0→~ per RFC 6901
        for (size_t esc; (esc = token.find("~1")) != std::string::npos; )
            token.replace(esc, 2, "/");
        for (size_t esc; (esc = token.find("~0")) != std::string::npos; )
            token.replace(esc, 2, "~");
        if (!ref->contains(token)) return false;
        ref = &(*ref)[token];
        pos = (next == std::string::npos) ? path.size() : next + 1;
    }

    //y83
    if (ref->is_null()) {
        target = T{};
        return false;
    }

    target = ref->get<T>();
    return true;
}

int QDSDevice::getJsonCurStageToInt(const json& jsonData, std::string jsonName)
{
    if (!jsonData.contains(jsonName) || !jsonData[jsonName].is_number_integer()) {
        return -1;
    }
    return jsonData[jsonName].get<int>();
}

bool extractNumberWithSscanf(const std::string& str, int& result) {
	// 使用 sscanf 直接匹配格式并提取数字
	return (sscanf(str.c_str(), "fila%d", &result) == 1);
}

std::vector<float> QDSDevice::getNozzleDiameter(){
    std::lock_guard<std::mutex> lock(m_config_mtx);
    return m_nozzle_diameter;
}

bool QDSDevice::setReportedNozzleDiameters(std::vector<float> diameters)
{
    const bool has_reported_nozzles = !diameters.empty() &&
        std::all_of(diameters.begin(), diameters.end(),
                    [](float diameter) { return std::isfinite(diameter) && diameter > 0.0f; });
    if (!has_reported_nozzles)
        diameters.clear();
    std::lock_guard<std::mutex> lock(m_config_mtx);
    m_has_reported_nozzle_diameter = true;
    m_reported_nozzle_metadata_valid = has_reported_nozzles;
    m_nozzle_diameter = has_reported_nozzles ? std::move(diameters) : std::vector<float>{0.4f};
    return has_reported_nozzles;
}

QDSBoxSync::PrinterMetadata QDSDevice::getPrinterMetadata()
{
    std::lock_guard<std::mutex> lock(m_config_mtx);
    QDSBoxSync::PrinterMetadata metadata;
    if (!m_type.empty())
        metadata.configured_model = m_type;
    if (m_has_reported_nozzle_diameter) {
        metadata.reported_nozzles.emplace();
        if (m_reported_nozzle_metadata_valid)
            metadata.reported_nozzles->assign(m_nozzle_diameter.begin(), m_nozzle_diameter.end());
    }
    return metadata;
}

//y79
void QDSDevice::updatePrinterStatusData(json& status){

    if (m_print_msg == "Printing")
        return;

    std::lock_guard<std::mutex> lock(m_config_mtx);
    maker_job_is_update = true;
    maker_job_state = status.contains("jobState") ? status["jobState"].get<std::string>() : maker_job_state;
    maker_job_progress = status.contains("progress") ? status["progress"].get<std::string>() : maker_job_progress;

    if(maker_job_state == "Generating_Gcode"){
        m_print_msg = maker_job_state + " layer : " + maker_job_progress;
    } else if(maker_job_state == "SLICING_FINISHED"){
        m_print_msg = maker_job_progress;
    } else {
        m_print_msg = maker_job_state + " : " + maker_job_progress + "%";
    }
    is_update = true;

    if(status.contains("failCause") && !status["failCause"].empty()){
        BOOST_LOG_TRIVIAL(trace) << "some error is " << status << std::endl;
        maker_job_is_update = false;
    }
}

std::string QDSDevice::getMakerJobState(){
    std::lock_guard<std::mutex> lock(m_config_mtx);
    return maker_job_state;
}

std::string QDSDevice::getMakerJobProgress(){
    std::lock_guard<std::mutex> lock(m_config_mtx);
    return maker_job_progress;
}

void QDSDevice::setMakerJobIsUpdate(bool value) {
    std::lock_guard<std::mutex> lock(m_config_mtx);
    maker_job_is_update = value;
}

void QDSDevice::updateAllErrorData(json& jsonData)
{
    {
        std::lock_guard<std::mutex> lock(m_errorData_mtx);
        m_errorData.clear();
    }
    std::string event_value = "";
    if(jsonData.contains("event"))
    if (jsonData.contains("results") && jsonData["results"].is_array()) {
        for (auto& obj : jsonData["results"]) {
            updateErrorDataSingle(obj, "");
        }
    }
}

void QDSDevice::updateErrorDataForNotiry(json& jsonData)
{
    BOOST_LOG_TRIVIAL(trace) << "notify result is :" << jsonData;
    if (jsonData.contains("data") && jsonData["data"].is_object()) {
        std::string event_value = "";
        if(jsonData["data"].contains("event")){
            event_value = jsonData["data"]["event"].get<std::string>();
        }
        if (jsonData["data"].contains("results") && jsonData["data"]["results"].is_object()) {
            updateErrorDataSingle(jsonData["data"]["results"], event_value);
        }
    } 
}

void QDSDevice::updateErrorDataSingle(json& jsonData, std::string event_value)
{
    QDSDeviceErrorData errorData;
    errorData.event_value = event_value;
	parseJsonForPath(jsonData, errorData.error_code, "/error_code");
	parseJsonForPath(jsonData, errorData.error_message, "/error_message");
	parseJsonForPath(jsonData, errorData.error_popup, "/error_popup");
	parseJsonForPath(jsonData, errorData.error_type, "/error_type");
	parseJsonForPath(jsonData, errorData.error_weight, "/error_weight");
	parseJsonForPath(jsonData, errorData.prossess_message, "/prossess_message");

    std::lock_guard<std::mutex> lock(m_errorData_mtx);

    if(errorData.error_type == 0)
        return;

    if (errorData.event_value.empty() || errorData.event_value == "add") {
        m_errorData.push_back(errorData);
        m_needUpdateErrorData = true;
    }
    else if(errorData.event_value == "update"){
        bool found = false;
        for(auto &err : m_errorData){
            if(err.error_code == errorData.error_code){
                err = errorData;
                found = true;
                break;
            }
        }
        if (!found)
            m_errorData.push_back(errorData);
        m_needUpdateErrorData = true;
    } else if(errorData.event_value == "remove"){
        for (auto it = m_errorData.begin(); it != m_errorData.end(); ) {
            if (it->error_code == errorData.error_code) {
                it = m_errorData.erase(it);
            } else {
                ++it;
            }
        }
    }
    std::sort(m_errorData.begin(), m_errorData.end(),
        [](const QDSDeviceErrorData& a, const QDSDeviceErrorData& b) {
            return a.error_type < b.error_type;
        });
}

//y79

QDSDeviceManager::QDSDeviceManager() {
    health_check_running_ = true;
    health_check_thread_ = std::thread(&QDSDeviceManager::healthCheckLoop, this);
}

QDSDeviceManager::~QDSDeviceManager() {

    health_check_running_ = false;
    if (health_check_thread_.joinable()) {
        health_check_thread_.join();
    }
    stopAllConnection();
}

void QDSDeviceManager::healthCheckLoop() {
    while (health_check_running_) {
        std::this_thread::sleep_for(health_check_interval_);

        if (!health_check_running_) break;

        performHealthCheck();
    }
}

void QDSDeviceManager::performHealthCheck() {
    std::vector<std::string> devices_to_reconnect;
    const auto reconnect_cooldown = std::chrono::seconds(20);

    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        auto now = std::chrono::steady_clock::now();

        for (const auto& [device_id, device] : devices_) {
            // 检查设备状态
            bool needs_reconnect = false;
            std::string reason;

            if (device->is_selected.load() && !device->is_net_device) {
                if (device->reconnecting.load()) {
                    continue;
                }
                if (device->last_reconnect != std::chrono::steady_clock::time_point::min() &&
                    (now - device->last_reconnect) < reconnect_cooldown) {
                    continue;
                }
                // 1. 检查设备状态
                if (device->m_status == "Unauthorized")
                    continue;
                if (device->m_status == "offline" || device->m_status == "error") {
                    needs_reconnect = true;
                    reason = "status is " + device->m_status;
                }
                // 2. 检查最后更新时间（超过30秒无更新认为连接异常）
                else if (std::chrono::duration_cast<std::chrono::seconds>(now - device->last_update).count() > 60) {
                    needs_reconnect = true;
                    reason = "no update for " +
                        std::to_string(std::chrono::duration_cast<std::chrono::seconds>(now - device->last_update).count()) + " seconds";
                    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "device last update : " << (device->last_update).time_since_epoch().count() << std::endl;
                    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "now time is  : " << now.time_since_epoch().count() << std::endl;
                }
            }

            if (needs_reconnect) {
                BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[HealthCheck] Device " << device_id << "device name is " << device->m_name << " needs reconnect: " << reason << std::endl;
                devices_to_reconnect.push_back(device_id);
            }
        }
    }

    // 重新连接需要重连的设备
    for (const auto& device_id : devices_to_reconnect) {
        reconnectDevice(device_id);
    }
}

void QDSDeviceManager::reconnectDevice(const std::string& device_id) {
    auto device = getDevice(device_id);
    if (!device) {
        return;
    }
    bool expected = false;
    if (!device->reconnecting.compare_exchange_strong(expected, true)) {
        return;
    }
    struct ReconnectGuard {
        std::shared_ptr<QDSDevice> dev;
        ~ReconnectGuard() { if (dev) dev->reconnecting = false; }
    } guard { device };

    device->last_reconnect = std::chrono::steady_clock::now();
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[HealthCheck] Reconnecting device " << device_id << "..." << std::endl;

    // 先断开连接
    stopConnection(device_id);

    // 等待一小段时间
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 重新连接
    connectDevice(device_id);
}

int QDSDeviceManager::generateDeviceID() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> distrib(1000, 9999);
    return distrib(gen);
}

std::shared_ptr<QDSDevice> QDSDeviceManager::getDevice(const std::string& device_id) {
    std::shared_ptr<QDSDevice> ret;

    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        auto it = devices_.find(device_id);
        if (it != devices_.end())
            ret = it->second;
    }
    return ret;
}

//y80
std::string QDSDeviceManager::getNetDeviceIDByIp(const std::string& ip){
    std::lock_guard<std::mutex> lock(manager_mutex_);
    for(const auto& [device_id, device] : devices_){
        if(device->m_ip == ip && device->is_net_device)
            return device_id;
    }
    return "";
}

//y80
std::string QDSDeviceManager::getLocalDeviceIDByIp(const std::string& ip){
    std::lock_guard<std::mutex> lock(manager_mutex_);
    for(const auto& [device_id, device] : devices_){
        if(device->m_ip == ip && !device->is_net_device)
            return device_id;
    }
    return "";
}

//cj_3
std::vector<std::pair<std::string, std::shared_ptr<QDSDevice>>> QDSDeviceManager::snapshotDevices()
{
    std::lock_guard<std::mutex> lock(manager_mutex_);
    std::vector<std::pair<std::string, std::shared_ptr<QDSDevice>>> out;
    out.reserve(devices_.size());
    for (const auto& kv : devices_) {
        out.emplace_back(kv.first, kv.second);
    }
    return out;
}

//cj_5
void QDSDeviceManager::refreshLocalDevices(bool force, LocalDeviceDiscovery::RefreshCallback callback)
{
    m_local_discovery.refresh(force, std::move(callback));
    //cj_5 Trigger SSDP discovery alongside UDP so both run together.
    m_ssdp_discovery.refresh(force, nullptr);
}

//cj_5
bool QDSDeviceManager::findLocalDeviceBySerial(const std::string& serial, LocalDiscoveredDevice& out) const
{
    return m_local_discovery.findBySerial(serial, out);
}

//cj_5
LocalDeviceDiscovery::Snapshot QDSDeviceManager::snapshotLocalDevices() const
{
    return m_local_discovery.snapshot();
}

//cj_5 SSDP discovery API
void QDSDeviceManager::refreshSSDPDevices(bool force, SSDPDiscovery::RefreshCallback callback)
{
    m_ssdp_discovery.refresh(force, std::move(callback));
}

bool QDSDeviceManager::findSSDPDeviceBySerial(const std::string& serial, LocalDiscoveredDevice& out) const
{
    return m_ssdp_discovery.findBySerial(serial, out);
}

bool QDSDeviceManager::findSSDPDeviceByIP(const std::string& ip, LocalDiscoveredDevice& out) const
{
    return m_ssdp_discovery.findByIP(ip, out);
}

SSDPDiscovery::Snapshot QDSDeviceManager::snapshotSSDPDevices() const
{
    return m_ssdp_discovery.snapshot();
}

//cj_5
#if QDT_RELEASE_TO_PUBLIC
bool QDSDeviceManager::findLocalForNetDevice(const NetDevice& net_dev, LocalDiscoveredDevice& out) const
{
    // Primary match: serial number (cloud serialNumber == UDP field 7 serial)
    if (findLocalDeviceBySerial(net_dev.mac_address, out)) {
        BOOST_LOG_TRIVIAL(trace) << __FUNCTION__
            << " Found local match by serial: " << net_dev.mac_address
            << " at IP " << out.ip << std::endl;
        return true;
    }

    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__
        << " No local match for net device: " << net_dev.mac_address << std::endl;
    return false;
}
#endif

std::shared_ptr<QDSDevice> QDSDeviceManager::getSelectedDevice(){
    std::lock_guard<std::mutex> lock(manager_mutex_);
    for(const auto& [device_id, device] : devices_){
        if(device->is_selected)
            return device;
    }
    return nullptr;
}

void QDSDeviceManager::stopConnection(const std::string& device_id) {
    std::shared_ptr<WebSocketConnect> conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        auto conn_it = connections_.find(device_id);
        if (conn_it == connections_.end()) return;
        conn = conn_it->second;
        conn->stopping = true;
    }

    int wait_count = 0;
    while (conn && conn->processing_message && wait_count < 10) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        wait_count++;
    }

    safeStopConnection(device_id);

    cleanupConnection(device_id);
}

void QDSDeviceManager::safeStopConnection(const std::string& device_id) {
    std::shared_ptr<WebSocketConnect> conn = nullptr;
    
    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        auto conn_it = connections_.find(device_id);
        if (conn_it == connections_.end()) return;
        conn = conn_it->second;
    }
    
    if (!conn || !conn->running) return;
    
    try {
        websocketpp::lib::error_code ec;
        
        // 尝试正常关闭连接
        auto con = conn->client.get_con_from_hdl(conn->connection_hdl);
        if (con && con->get_state() == websocketpp::session::state::open) {
            conn->client.close(conn->connection_hdl, 
                               websocketpp::close::status::going_away, 
                               "Connection stopped by manager", ec);
            if (ec) {
                BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Stop] Warning closing connection for device " 
                          << device_id << ": " << ec.message() << std::endl;
            }
        }
        
        // 停止客户端
        conn->client.stop();
        conn->running = false;
        
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Stop] Exception while stopping client for device " 
                  << device_id << ": " << e.what() << std::endl;
        if (conn) {
            conn->running = false;
        }
    }
}

void QDSDeviceManager::cleanupConnection(const std::string& device_id) {
    std::shared_ptr<WebSocketConnect> conn = nullptr;

    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        auto conn_it = connections_.find(device_id);
        if (conn_it == connections_.end()) return;
        conn = conn_it->second;
        // 立即从 map 中移除，防止其他线程使用
        connections_.erase(conn_it);
    }

    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Manager] Starting cleanup for device " << device_id << std::endl;

    if (conn) {
        // 使用局部变量引用线程，避免通过 shared_ptr 多次访问
        std::thread& client_thread = conn->client_thread;

        if (client_thread.joinable()) {
            try {
                // 尝试 join，设置超时避免无限等待
                if (client_thread.joinable()) {
                    // 可以添加超时机制
                    client_thread.join();
                }
            }
            catch (const std::system_error& e) {
                BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Cleanup] System error joining thread for device "
                    << device_id << ": " << e.what()
                    << " (code: " << e.code() << ")" << std::endl;

                // 根据错误代码处理
                if (e.code() == std::errc::no_such_process ||
                    e.code() == std::errc::invalid_argument) {
                    // 线程已经结束或无效，尝试 detach
                    try {
                        if (client_thread.joinable()) {
                            client_thread.detach();
                        }
                    }
                    catch (...) {
                        // 如果 detach 也失败，记录日志
                        BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Cleanup] Failed to detach thread for device "
                            << device_id << std::endl;
                    }
                }
            }
            catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Cleanup] Error joining thread for device "
                    << device_id << ": " << e.what() << std::endl;
            }
        }
    }

    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Manager] Device " << device_id << " connection cleaned up." << std::endl;
}

std::string QDSDeviceManager::addDevice(const std::string& dev_name, const std::string& dev_ip, const std::string& dev_url, const std::string& dev_type) {
    std::string device_id;
    bool id_is_unique = false;

    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        while (!id_is_unique) {
            device_id = std::to_string(generateDeviceID());
            if (devices_.find(device_id) == devices_.end()) {
                id_is_unique = true;
            }
            else {
                std::cerr << "[Manager] Warning: Device ID " << device_id << " conflict, retrying." << std::endl;
            }
        }

        auto device = std::make_shared<QDSDevice>(device_id, dev_name, dev_ip, dev_url, dev_type);
        //y79
        device->m_frp_url = "http://" + dev_url ;

        devices_[device_id] = device;
        BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Manager] Device added: " << device_id << std::endl;
    }

    std::thread([this, device_id]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        connectDevice(device_id);
    }).detach();

    return device_id;
}

bool QDSDeviceManager::addDevice(std::shared_ptr<QDSDevice> device)
{
	std::string device_id = device->m_id;
	std::lock_guard<std::mutex> lock(manager_mutex_);
	if (devices_.find(device_id) != devices_.end()) {
		BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "device :" << device << "exit" << std::endl;
		return false;
	}
    
	devices_[device_id] = device;
	return true;
}

bool QDSDeviceManager::removeDevice(const std::string& device_id) {
    bool removed = false;
    disconnectDevice(device_id);
    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        removed = devices_.erase(device_id) > 0;
    }
    if (removed) {
        //cj_5 Clear Plater sync status badges after device removal
        GUI::wxGetApp().plater()->update_machine_sync_status();
        auto callback = getDeleteDeviceIDCallback();
        if (callback) {
            callback(device_id);
        }
    }
    return removed;
}

bool QDSDeviceManager::connectDevice(const std::string device_id) {
    std::shared_ptr<QDSDevice> dev = getDevice(device_id);
    if (!dev) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "[Connect] Error: Device " << device_id << " not found." << std::endl;
        return false;
    }

    disconnectDevice(device_id);

    std::shared_ptr<WebSocketConnect> connection = nullptr;
    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        connection = std::make_shared<WebSocketConnect>();
        connection->info = dev;
        connections_[device_id] = connection;
    }

    //初始化websocket客户端
    connection->client.init_asio();
    connection->client.clear_access_channels(websocketpp::log::alevel::all);
    connection->client.clear_error_channels(websocketpp::log::elevel::all);

    // 绑定事件处理器（使用 lambda 捕获 device_id）
    std::weak_ptr<WebSocketConnect> weak_conn = connection;

    connection->client.set_open_handler([this, device_id, weak_conn](auto hdl) {
        auto conn = weak_conn.lock();
        if (conn) {
            conn->last_activity = std::chrono::steady_clock::now();
        }
        onOpen(device_id, hdl);
    });

    connection->client.set_message_handler([this, device_id, weak_conn](auto hdl, auto msg) {
        auto conn = weak_conn.lock();
        if (conn && !conn->stopping) {
            conn->processing_message = true;
            conn->message_processing_count++;
            conn->last_activity = std::chrono::steady_clock::now();
            
            try {
                onMessage(device_id, hdl, msg);
            } catch (...) {
                // 捕获所有异常，确保processing_message被重置
            }
            
            conn->message_processing_count--;
            if (conn->message_processing_count == 0) {
                conn->processing_message = false;
            }
        }
    });

    connection->client.set_close_handler([this, device_id, weak_conn](auto hdl) {
        auto conn = weak_conn.lock();
        if (conn) {
            conn->last_activity = std::chrono::steady_clock::now();
            BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "[DEBUG] WebSocket closed for device: " << device_id << std::endl;
            auto ws_conn = conn->client.get_con_from_hdl(hdl);
            if (ws_conn) {
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "[DEBUG] Close code: " << ws_conn->get_remote_close_code() << std::endl;
                BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "[DEBUG] Close reason: " << ws_conn->get_remote_close_reason() << std::endl;
            }
        }

        onClose(device_id, hdl);
    });

    connection->client.set_fail_handler([this, device_id, weak_conn](auto hdl) {
        auto conn = weak_conn.lock();
        if (conn) {
            conn->last_activity = std::chrono::steady_clock::now();
        }
        onFail(device_id, hdl);
    });

    try {
        websocketpp::lib::error_code ec;
        auto con = connection->client.get_connection(dev->m_url, ec);
        if (ec) {
            BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "[Connect] Connection error for device " << device_id 
                      << ": " << ec.message() << std::endl;
            updateDeviceStatus(device_id, "offline");
            
            std::lock_guard<std::mutex> lock(manager_mutex_);
            connections_.erase(device_id);
            return false;
        }
        
        connection->connection_hdl = con->get_handle();
        connection->running = true;
        
        connection->client.connect(con);
        
        connection->client_thread = std::thread([connection]() {
            try {
                connection->client.run();
            } catch (const websocketpp::exception& e) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "[WebSocket] Exception in client thread: " 
                          << e.what() << std::endl;
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "[WebSocket] Exception in client thread: "
                          << e.what() << std::endl;
            }
            connection->running = false;
        });
        
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << "[Connect] Connecting to device " << device_id 
                  << " (" << dev->m_name << ")..." << std::endl;
        return true;
        
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "[Connect] Exception while connecting to device " << device_id
                  << ": " << e.what() << std::endl;
        updateDeviceStatus(device_id, "error");
        
        std::lock_guard<std::mutex> lock(manager_mutex_);
        connections_.erase(device_id);
        return false;
    }
}

bool QDSDeviceManager::disconnectDevice(const std::string& device_id) {
    stopConnection(device_id);
    return true;
}

void QDSDeviceManager::onOpen(const std::string& device_id, websocketpp::connection_hdl hdl) {
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[WS] Device " << device_id << " connected." << std::endl;
    processConnectionStatus(device_id, "connected");
    
    // 延迟发送订阅消息
    std::thread([this, device_id]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        sendSubscribeMessage(device_id);
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
        getAllErrorList(device_id);


    }).detach();
}
void QDSDeviceManager::onMessage(const std::string& device_id, websocketpp::connection_hdl hdl, WebSocketClient::message_ptr msg) {
    std::string msg_str;
    try {
        msg_str = msg->get_payload();
        
        // 检查连接是否正在停止
        std::shared_ptr<WebSocketConnect> conn;
        {
            std::lock_guard<std::mutex> lock(manager_mutex_);
            auto conn_it = connections_.find(device_id);
            if (conn_it == connections_.end() || conn_it->second->stopping) {
                return;
            }
        }
        
        // 解析JSON
        json message_json;
        try {
            message_json = json::parse(msg_str);
        } catch (const json::parse_error& e) {
            BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Message] JSON parse error for device " << device_id 
                      << ": " << e.what() << std::endl;
            return;
        }
        //cj_3
        // Any valid websocket payload means the connection is alive.
        if (auto dev = getDevice(device_id)) {
            dev->last_update = std::chrono::steady_clock::now();
        }
        
        // 处理消息
        handleDeviceMessage(device_id, message_json);
        
    } catch (const std::exception& e) {
        // 使用已保存的消息字符串
//         BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Message] Error in onMessage for device " << device_id 
//                   << ": " << e.what() 
//                   << ", message length: " << msg_str.length() << std::endl;
    }
}

void QDSDeviceManager::onClose(const std::string& device_id, websocketpp::connection_hdl hdl) {
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[WS] Device " << device_id << " disconnected." << std::endl;
    processConnectionStatus(device_id, "offline");
}

void QDSDeviceManager::onFail(const std::string& device_id, websocketpp::connection_hdl hdl) {
    BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[WS] Device " << device_id << " connection failed." << std::endl;
    processConnectionStatus(device_id, "offline");
}

void QDSDeviceManager::sendSubscribeMessage(const std::string& device_id) {
    std::shared_ptr<WebSocketConnect> conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        auto conn_it = connections_.find(device_id);
        if (conn_it == connections_.end() || conn_it->second->stopping) {
            return;
        }
        conn = conn_it->second;
    }

    if (!conn) return;

    json subscribe_msg = {
        {"id", std::atoi(device_id.c_str())},
        {"method", "printer.objects.subscribe"},
        {"jsonrpc", "2.0"},
        {"params", {
            {"objects", {
            // {"gcode", nullptr},
            // {"configfile", nullptr},
            // {"mcu", nullptr},
            // {"mcu mcu_box1", nullptr},
            // {"mcu THR", nullptr},
            // {"gcode_macro _KAMP_Settings", nullptr},
            // {"gcode_macro BED_MESH_CALIBRATE", nullptr},
            // {"gcode_macro PRINTER_PARAM", nullptr},
            // {"gcode_macro _CG28", nullptr},
            // {"gcode_macro save_zoffset", nullptr},
            // {"gcode_macro set_zoffset", nullptr},
            // {"gcode_macro CLEAR_NOZZLE_PLR", nullptr},
            // {"gcode_macro CLEAR_NOZZLE", nullptr},
            // {"gcode_macro SHAKE_OOZE", nullptr},
            // {"gcode_macro MOVE_TO_TRASH", nullptr},
            // {"gcode_macro EXTRUSION_AND_FLUSH", nullptr},
            // {"gcode_macro PRINT_START", nullptr},
            // {"gcode_macro ENABLE_ALL_SENSOR", nullptr},
            // {"gcode_macro DISABLE_ALL_SENSOR", nullptr},
            // {"gcode_macro AUTOTUNE_SHAPERS", nullptr},
            // {"gcode_macro M84", nullptr},
            // {"gcode_macro DETECT_INTERRUPTION", nullptr},
            // {"gcode_macro _HOME_X", nullptr},
            // {"gcode_macro _HOME_Y", nullptr},
            // {"gcode_macro _HOME_XY", nullptr},
            // {"gcode_macro SHAPER_CALIBRATE", nullptr},
            // {"gcode_macro PRINT_END", nullptr},
            // {"gcode_macro CANCEL_PRINT", nullptr},
            // {"gcode_macro PAUSE", nullptr},
            // {"gcode_macro RESUME_PRINT", nullptr},
            // {"gcode_macro RESUME", nullptr},
            // {"gcode_macro RESUME_1", nullptr},
            // {"gcode_macro M141", nullptr},
            // {"gcode_macro M191", nullptr},
            // {"gcode_macro M106", nullptr},
            // {"gcode_macro M107", nullptr},
            // {"gcode_macro M303", nullptr},
            // {"gcode_macro M900", nullptr},
            // {"gcode_macro M290", nullptr},
            // {"gcode_macro M901", nullptr},
            // {"gcode_macro M0", nullptr},
            // {"gcode_macro M25", nullptr},
            // {"gcode_macro M4029", nullptr},
            // {"gcode_macro move_screw1", nullptr},
            // {"gcode_macro move_screw2", nullptr},
            // {"gcode_macro move_screw3", nullptr},
            // {"gcode_macro move_screw4", nullptr},
            // {"gcode_macro M4030", nullptr},
            // {"gcode_macro M4031", nullptr},
            // {"gcode_macro CUT_FILAMENT_1", nullptr},
            // {"gcode_macro M603", nullptr},
            // {"gcode_macro M604", nullptr},
            {"gcode_move", nullptr},
            // {"gcode_macro M109", nullptr},
             {"exclude_object", nullptr},
            // {"gcode_macro G31", nullptr},
            // {"gcode_macro G32", nullptr},
            // {"gcode_macro G29", nullptr},
            // {"gcode_macro M204", nullptr},
            // {"gcode_macro BEEP", nullptr},
            // {"gcode_macro beep_on", nullptr},
            // {"gcode_macro beep_off", nullptr},
            // {"gcode_macro LED_ON", nullptr},
            // {"gcode_macro LED_OFF", nullptr},
            // {"gcode_macro GET_TIMELAPSE_SETUP", nullptr},
            // {"gcode_macro _SET_TIMELAPSE_SETUP", nullptr},
            // {"gcode_macro TIMELAPSE_TAKE_FRAME", nullptr},
            // {"gcode_macro _TIMELAPSE_NEW_FRAME", nullptr},
            // {"gcode_macro HYPERLAPSE", nullptr},
            // {"gcode_macro TIMELAPSE_RENDER", nullptr},
            // {"gcode_macro TEST_STREAM_DELAY", nullptr},
            // {"gcode_macro save_last_file", nullptr},
            // {"gcode_macro CLEAR_LAST_FILE", nullptr},
            // {"gcode_macro LOG_Z", nullptr},
            // {"gcode_macro RESUME_INTERRUPTED", nullptr},
            // {"stepper_enable", nullptr},
            // {"motion_report", nullptr},
            // {"query_endstops", nullptr},
            // // 盒子信息
            // {"box_extras", nullptr},
            // {"box_stepper slot0", nullptr},
            // {"box_stepper slot1", nullptr},
            // {"box_stepper slot2", nullptr},
            // {"box_stepper slot3", nullptr},
            // {"aht20_f heater_box1", nullptr},
            // {"heater_generic heater_box1", nullptr},
            // {"temperature_sensor heater_temp_a_box1", nullptr},
            // {"temperature_sensor heater_temp_b_box1", nullptr},
            // {"box_heater_fan heater_fan_a_box1", nullptr},
            // {"box_heater_fan heater_fan_b_box1", nullptr},
            // {"controller_fan board_fan_box1", nullptr},
            // {"heaters", nullptr},
            // {"heater_air", nullptr},
            // {"gcode_macro T0", nullptr},
            // {"gcode_macro T1", nullptr},
            // {"gcode_macro T2", nullptr},
            // {"gcode_macro T3", nullptr},
            // {"gcode_macro UNLOAD_T0", nullptr},
            // {"gcode_macro UNLOAD_T1", nullptr},
            // {"gcode_macro UNLOAD_T2", nullptr},
            // {"gcode_macro UNLOAD_T3", nullptr},
            // {"gcode_macro T4", nullptr},
            // {"gcode_macro T5", nullptr},
            // {"gcode_macro T6", nullptr},
            // {"gcode_macro T7", nullptr},
            // {"gcode_macro UNLOAD_T4", nullptr},
            // {"gcode_macro UNLOAD_T5", nullptr},
            // {"gcode_macro UNLOAD_T6", nullptr},
            // {"gcode_macro UNLOAD_T7", nullptr},
            // {"gcode_macro T8", nullptr},
            // {"gcode_macro T9", nullptr},
            // {"gcode_macro T10", nullptr},
            // {"gcode_macro T11", nullptr},
            // {"gcode_macro UNLOAD_T8", nullptr},
            // {"gcode_macro UNLOAD_T9", nullptr},
            // {"gcode_macro UNLOAD_T10", nullptr},
            // {"gcode_macro UNLOAD_T11", nullptr},
            // {"gcode_macro T12", nullptr},
            // {"gcode_macro T13", nullptr},
            // {"gcode_macro T14", nullptr},
            // {"gcode_macro T15", nullptr},
            // {"gcode_macro UNLOAD_T12", nullptr},
            // {"gcode_macro UNLOAD_T13", nullptr},
            // {"gcode_macro UNLOAD_T14", nullptr},
            // {"gcode_macro UNLOAD_T15", nullptr},
            // {"gcode_macro UNLOAD_FILAMENT", nullptr},
            // {"pause_resume", nullptr},
             {"filament_switch_sensor filament_switch_sensor", nullptr},
            // {"bed_screws", nullptr},
            // {"tmc2209 extruder", nullptr},
            // {"z_tilt", nullptr},
            // {"tmc2240 stepper_x", nullptr},
            // {"tmc2240 stepper_y", nullptr},
            // {"tmc2209 stepper_z1", nullptr},
            // {"tmc2209 stepper_z", nullptr},
            // {"temperature_sensor Chamber_Thermal_Protection_Sensor", nullptr},
             {"fan_generic chamber_circulation_fan", nullptr},
            // {"controller_fan chamber_fan", nullptr},
            // {"heater_fan hotend_fan", nullptr},
             {"fan_generic cooling_fan", nullptr},
            // {"controller_fan board_fan", nullptr},
             {"fan_generic auxiliary_cooling_fan", nullptr},
             //cj_3
             {"output_pin polar_cooler", nullptr},
            // {"output_pin beeper", nullptr},
            // {"probe", nullptr},
            // {"probe_air", nullptr},
            // {"bed_mesh", nullptr},
            // {"idle_timeout", nullptr},
            // {"system_stats", nullptr},
            // {"manual_probe", nullptr},
            { "print_stats_manager",nullptr},
            {"print_stats", nullptr},
            {"display_status", nullptr},
            // {"webhooks", nullptr},
            // {"virtual_sdcard", nullptr},
            {"toolhead", nullptr},
            {"heater_bed", nullptr},
            {"extruder", nullptr},
            {"heater_generic chamber", nullptr},
            {"output_pin caselight", nullptr},
            {"save_variables", nullptr},
			{ "aht20_f heater_box1",nullptr },
			{ "aht20_f heater_box2",nullptr },
			{ "aht20_f heater_box3",nullptr },
			{ "aht20_f heater_box4",nullptr },
            {"box_stepper slot0", nullptr},
            {"box_stepper slot1", nullptr},
            {"box_stepper slot2", nullptr},
            {"box_stepper slot3", nullptr},
            {"box_stepper slot4", nullptr},
            {"box_stepper slot5", nullptr},
            {"box_stepper slot6", nullptr},
            {"box_stepper slot7", nullptr},
            {"box_stepper slot8", nullptr},
            {"box_stepper slot9", nullptr},
            {"box_stepper slot10", nullptr},
            {"box_stepper slot11", nullptr},
            {"box_stepper slot12", nullptr},
            {"box_stepper slot13", nullptr},
            {"box_stepper slot14", nullptr},
            {"box_stepper slot15", nullptr},
            {"box_stepper slot16", nullptr},
             {"data",nullptr}
        }}
    }}
    };


    
    try {
        websocketpp::lib::error_code ec;
        conn->client.send(conn->connection_hdl, 
                          subscribe_msg.dump(), 
                          websocketpp::frame::opcode::text, 
                          ec);
        if (ec) {
            BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Send] Error sending subscribe to " << device_id 
                      << ": " << ec.message() << std::endl;
        } else {
            BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Send] Sent subscribe message to " << device_id << std::endl;
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Send] Exception sending to device " << device_id 
                  << ": " << e.what() << std::endl;
    }
}

void QDSDeviceManager::getAllErrorList(const std::string& device_id)
{
    sendCommand(device_id, "method", "get_all_error_list", "server.extensions.request");
}

void QDSDeviceManager::sendCommand(const std::string& device_id, const std::string& script){
    std::shared_ptr<WebSocketConnect> conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        auto conn_it = connections_.find(device_id);
        if (conn_it == connections_.end() || conn_it->second->stopping) {
            return;
        }
        conn = conn_it->second;
    }

    if (!conn) return;

    json subscribe_msg = {
        {"id", std::atoi(device_id.c_str())},
        {"method", "printer.gcode.script"},
        {"jsonrpc", "2.0"},
        {"params", {
            {"script", script}
        }}
    };

    try {
        websocketpp::lib::error_code ec;
        conn->client.send(conn->connection_hdl, 
                          subscribe_msg.dump(), 
                          websocketpp::frame::opcode::text, 
                          ec);
        if (ec) {
            BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Send] Error sending subscribe to " << device_id 
                      << ": " << ec.message() << std::endl;
        } else {
            BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Send] Sent subscribe message to " << device_id << std::endl;
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Send] Exception sending to device " << device_id 
                  << ": " << e.what() << std::endl;
    }
}

bool QDSDeviceManager::sendCommand(const std::string& device_id, const std::string& scriptName, const std::string& script, const std::string& method)
{
	std::shared_ptr<WebSocketConnect> conn = nullptr;
	{
		std::lock_guard<std::mutex> lock(manager_mutex_);
		auto conn_it = connections_.find(device_id);
		if (conn_it == connections_.end() || conn_it->second->stopping) {
			return false;
		}
		conn = conn_it->second;
	}

	if (!conn) return false;

	json subscribe_msg = {
		{"id", std::atoi(device_id.c_str())},
		{"method", method},
		{"jsonrpc", "2.0"},
		{"params", {
			{scriptName, script}
		}}
	};

	try {
		websocketpp::lib::error_code ec;
		conn->client.send(conn->connection_hdl,
			subscribe_msg.dump(),
			websocketpp::frame::opcode::text,
			ec);
		if (ec) {
			BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Send] Error sending subscribe to " << device_id
				<< ": " << ec.message() << std::endl;
			return false;
		}
		BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Send] Sent subscribe message to " << device_id << std::endl;
		return true;
	}
	catch (const std::exception& e) {
		BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Send] Exception sending to device " << device_id
			<< ": " << e.what() << std::endl;
		return false;
	}
}

void QDSDeviceManager::sendActionCommand(const std::string& device_id, const std::string& action_type){
    std::shared_ptr<WebSocketConnect> conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        auto conn_it = connections_.find(device_id);
        if (conn_it == connections_.end() || conn_it->second->stopping) {
            return;
        }
        conn = conn_it->second;
    }

    if (!conn) return;

    std::string script = "printer.print.";
    json subscribe_msg = {
        {"id", std::atoi(device_id.c_str())},
        {"method", script + action_type},
        {"jsonrpc", "2.0"}
    };

    try {
        websocketpp::lib::error_code ec;
        conn->client.send(conn->connection_hdl, 
                          subscribe_msg.dump(), 
                          websocketpp::frame::opcode::text, 
                          ec);
        if (ec) {
            BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Send] Error sending subscribe to " << device_id 
                      << ": " << ec.message() << std::endl;
        } else {
            BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Send] Sent subscribe message to " << device_id << std::endl;
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << "[Send] Exception sending to device " << device_id 
                  << ": " << e.what() << std::endl;
    }
}

void QDSDeviceManager::handleDeviceMessage(const std::string& device_id, const json& message) {
    std::shared_ptr<QDSDevice> device = getDevice(device_id);
    if (!device) {
        return;
    }

    // if(!device->is_selected.load()){
    //     std::thread([this, device_id]() {
    //         std::this_thread::sleep_for(std::chrono::milliseconds(10));
    //         stopConnection(device_id);
    //     }).detach();
    // }

    updateDeviceMsg(device_id, message);
}

void QDSDeviceManager::updateDeviceMsg(const std::string& device_id, const json& message) {
    std::string new_status;
    bool is_update = false;
    bool is_file_info_update = false;
    std::shared_ptr<QDSDevice> device = nullptr;
    
    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        auto dev_it = devices_.find(device_id);
        if (dev_it == devices_.end()) {
            return;
        }
        device = dev_it->second;
        
        // if(!device->is_selected.load())
        //     return;
        //BOOST_LOG_TRIVIAL(trace) << device << " : " << message;
		if (message.contains("method") && message.contains("params") 
            ) {
            if (message.at("method").get<std::string>() == "notify_proc_stat_update" && message.at("params").is_array()) {
                
                const json& result = message.at("params").at(0);

                if(result.contains("config_items")){
                    device->m_enable_polar_cooler = result["config_items"]["printing.polar_cooler"].get<std::string>() == "1" ? true : false;
                    
                    //y78
                    if(result["config_items"].contains("nozzle.diameter")){
                        std::vector<float> reported_nozzles;
                        try {
                            const auto &nozzle_value = result["config_items"]["nozzle.diameter"];
                            auto append_nozzle = [&reported_nozzles](const json &item) {
                                if (item.is_number())
                                    reported_nozzles.push_back(item.get<float>());
                                else if (item.is_string()) {
                                    const std::string value = item.get<std::string>();
                                    size_t parsed = 0;
                                    const float nozzle = std::stof(value, &parsed);
                                    if (parsed != value.size())
                                        throw std::invalid_argument("nozzle diameter contains trailing characters");
                                    reported_nozzles.push_back(nozzle);
                                } else
                                    throw std::invalid_argument("unsupported nozzle diameter value");
                            };
                            if (nozzle_value.is_array()) {
                                for (const auto &item : nozzle_value)
                                    append_nozzle(item);
                            } else {
                                append_nozzle(nozzle_value);
                            }
                        } catch (const std::exception &e) {
                            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": invalid reported nozzle metadata: " << e.what();
                            reported_nozzles.clear();
                        }
                        if (!device->setReportedNozzleDiameters(std::move(reported_nozzles)))
                            BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": reported nozzle metadata was empty";
                    }
                }

            }


            if (message.at("method").is_string()&&message.at("method").get<std::string>() == "notify_agent_event" && message.at("params").is_array()) {
                json result = message.at("params").at(0);
                device->updateErrorDataForNotiry(result);
            }

        }
        
        // 处理状态更新
        if (message.contains("result")) {

            if (message.at("result").contains("files")) {
                const json& result = message.at("result");
                updateDeviceFileInfo(device, result);
                is_file_info_update = true;
            }
            if (message.at("result").contains("status")) {
                const json& result = message.at("result").at("status");

                updateDeviceData(device, result, new_status, is_update);
            }

            if (message.at("result").contains("event") && message["result"]["event"].is_string()
                    && message["result"]["event"].get<std::string>() == "GetAll") {
                json jsonResult = message["result"];

                device->updateAllErrorData(jsonResult);
            }
        }

        
        // 打印记录修改
        if(message.contains("method")){
            if(message.at("method").get<std::string>() == "notify_history_changed"){
                updatePrintThumbUrl(device, message);
            }
        }

        // if(message.contains("method")){
        //     if(message.at("method").get<std::string>() == "notify_filelist_changed"){

        //     }
        // }
        
        // 处理通知更新
        if (message.contains("method") && 
            message.at("method").get<std::string>() == "notify_status_update" && 
            message.at("params").is_array() && 
            !message.at("params").empty()) {
            
            const json& result = message.at("params").at(0);
            
            updateDeviceData(device, result, new_status, is_update);

// 			if (result.contains("files")) {
// 				updateDeviceFileInfo(device, result);
// 			}
        }
        
        // 处理错误
        if (message.contains("error") && 
            message["error"].contains("message") &&
            message["error"]["message"] == "Unauthorized") {
            device->m_status = "Unauthorized";
            new_status = "Unauthorized";
        }
        
        if (is_update) {
            device->last_update = std::chrono::steady_clock::now();
        }
    }
    
    // 触发回调
    if (!new_status.empty()) {
        updateDeviceStatus(device_id, new_status);
    }
    
    if (is_update) {
        auto callback = getParameterUpdateCallback();
        if (callback) {
            callback(device_id);
        }
    }

    if(is_file_info_update){
        auto callback = getFileInfoUpdateCallback();
        if(callback)
            callback(device_id);
    }
}

void QDSDeviceManager::updateDeviceData(std::shared_ptr<QDSDevice>& device,
                                        const json& result,
                                        std::string& new_status,
                                        bool& is_update) {

    //cj_4
    // Capture pre-call state for diff detection.
    std::string old_status = device->m_status;
    std::string old_print_duration = device->m_print_duration;

    // Delegate all Klipper status field parsing to device.
    device->updateByJsonData(result);

    // --- Post-processing that belongs to manager, not device ---

    // Status transition → report via connection callback.
    if (device->m_status != old_status) {
        new_status = device->m_status;
    }

    // y83
    if (device->is_update.exchange(false)) {
        is_update = true;
    }

    // When print_duration changes, refresh thumb URL if not already loaded.e
    if (device->m_print_duration != old_print_duration
        /*&& device->m_print_png_url.empty()*/) {
        updatePrintThumbUrlWithOutMsg(device);
    }
}

//cj_2
std::string extractAfterGcodes(const std::string& fullPath) {
	std::string keyword = "/gcodes/";

	size_t pos = fullPath.find(keyword);
	if (pos != std::string::npos) {
		// 从 keyword 后面开始截取
		return fullPath.substr(pos + keyword.length());
	}

	return "";  // 没找到返回空字符串
}

void QDSDeviceManager::updateDeviceFileInfo(std::shared_ptr<QDSDevice>& device, const json& result, bool support_p2p){
    device->file_info.clear();
    const auto& result_array = support_p2p ? result : result["result"];
    //y78
    for(const auto& file_item : result_array){
        GCodeFileInfo file_info;
        //cj_2 filter cache file
        file_info.file_path = file_item["filepath"].get<std::string>();
        if (file_info.file_path.find("/.cache/")!= std::string::npos) {
            continue;
        }
        file_info.extension = file_item["extension"].get<std::string>();
		//file_info.file_name = file_item["filename"].get<std::string>();
		file_info.file_name = extractAfterGcodes(file_item["filepath"].get<std::string>());
        file_info.plate_count = file_item["plate_count"].get<std::string>();
        file_info.show_filament_weight = file_item["show_filament_weight"].get<std::string>();
        file_info.show_print_time = file_item["show_print_time"].get<std::string>();
        
        int plate_count = std::stoi(file_info.plate_count);
        if(plate_count > 0){
            auto plates_array = file_item["plates"];
            for(const auto& plate_item : plates_array){
                
                PlateInfo plate_info;
                plate_info.index = plate_item["plate_index"].get<std::string>();
                boost::split(plate_info.filament_colours, plate_item["filament_colour"].get<std::string>(), boost::is_any_of(";"));
                boost::split(plate_info.filament_types, plate_item["filament_type"].get<std::string>(), boost::is_any_of(";"));
                boost::split(plate_info.used_extruders, plate_item["used_extruders"].get<std::string>(), boost::is_any_of(";"));
                plate_info.filament_weight = plate_item["filament_weight"].get<std::string>();
                plate_info.print_time = plate_item["print_time"].get<std::string>();
                plate_info.nozzle_diameter = plate_item["nozzle_diameter"].get<std::string>();

                // Only strip .3mf extension; keep other extensions intact
                std::string name_without_extension = file_info.file_name;
                const std::string ext_3mf = ".3mf";
                if (name_without_extension.size() > ext_3mf.size()) {
                    std::string suffix = name_without_extension.substr(name_without_extension.size() - ext_3mf.size());
                    std::transform(suffix.begin(), suffix.end(), suffix.begin(), ::tolower);
                    if (suffix == ext_3mf) {
                        name_without_extension = name_without_extension.substr(0, name_without_extension.size() - ext_3mf.size());
                    }
                }

				plate_info.thumb_url = device->m_frp_url + "/server/files/gcodes/.thumbs/" + name_without_extension + "/plate_" + plate_info.index + ".png";

                wxBitmap bitmap = ScalableBitmap(nullptr, "monitor_placeholder", 160).bmp();
                wxImage image = bitmap.ConvertToImage();
                if (image.IsOk()) {
                    wxMemoryOutputStream mos;
                    if (image.SaveFile(mos, wxBITMAP_TYPE_PNG)) {
						const size_t len = mos.GetSize();
                        plate_info.thumbnailData.pixels.resize(len);
                        if (len > 0) {
                            mos.CopyTo(plate_info.thumbnailData.pixels.data(), len);
                        }
                    }
                }

                file_info.plates.emplace_back(plate_info);

                // y83
                if (support_p2p) {
                    std::string p2pKey = file_info.file_path + "|" + plate_info.index;
                    auto it = m_p2p_thumbnails.find(p2pKey);
                    if (it != m_p2p_thumbnails.end() && !it->second.empty()) {
                        // Update the already-emplaced plate_info's thumbnailData
                        auto &emplaced = file_info.plates.back();
                        emplaced.thumbnailData.pixels.assign(
                            (const unsigned char *)it->second.data(),
                            (const unsigned char *)it->second.data() + it->second.size());
                        BOOST_LOG_TRIVIAL(trace) << "QDSDeviceManager: use P2P thumbnail for "
                                                  << p2pKey << " (" << it->second.size() << " bytes)";
                    }
                } else {
                    DownloadManager::getInstance().downloadThumbnail(
                        UrlEncodeForFilename(plate_info.thumb_url),
                        file_info.file_name,
                        [device](ThumbnailResult result) {
                            if (!result.success)
                                return;

                            for (auto& file_info_item : device->file_info) {
                                for (auto& plate_info_item : file_info_item.plates) {
                                    if (UrlEncodeForFilename(plate_info_item.thumb_url) == result.url) {
                                        plate_info_item.thumbnailData.pixels.assign(result.png_data.begin(), result.png_data.end());
                                        return;
                                    }
                                }
                            }
                        }
                    );
                }
            }
        }
        file_info.show_thumb_url = file_info.plates.empty() ? "" : file_info.plates[0].thumb_url;
        
        
        const auto& thumbnails = file_item["thumbnails"];
        for (const auto& thumbnailItem : thumbnails) {
            file_info.thumbnailsSize = thumbnailItem["data_size"].get<int>();
            break;
        }


        device->file_info.emplace_back(file_info);
    }       
    device->m_fresh_file_info = true;
}

void QDSDeviceManager::updatePrintThumbUrl(std::shared_ptr<QDSDevice>& device, const json& message){
//y83
    try{
        const json& result = message.at("params")[0];
        if(result["action"] == "added"){
            BOOST_LOG_TRIVIAL(trace) << result;
            device->m_print_filename = result["job"]["filename"];
            std::string thumb_path = result["job"]["metadata"]["thumbnails"]["relative_path"];
            device->m_print_png_url = device->m_frp_url + "/server/files/gcodes/" + thumb_path;
        }
        else if (result["action"] == "finished") {
            device->m_print_filename = "";
            device->m_print_png_url = "";
        }
    }
    catch(...) {
        BOOST_LOG_TRIVIAL(error) << __FUNCTION__ << "get json error in message, message is "<< message;
    }
}

void QDSDeviceManager::updatePrintThumbUrlWithOutMsg(std::shared_ptr<QDSDevice>& device){
        if(!device->file_info.empty() /*&& device->m_print_png_url.empty()*/){
        std::string print_file_name = device->m_print_filename;
        std::vector<GCodeFileInfo> files_info = device->file_info;
        for(auto file_ : files_info){
			if (file_.file_name == print_file_name)
            {
                
                device->m_print_png_url = UrlEncodeForFilename(file_.show_thumb_url);
            }
        }
    }
}

void QDSDeviceManager::updateDeviceStatus(const std::string& device_id, std::string new_status) {
    bool should_callback = false;

    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        auto dev_it = devices_.find(device_id);
        if (dev_it != devices_.end()) {
            if (!new_status.empty()) {
                dev_it->second->m_status = new_status;
                should_callback = true;
            }
        }
    }

    if (should_callback) {
        auto callback = getConnectionEventCallback();
        if (callback) {
            callback(device_id, new_status);
        }
    }
}

void QDSDeviceManager::processConnectionStatus(const std::string& device_id, 
                                               const std::string& status) {
    updateDeviceStatus(device_id, status);
}

void QDSDeviceManager::stopAllConnection() {
    std::vector<std::string> device_ids;
    
    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        if (connections_.empty()) return;
        
        for (const auto& pair : connections_) {
            device_ids.push_back(pair.first);
        }
    }
    
    for (const auto& device_id : device_ids) {
        disconnectDevice(device_id);
    }
    
    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        devices_.clear();
    }
}

std::string QDSDeviceManager::getDeviceTempNozzle(const std::string& deviceId)
{
    std::lock_guard<std::mutex> lock(manager_mutex_);
    auto dev_it = devices_.find(deviceId);
    return (dev_it != devices_.end()) ? dev_it->second->m_extruder_temperature : "0.0";
}

std::string QDSDeviceManager::getDeviceTempBed(const std::string& deviceId)
{
    std::lock_guard<std::mutex> lock(manager_mutex_);
    auto dev_it = devices_.find(deviceId);
    return (dev_it != devices_.end()) ? dev_it->second->m_bed_temperature : "0.0";
}

std::string QDSDeviceManager::getDeviceTempChamber(const std::string& deviceId)
{
    std::lock_guard<std::mutex> lock(manager_mutex_);
    auto dev_it = devices_.find(deviceId);
    return (dev_it != devices_.end()) ? dev_it->second->m_chamber_temperature : "0.0";
}

void QDSDeviceManager::setSelected(const std::string& device_id){
    std::shared_ptr<QDSDevice> device = nullptr;
    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        for (auto [device_id, device] : devices_) {
            device->is_selected = false;
        }

        auto dev_it = devices_.find(device_id);
        if (dev_it == devices_.end()) {
            return;
        }
        device = dev_it->second;
        device->is_selected = true;
        //cj_4
        device->m_polar_cooler_dirty_for_ui = true;
    }
}

void QDSDeviceManager::unSelected(){
    std::lock_guard<std::mutex> lock(manager_mutex_);
    for (auto [device_id, device] : devices_) {
        device->is_selected = false;
    }
}

//y76
#if QDT_RELEASE_TO_PUBLIC
void QDSDeviceManager::setNetDevices(std::vector<NetDevice> devices){
    std::lock_guard<std::mutex> lock(manager_mutex_);
    net_devices = devices;
}

std::vector<NetDevice> QDSDeviceManager::getNetDevices(){
    std::lock_guard<std::mutex> lock(manager_mutex_);
    return net_devices;
}
#endif

bool QDSDeviceManager::upBoxInfoToBoxMsg(std::shared_ptr<QDSDevice>& device){
    std::vector<int> slot_state(17, 0);
    std::vector<int> slot_id(17, -1);
    std::vector<std::string> filament_id(17);
    std::vector<std::string> filament_colors(17);
    std::vector<std::string> filament_type(17);

    if (device == nullptr || wxGetApp().preset_bundle == nullptr) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": no selected QDS device or preset bundle";
        return false;
    }

    auto &selected_preset = wxGetApp().preset_bundle->printers.get_edited_preset();
    const std::string selected_model = selected_preset.config.opt_string("printer_model");
    const std::string box_id = selected_preset.config.opt_string("box_id");
    const auto *nozzle_opt = selected_preset.config.option<ConfigOptionFloatsNullable>("nozzle_diameter");
    if (selected_model.empty() || box_id.empty() || nozzle_opt == nullptr || nozzle_opt->values.empty()) {
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": selected printer profile lacks model, Box id, or nozzle metadata";
        return false;
    }

    QDSBoxSync::PrinterMetadata metadata;
    QDSBoxSync::BoxSnapshotInput snapshot_input;
    int auto_reload_detect = 0;
    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        metadata = device->getPrinterMetadata();
        snapshot_input = device->m_box_snapshot_input;
        auto_reload_detect = device->m_auto_reload_detect;
    }

    const auto compatibility = QDSBoxSync::resolve_compatibility(metadata, selected_model, nozzle_opt->values.front());
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": compatibility=" << compatibility.compatible
                            << " selected_model=" << selected_model
                            << " model_fallback=" << compatibility.used_selected_model_fallback
                            << " nozzle_fallback=" << compatibility.used_selected_nozzle_fallback
                            << " reason=" << compatibility.reason;
    if (!compatibility.compatible)
        return false;

    snapshot_input.box_id = box_id;
    auto snapshot = QDSBoxSync::normalize_snapshot(snapshot_input);
    for (const auto &diagnostic : snapshot.diagnostics)
        BOOST_LOG_TRIVIAL(warning) << __FUNCTION__ << ": " << diagnostic;

    for (const auto &slot : snapshot.slots) {
        const size_t index = static_cast<size_t>(slot.slot_index);
        if (index >= slot_state.size())
            continue;
        slot_state[index]      = 1;
        slot_id[index]         = slot.slot_index;
        filament_id[index]     = slot.filament_preset_id.value_or("");
        filament_colors[index] = slot.colour.value_or("#CECECE");
        filament_type[index]   = slot.material_type.value_or("");
        BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << ": Box slot=" << slot.slot_index
                                << " preset_id=" << filament_id[index]
                                << " material=" << slot.material_name.value_or(filament_type[index])
                                << " colour=" << filament_colors[index];
    }
    if (snapshot.external_spool) {
        constexpr size_t external_index = 16;
        slot_state[external_index]      = 1;
        slot_id[external_index]         = 16;
        filament_id[external_index]     = snapshot.external_spool->filament_preset_id.value_or("");
        filament_colors[external_index] = snapshot.external_spool->colour.value_or("#CECECE");
        filament_type[external_index]   = snapshot.external_spool->material_type.value_or("");
    }

    {
        std::lock_guard<std::mutex> lock(manager_mutex_);
        device->m_box_snapshot    = snapshot;
        device->m_filament_colors = filament_colors;
        device->m_filament_type   = filament_type;
        device->m_filament_id     = filament_id;
        device->m_slot_id         = slot_id;
        device->m_slot_state      = slot_state;
    }

    GUI::wxGetApp().sidebar().update_sync_status(device);

    wxGetApp().plater()->box_msg.slot_state = slot_state;
    wxGetApp().plater()->box_msg.filament_id = filament_id;
    wxGetApp().plater()->box_msg.filament_colors = filament_colors;
    wxGetApp().plater()->box_msg.box_count = snapshot.box_count;
    wxGetApp().plater()->box_msg.filament_type = filament_type;
    wxGetApp().plater()->box_msg.slot_id = slot_id;
    wxGetApp().plater()->box_msg.auto_reload_detect = auto_reload_detect;
    wxGetApp().plater()->box_msg.box_list_preset_name = selected_preset.name;
    //y78
    if(snapshot.box_count > 0)
        wxGetApp().plater()->sidebar().box_list_printer_ip = device->m_ip;
    else
        wxGetApp().plater()->sidebar().box_list_printer_ip = "";

    GUI::wxGetApp().sidebar().load_box_list();
    return true;
}

//y83
bool QDSDeviceManager::getFileInfoViaP2P()
{
#if QDT_RELEASE_TO_PUBLIC
    auto &p2p = P2PManager::instance();

    // ── Step 1: fetch file list (text command) ──
    std::mutex syncMutex;
    std::condition_variable syncCV;
    bool listReceived = false;
    std::string fileListJson;

    int textToken = p2p.onText([&](uint8_t type, int64_t reqId, int32_t,
                                    const uint8_t *data, size_t len) {
        std::string text((const char *)data, len);
        BOOST_LOG_TRIVIAL(trace) << "QDSDeviceManager: fetch_model_list response, reqId=" << reqId;
        {
            std::lock_guard<std::mutex> lock(syncMutex);
            fileListJson = text;
            listReceived = true;
        }
        syncCV.notify_one();
    });

    int64_t listReqId = (int64_t)(std::chrono::system_clock::now().time_since_epoch().count());
    bool sent = false;
    for (int retry = 0; retry < 5; retry++) {
        if (p2p.sendTextCommand(R"({"method":"fetch_model_list"})", listReqId) >= 0) {
            sent = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!sent) {
        BOOST_LOG_TRIVIAL(error) << "QDSDeviceManager: failed to send fetch_model_list";
        p2p.off(textToken);
        syncCV.notify_one();
    }

    {
        std::unique_lock<std::mutex> lock(syncMutex);
        if (!syncCV.wait_for(lock, std::chrono::seconds(30), [&] { return listReceived; })) {
            BOOST_LOG_TRIVIAL(error) << "QDSDeviceManager: fetch_model_list timeout";
            p2p.off(textToken);
            return false;
        }
    }

    // Keep text token alive; we may unregister after thumbnails
    m_text_from_p2p = fileListJson;

    // ── Step 2: build thumbnail request list ──
    struct ThumbReq {
        std::string filePath;
        std::string plateIndex;
    };
    std::vector<ThumbReq> pendingThumbs;

    try {
        json parsed = json::parse(fileListJson);
        json arr = parsed.is_array() ? parsed : (parsed.contains("result") ? parsed["result"] : parsed);
        if (arr.is_array()) {
            for (const auto &file : arr) {
                std::string filePath = file.value("filepath", "");
                if (filePath.find("/.cache/") != std::string::npos)
                    continue;
                if (file.contains("plates") && file["plates"].is_array()) {
                    for (const auto &plate : file["plates"]) {
                        ThumbReq req;
                        req.filePath   = filePath;
                        req.plateIndex = plate.value("plate_index", "0");
                        pendingThumbs.push_back(req);
                    }
                }
            }
        }
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "QDSDeviceManager: parse file list failed: " << e.what();
        p2p.off(textToken);
        return false;
    }

    // ── Step 3: fetch thumbnails one by one ──
    if (!pendingThumbs.empty()) {
        m_p2p_thumbnails.clear();

        // Shared state accessed from both main thread and P2P event-loop thread
        std::mutex              xferMutex;
        std::condition_variable xferCV;
        bool                    xferDone   = false;
        bool                    xferCancel = false;
        std::vector<char>       xferBuf;

        int fileToken = p2p.onFile([&](uint8_t type, int64_t transferId, int32_t sequence,
                                        const uint8_t *data, size_t len) {
            if (type == 0x20) { // FILE_BEGIN
                std::lock_guard<std::mutex> lock(xferMutex);
                xferBuf.clear();
                xferDone   = false;
                xferCancel = false;
            } else if (type == 0x21) { // FILE_CHUNK
                std::lock_guard<std::mutex> lock(xferMutex);
                xferBuf.insert(xferBuf.end(), data, data + len);
            } else if (type == 0x22) { // FILE_END
                {
                    std::lock_guard<std::mutex> lock(xferMutex);
                    xferDone = true;
                }
                xferCV.notify_one();
            } else if (type == 0x23) { // FILE_CANCEL
                {
                    std::lock_guard<std::mutex> lock(xferMutex);
                    xferCancel = true;
                    xferBuf.clear();
                }
                xferCV.notify_one();
            }
        });

        for (size_t i = 0; i < pendingThumbs.size(); i++) {
            const auto &req = pendingThumbs[i];
            // Reset state under lock
            {
                std::lock_guard<std::mutex> lock(xferMutex);
                xferBuf.clear();
                xferDone   = false;
                xferCancel = false;
            }

            json imgReq;
            imgReq["method"]              = "request_model_image";
            imgReq["params"]["file_path"]   = req.filePath;
            imgReq["params"]["plate_index"] = req.plateIndex;

            int64_t imgReqId = (int64_t)(std::chrono::system_clock::now().time_since_epoch().count() + i + 1);
            p2p.sendTextCommand(imgReq.dump(), imgReqId);
            BOOST_LOG_TRIVIAL(trace) << "QDSDeviceManager: request thumbnail for "
                                     << req.filePath << " plate=" << req.plateIndex;

            // Wait for file transfer to complete (max 15s per thumbnail)
            std::vector<char> received;
            {
                std::unique_lock<std::mutex> lock(xferMutex);
                bool ok = xferCV.wait_for(lock, std::chrono::seconds(15),
                                          [&] { return xferDone || xferCancel; });
                if (ok && xferDone && !xferBuf.empty()) {
                    received = std::move(xferBuf);
                }
            }

            if (!received.empty()) {
                std::string key = req.filePath + "|" + req.plateIndex;
                m_p2p_thumbnails[key] = std::move(received);
                BOOST_LOG_TRIVIAL(trace) << "QDSDeviceManager: received thumbnail ("
                                         << m_p2p_thumbnails[key].size() << " bytes) for "
                                         << req.filePath << " plate=" << req.plateIndex;
            } else {
                BOOST_LOG_TRIVIAL(warning) << "QDSDeviceManager: thumbnail fetch "
                                           << "failed for " << req.filePath
                                           << " plate=" << req.plateIndex;
            }
        }




        p2p.off(fileToken);
    }

    p2p.off(textToken);
#endif
    return true;
}

//y83
bool QDSDeviceManager::getTimelapseInfoP2P(){
#if QDT_RELEASE_TO_PUBLIC
    auto &p2p = P2PManager::instance();

    // ── Step 1: fetch timelapse list (text command) ──
    std::mutex syncMutex;
    std::condition_variable syncCV;
    bool listReceived = false;
    std::string fileListJson;

    int textToken = p2p.onText([&](uint8_t type, int64_t reqId, int32_t,
                                    const uint8_t *data, size_t len) {
        std::string text((const char *)data, len);
        BOOST_LOG_TRIVIAL(trace) << "QDSDeviceManager: fetch_timelapse_list response, reqId=" << reqId;
        {
            std::lock_guard<std::mutex> lock(syncMutex);
            fileListJson = text;
            listReceived = true;
        }
        syncCV.notify_one();
    });

    int64_t listReqId = (int64_t)(std::chrono::system_clock::now().time_since_epoch().count());
    bool sent = false;
    for (int retry = 0; retry < 5; retry++) {
        if (p2p.sendTextCommand(R"({"method":"fetch_timelapse_list","params":{}})", listReqId) >= 0) {
            sent = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (!sent) {
        BOOST_LOG_TRIVIAL(error) << "QDSDeviceManager: failed to send fetch_timelapse_list";
        p2p.off(textToken);
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(syncMutex);
        if (!syncCV.wait_for(lock, std::chrono::seconds(30), [&] { return listReceived; })) {
            BOOST_LOG_TRIVIAL(error) << "QDSDeviceManager: fetch_timelapse_list timeout";
            p2p.off(textToken);
            return false;
        }
    }

    m_text_from_p2p = fileListJson;

    // ── Step 2: extract .jpg thumbnail filenames from the list ──
    struct JpgReq {
        std::string jpgFileName;  // e.g. "timelapse_20240718.mp4" → "timelapse_20240718.jpg"
    };
    std::vector<JpgReq> pendingJpgs;

    try {
        json parsed = json::parse(fileListJson);
        // The JSON may be a direct array or wrapped in {"result": {...}}
        const json *pArr = nullptr;
        if (parsed.is_array()) {
            pArr = &parsed;
        } else if (parsed.contains("result") && parsed["result"].is_object()
                   && parsed["result"].contains("files") && parsed["result"]["files"].is_array()) {
            pArr = &parsed["result"]["files"];
        }
        if (pArr) {
            std::unordered_set<std::string> allNames;
            for (const auto &f : *pArr) {
                if (f.is_object() && f.contains("filename") && f["filename"].is_string())
                    allNames.insert(f["filename"].get<std::string>());
            }
            for (const auto &f : *pArr) {
                if (!f.is_object() || !f.contains("filename") || !f["filename"].is_string())
                    continue;
                std::string fname = f["filename"].get<std::string>();
                size_t dot = fname.rfind('.');
                if (dot == std::string::npos)
                    continue;
                std::string ext = fname.substr(dot);
                for (char &c : ext)
                    c = (char)std::tolower((unsigned char)c);
                if (ext != ".mp4")
                    continue;
                std::string jpgName = fname.substr(0, dot) + ".jpg";
                if (allNames.find(jpgName) != allNames.end()) {
                    pendingJpgs.push_back({std::move(jpgName)});
                }
            }
        }
    } catch (const std::exception &e) {
        BOOST_LOG_TRIVIAL(error) << "QDSDeviceManager: parse timelapse list failed: " << e.what();
        p2p.off(textToken);
        return false;
    }

    // ── Step 3: fetch thumbnail images one by one ──
    if (!pendingJpgs.empty()) {
        m_p2p_timelapse_thumbnails.clear();

        std::mutex              xferMutex;
        std::condition_variable xferCV;
        bool                    xferDone   = false;
        bool                    xferCancel = false;
        std::vector<char>       xferBuf;

        int fileToken = p2p.onFile([&](uint8_t type, int64_t transferId, int32_t sequence,
                                        const uint8_t *data, size_t len) {
            if (type == 0x20) { // FILE_BEGIN
                std::lock_guard<std::mutex> lock(xferMutex);
                xferBuf.clear();
                xferDone   = false;
                xferCancel = false;
            } else if (type == 0x21) { // FILE_CHUNK
                std::lock_guard<std::mutex> lock(xferMutex);
                const uint8_t* jpegData = data + 12;
                size_t jpegLen = len - 12;
                xferBuf.insert(xferBuf.end(), jpegData, jpegData + jpegLen);
            } else if (type == 0x22) { // FILE_END
                {
                    std::lock_guard<std::mutex> lock(xferMutex);
                    xferDone = true;
                }
                xferCV.notify_one();
            } else if (type == 0x23) { // FILE_CANCEL
                {
                    std::lock_guard<std::mutex> lock(xferMutex);
                    xferCancel = true;
                    xferBuf.clear();
                }
                xferCV.notify_one();
            }
        });

        for (size_t i = 0; i < pendingJpgs.size(); i++) {
            const auto &req = pendingJpgs[i];
            {
                std::lock_guard<std::mutex> lock(xferMutex);
                xferBuf.clear();
                xferDone   = false;
                xferCancel = false;
            }

            // Request the .jpg thumbnail file via P2P
            json imgReq;
            imgReq["method"] = "request_file";
            imgReq["params"]["file_path"] = "/home/qidi/printer_data/timelapse/" + req.jpgFileName;
            int64_t imgReqId = (int64_t)(std::chrono::system_clock::now().time_since_epoch().count() + i + 1);
            p2p.sendTextCommand(imgReq.dump(), imgReqId);
            BOOST_LOG_TRIVIAL(trace) << "QDSDeviceManager: request timelapse thumbnail " << req.jpgFileName;

            // Wait for file transfer (max 15s per thumbnail)
            std::vector<char> received;
            {
                std::unique_lock<std::mutex> lock(xferMutex);
                bool ok = xferCV.wait_for(lock, std::chrono::seconds(15),
                                          [&] { return xferDone || xferCancel; });
                if (ok && xferDone && !xferBuf.empty()) {
                    received = std::move(xferBuf);
                }
            }

            if (!received.empty()) {
                m_p2p_timelapse_thumbnails[req.jpgFileName] = std::move(received);
                BOOST_LOG_TRIVIAL(trace) << "QDSDeviceManager: received timelapse thumbnail "
                                         << req.jpgFileName
                                         << " (" << m_p2p_timelapse_thumbnails[req.jpgFileName].size() << " bytes)";
            } else {
                BOOST_LOG_TRIVIAL(warning) << "QDSDeviceManager: failed to get timelapse thumbnail "
                                           << req.jpgFileName;
            }
        }

        p2p.off(fileToken);
    }

    p2p.off(textToken);
#endif
    return true;
}

void QDSDeviceManager::getFileInfo(const std::string& device_id){
    std::shared_ptr<QDSDevice> device = getDevice(device_id);

    //y83
    if(device && device->active_p2p){
        new std::thread([this, &device_id](){
            std::shared_ptr<QDSDevice> device = getDevice(device_id);
            bool has_p2p_result = getFileInfoViaP2P();
            if(has_p2p_result){
                json bodyJson = json::parse(m_text_from_p2p);
                updateDeviceFileInfo(device, bodyJson, has_p2p_result);
            } else{
                BOOST_LOG_TRIVIAL(error) << "getFileInfo failed!";
            }

            bool p2p_get_timelapse_file = getTimelapseInfoP2P();
            if(p2p_get_timelapse_file){
                updateDeviceTimelapseFileInfo(device, m_text_from_p2p);

                auto file_cb = getFileInfoUpdateCallback();
                if (file_cb) {
                    file_cb(device_id);
                }
            } else {
                BOOST_LOG_TRIVIAL(error) << "getTimelapseInfo failed!";
            }
        });
        return;
    }

    new std::thread([this, &device_id]() {
        std::shared_ptr<QDSDevice> device = getDevice(device_id);
        if (!device) {
            return;
        }

        std::string api_url = device->m_frp_url + "/api/qidiclient/files/list";

        auto http = Http::get(std::move(api_url));

        http.on_error([&](std::string body, std::string error, unsigned status) {
            //BOOST_LOG_TRIVIAL(trace) << boost::format("Error getting version: %1%, HTTP %2%, body: `%3%`") % error % status % body;

            })
            .on_complete([&, this](std::string body, unsigned) {
                try {
                    json bodyJson = json::parse(body);
                    if (bodyJson.contains("result"))
                        updateDeviceFileInfo(device, bodyJson);
                }
                catch (const std::exception& error) {
                    BOOST_LOG_TRIVIAL(trace) << "json error " << error.what();
                };
                })
                .perform_sync();

        //cj_3
        const std::string timelapse_dir_url =
            device->m_frp_url + "/server/files/directory?root=timelapse&path=timelapse&extended=true";
        auto http_timelapse = Http::get(timelapse_dir_url);
        http_timelapse
            .on_error([&](std::string body, std::string error, unsigned status) {
            (void)body;
            (void)error;
            (void)status;
                })
            .on_complete([&, this](std::string body, unsigned) {
                    updateDeviceTimelapseFileInfo(device, body);
                })
                    .perform_sync();

        auto file_cb = getFileInfoUpdateCallback();
        if (file_cb) {
            file_cb(device_id);
        }
        });
}

//cj_3
void QDSDeviceManager::updateDeviceTimelapseFileInfo(std::shared_ptr<QDSDevice>& device, const std::string& response_body)
{
    if (!device) {
        return;
    }

    device->timelapse_file_info.clear();

    try {
        json bodyJson = json::parse(response_body);

        //cj_3
        if (!bodyJson.contains("result") || !bodyJson["result"].is_object()) {
            device->m_fresh_timelapse_file_info = true;
            return;
        }
        const json& res = bodyJson["result"];
        if (!res.contains("files") || !res["files"].is_array()) {
            device->m_fresh_timelapse_file_info = true;
            return;
        }
        const json& files = res["files"];

        std::unordered_set<std::string> name_set;
        for (const auto& f : files) {
            if (f.is_object() && f.contains("filename") && f["filename"].is_string()) 
                name_set.insert(f["filename"].get<std::string>());
            
        }

        for (const auto& f : files) {
            if (!f.is_object() || !f.contains("filename") || !f["filename"].is_string())
                continue;
            const std::string fname = f["filename"].get<std::string>();
            const size_t dot = fname.rfind('.');
            if (dot == std::string::npos || dot + 4 > fname.size())
                continue;
            std::string ext = fname.substr(dot);
            for (char& c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext != ".mp4")
                continue;

            TimelapseFileInfo info;
            info.file_name = fname;
            if (f.contains("size")) {
                std::uint64_t size_bytes = 0;
                bool            have_size = false;
                if (f["size"].is_number_integer()) {
                    const auto v = f["size"].get<std::int64_t>();
                    size_bytes = v > 0 ? static_cast<std::uint64_t>(v) : 0;
                    have_size  = true;
                } else if (f["size"].is_number_unsigned()) {
                    size_bytes = f["size"].get<std::uint64_t>();
                    have_size  = true;
                } else if (f["size"].is_number_float()) {
                    const double d = f["size"].get<double>();
                    if (d > 0 && std::isfinite(d))
                        size_bytes = static_cast<std::uint64_t>(d);
                    have_size = true;
                }
                if (have_size)
                    info.file_size = format_timelapse_file_size_b_kb_mb(size_bytes);
            }
            if (f.contains("modified") && f["modified"].is_number()) {
                const double mod = f["modified"].get<double>();
                wxDateTime dt(static_cast<time_t>(std::llround(mod)));
                info.modified_time = std::string(dt.Format("%Y/%m/%d %H:%M").utf8_string());
            }

            const std::string jpg_name = fname.substr(0, dot) + ".jpg";
            

            // Try to populate thumbnail from P2P-fetched data first
            auto p2pIt = m_p2p_timelapse_thumbnails.find(jpg_name);
            if (p2pIt != m_p2p_timelapse_thumbnails.end() && !p2pIt->second.empty()) {
                info.thumbnailData.pixels.assign(
                    (const unsigned char *)p2pIt->second.data(),
                    (const unsigned char *)p2pIt->second.data() + p2pIt->second.size());
                BOOST_LOG_TRIVIAL(trace) << "QDSDeviceManager: use P2P timelapse thumbnail "
                                         << jpg_name << " (" << p2pIt->second.size() << " bytes)";
            }


            if (name_set.find(jpg_name) != name_set.end()) {
                info.thumb_url = device->m_frp_url + "/server/files/timelapse/" + UrlEncodeForFilename(jpg_name);
            }

            device->timelapse_file_info.push_back(std::move(info));
        }
    }
    catch (const std::exception& err) {
        BOOST_LOG_TRIVIAL(trace) << "timelapse directory json error " << err.what();
    }

    device->m_fresh_timelapse_file_info = true;
}

void QDSDeviceManager::resetBoxUpdateStatus(const std::string& device_id) {
    std::shared_ptr<QDSDevice> device = getDevice(device_id);
    if (device) {
        device->reset_update_status();
    }
}
}}
