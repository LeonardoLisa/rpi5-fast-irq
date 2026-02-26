/**
 * @file cps_monitor.cpp
 * @version 2.1.0
 * @date 2026-02-25
 * @author Leonardo Lisa
 * @brief Real-time CPS monitor for GPIO interrupts based on absolute Hardware Timestamps.
 * @requirements RpiFastIrq library, kernel module loaded, root privileges for SCHED_FIFO.
 * * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <csignal>
#include <iomanip>
#include "RpiFastIrq.hpp"

#define ANSI_RESET   "\033[0m"
#define ANSI_BOLD    "\033[1m"
#define ANSI_CYAN    "\033[36m"
#define ANSI_GREEN   "\033[32m"
#define ANSI_YELLOW  "\033[33m"
#define ANSI_RED     "\033[31m"
#define CLEAR_SCREEN "\033[2J\033[H"
#define CLEAR_LINE   "\033[K"
#define HIDE_CURSOR  "\033[?25l"
#define SHOW_CURSOR  "\033[?25h"

std::atomic<bool> g_keep_running{true};

// Store the hard real-time data generated directly by the kernel
std::atomic<uint64_t> g_latest_timestamp_ns{0};
std::atomic<uint32_t> g_latest_event_counter{0};

void signal_handler(int signum) {
    (void)signum;
    g_keep_running.store(false, std::memory_order_release);
}

void print_banner() {
    std::cout << CLEAR_SCREEN;
    std::cout << ANSI_CYAN << ANSI_BOLD;
    std::cout << "  _____  _____  _____   __  __             _ _             \n";
    std::cout << " / ____|  __ \\/ ____| |  \\/  |           (_) |            \n";
    std::cout << "| |    | |__) | (___  | \\  / | ___  _ __  _| |_ ___  _ __ \n";
    std::cout << "| |    |  ___/ \\___ \\ | |\\/| |/ _ \\| '_ \\| | __/ _ \\| '__|\n";
    std::cout << "| |____| |     ____) || |  | | (_) | | | | | || (_) | |   \n";
    std::cout << " \\_____|_|    |_____/ |_|  |_|\\___/|_| |_|_|\\__\\___/|_|   \n";
    std::cout << ANSI_RESET << "\n";
    std::cout << "===========================================================\n";
    std::cout << " Listening on /dev/rp1_gpio_irq | Press Ctrl+C to stop\n";
    std::cout << "===========================================================\n\n";
}

int main() {
    std::signal(SIGINT, signal_handler);
    std::cout << HIDE_CURSOR;
    print_banner();

    RpiFastIrq irq_handler("/dev/rp1_gpio_irq");

    // The callback no longer counts events manually. 
    // It simply stores the latest data packet certified by the kernel.
    auto my_irq_callback = [](const GpioIrqEvent& event) {
        g_latest_timestamp_ns.store(event.timestamp_ns, std::memory_order_relaxed);
        g_latest_event_counter.store(event.event_counter, std::memory_order_relaxed);
    };

    if (!irq_handler.start(my_irq_callback)) {
        std::cerr << ANSI_RED << "[Error] Failed to start IRQ listener." << ANSI_RESET << "\n";
        std::cout << SHOW_CURSOR;
        return 1;
    }

    // Wait briefly to synchronize the first events
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    uint64_t prev_ts = g_latest_timestamp_ns.load(std::memory_order_relaxed);
    uint32_t prev_counter = g_latest_event_counter.load(std::memory_order_relaxed);

    auto next_tick = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    
    while (g_keep_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_until(next_tick);
        if (!g_keep_running.load(std::memory_order_acquire)) break;

        uint64_t curr_ts = g_latest_timestamp_ns.load(std::memory_order_relaxed);
        uint32_t curr_counter = g_latest_event_counter.load(std::memory_order_relaxed);
        
        uint32_t current_cps = 0;
        uint64_t dt_ns = 0;
        uint32_t delta_events = 0;

        // Calculate the true frequency based on the Raspberry Pi hardware clock
        if (curr_counter > prev_counter && curr_ts > prev_ts) {
            dt_ns = curr_ts - prev_ts;
            double dt_sec = dt_ns / 1e9;
            delta_events = curr_counter - prev_counter;
            current_cps = static_cast<uint32_t>((delta_events / dt_sec) + 0.5); // Round to nearest integer
        }

        prev_ts = curr_ts;
        prev_counter = curr_counter;
        
        const char* color_code = ANSI_GREEN;
        if (current_cps > 50000) color_code = ANSI_RED;
        else if (current_cps > 10000) color_code = ANSI_YELLOW;

        /* Print UI and hardware debug data
        std::cout << "\r" << CLEAR_LINE
                  << ANSI_BOLD << " Live Rate: " << color_code << std::setw(8) << current_cps 
                  << ANSI_RESET << " cps"
                  << " | Debug -> Delta TS: " << dt_ns << " ns"
                  << " | Delta Events: " << delta_events
                  << std::flush;
        */

        next_tick += std::chrono::seconds(1);
    }
    
    irq_handler.stop();
    std::cout << "\n\n" << ANSI_YELLOW << "[System] Monitor stopped cleanly." << ANSI_RESET << "\n";
    std::cout << SHOW_CURSOR;

    return 0;
}