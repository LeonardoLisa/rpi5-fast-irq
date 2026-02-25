/**
 * @file cps_monitor.cpp
 * @version 1.0.0
 * @date 2026-02-25
 * @author Leonardo Lisa
 * @brief Real-time Counts Per Second (CPS) monitor for GPIO interrupts with ANSI terminal UI.
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

// ANSI escape codes for terminal styling
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
std::atomic<uint32_t> g_pulse_count{0};

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
    
    // Hide standard terminal cursor to prevent flickering during redraws
    std::cout << HIDE_CURSOR;
    print_banner();

    RpiFastIrq irq_handler("/dev/rp1_gpio_irq");

    // Callback increments the local counter lock-free
    auto my_irq_callback = [](const GpioIrqEvent& /*event*/) {
        g_pulse_count.fetch_add(1, std::memory_order_relaxed);
    };

    if (!irq_handler.start(my_irq_callback)) {
        std::cerr << ANSI_RED << "[Error] Failed to start IRQ listener." << ANSI_RESET << "\n";
        std::cout << SHOW_CURSOR;
        return 1;
    }

    // Align loop to a precise 1-second boundary
    auto next_tick = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    
    while (g_keep_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_until(next_tick);
        
        if (!g_keep_running.load(std::memory_order_acquire)) break;

        // Read and reset the counter atomically
        uint32_t current_cps = g_pulse_count.exchange(0, std::memory_order_relaxed);
        
        // Dynamic color coding based on frequency rate
        const char* color_code = ANSI_GREEN;
        if (current_cps > 50000) {
            color_code = ANSI_RED;
        } else if (current_cps > 10000) {
            color_code = ANSI_YELLOW;
        }

        // Carriage return (\r) moves cursor to the beginning of the line
        // CLEAR_LINE (\033[K) erases the current line to prevent trailing artifacts
        std::cout << "\r" << CLEAR_LINE
                  << ANSI_BOLD << " Live CPS: " << color_code << std::setw(8) << current_cps 
                  << ANSI_RESET << " Hz" << std::flush;

        next_tick += std::chrono::seconds(1);
    }

    irq_handler.stop();
    
    std::cout << "\n\n" << ANSI_YELLOW << "[System] Monitor stopped cleanly." << ANSI_RESET << "\n";
    std::cout << SHOW_CURSOR;

    return 0;
}