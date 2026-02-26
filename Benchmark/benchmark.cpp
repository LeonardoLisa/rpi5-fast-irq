/**
 * @file benchmark.cpp
 * @version 1.2.0
 * @date 2026-02-23
 * @author Leonardo Lisa
 * @brief Benchmark application for Rpi5 GPIO IRQ Latency and Jitter analysis.
 * Requirements: RpiFastIrq library, isolated CPU core 3, rpi_fast_irq kernel module.
 * * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <iostream>
#include <vector>
#include <atomic>
#include <csignal>
#include <thread>
#include <chrono>
#include <fstream>
#include <string>
#include <iomanip>
#include <sstream>
#include "RpiFastIrq.hpp"

template <typename T, size_t Size>
class LockFreeRingBuffer {
private:
    T m_data[Size];
    std::atomic<size_t> m_head{0};
    std::atomic<size_t> m_tail{0};

public:
    bool push(const T& item) {
        size_t current_head = m_head.load(std::memory_order_relaxed);
        if (current_head - m_tail.load(std::memory_order_acquire) >= Size) {
            return false;
        }
        m_data[current_head % Size] = item;
        m_head.store(current_head + 1, std::memory_order_release);
        return true;
    }

    bool pop(T& item) {
        size_t current_tail = m_tail.load(std::memory_order_relaxed);
        if (current_tail == m_head.load(std::memory_order_acquire)) {
            return false; 
        }
        item = m_data[current_tail % Size];
        m_tail.store(current_tail + 1, std::memory_order_release);
        return true;
    }
};

std::atomic<bool> g_keep_running{true};
std::atomic<bool> g_capture_active{false};
LockFreeRingBuffer<GpioIrqEvent, 1024> g_event_buffer; 

void print_header() {
    std::cout << R"(
    ____  ____  _   ______               __  ___            
   / __ \/ __ \(_) / ____/___ _ _____  / /_/  _/________ _ 
  / /_/ / /_/ / / / /_  / __ `// ___/ / __// / / ___/ __ `/ 
 / _, _/ ____/ / / __/ / /_/ /(__  ) / /__/ / / /  / /_/ /  
/_/ |_/_/   /_/ /_/    \__,_//____/  \__/___/_/   \__, /   
                                                 /____/    
    )" << std::endl;
    std::cout << "==============================================================" << std::endl;
    std::cout << " RPI5-FAST-IRQ BENCHMARK TOOL - High Performance GPIO Monitor" << std::endl;
    std::cout << "==============================================================" << std::endl;
}

void signal_handler(int signum) {
    (void)signum; 
    g_keep_running = false;
}

std::string get_timestamp_filename() {
    auto now = std::chrono::system_clock::now();
    auto in_time_t = std::chrono::system_clock::to_time_t(now);
    std::stringstream ss;
    ss << "deltaevents_" << std::put_time(std::localtime(&in_time_t), "%H-%M-%S_%d-%m-%Y") << ".dat";
    return ss.str();
}

int main() {
    print_header();
    std::signal(SIGINT, signal_handler);

    RpiFastIrq irq_handler("/dev/rp1_gpio_irq");

    auto my_irq_callback = [](const GpioIrqEvent& event) {
        if (g_capture_active) {
            g_event_buffer.push(event);
        }
    };

    if (!irq_handler.start(my_irq_callback)) {
        std::cerr << "\033[31m[Error] Could not start IRQ listener.\033[0m" << std::endl;
        return 1;
    }

    std::cout << "\n[Status] Ready. Press ENTER to start benchmark..." << std::endl;
    
    // Wait for a single ENTER key press
    std::cin.get();

    std::cout << "[Running] Capturing... Press Ctrl+C to stop." << std::endl;
    g_capture_active = true;
    
    std::vector<uint64_t> deltas;
    deltas.reserve(1000000); 
    
    uint64_t last_timestamp = 0;
    uint32_t dropped_events = 0;
    uint32_t last_counter = 0;
    GpioIrqEvent event;
    auto last_ui_update = std::chrono::steady_clock::now();

    while (g_keep_running) {
        if (g_event_buffer.pop(event)) {
            if (last_counter != 0 && event.event_counter != last_counter + 1) {
                dropped_events += (event.event_counter - last_counter - 1);
            }
            last_counter = event.event_counter;

            if (last_timestamp != 0) {
                deltas.push_back(event.timestamp_ns - last_timestamp);
            }
            last_timestamp = event.timestamp_ns;

            // Non-blocking UI update rate-limited to 250ms
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_ui_update).count() >= 250) {
                std::cout << "\r[Running] Captured: " << deltas.size() 
                          << " | Dropped: " << dropped_events << std::flush;
                last_ui_update = now;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }
    std::cout << "\n";

    irq_handler.stop();

    std::string filename = get_timestamp_filename();
    std::cout << "\n[System] Saving to " << filename << "..." << std::endl;
    
    std::ofstream outfile(filename);
    if (outfile.is_open()) {
        for (const auto& d : deltas) outfile << d << "\n";
        outfile << "# Total_Samples: " << deltas.size() << "\n";
        outfile << "# Hardware_Dropped_Events: " << dropped_events << "\n";
        outfile.close();
    }

    return 0;
}