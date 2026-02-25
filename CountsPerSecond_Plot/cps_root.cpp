/**
 * @file cps_root.cpp
 * @version 1.0.0
 * @date 2026-02-25
 * @author Leonardo Lisa
 * @brief Real-time CPS monitor with ROOT GUI for Raspberry Pi 5.
 * @requirements RpiFastIrq library, kernel module loaded, ROOT framework installed.
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
#include <algorithm>
#include <TApplication.h>
#include <TCanvas.h>
#include <TGraph.h>
#include <TAxis.h>
#include <TSystem.h>
#include "RpiFastIrq.hpp"

std::atomic<bool> g_keep_running{true};
std::atomic<uint32_t> g_pulse_count{0};

void signal_handler(int signum) {
    (void)signum;
    g_keep_running.store(false, std::memory_order_release);
}

int main(int argc, char** argv) {
    std::signal(SIGINT, signal_handler);

    // Initialize ROOT application to handle GUI events
    TApplication app("CPS_ROOT_GUI", &argc, argv);

    // Setup Canvas
    auto canvas = new TCanvas("c_cps", "Real-Time CPS Monitor", 1000, 600);
    canvas->SetGrid();

    // Setup Graph with points connected by line segments (PL)
    auto graph = new TGraph();
    graph->SetTitle("Live Counts Per Second;Time (s);CPS (Hz)");
    graph->SetLineColor(kBlue);
    graph->SetLineWidth(2);
    graph->SetMarkerStyle(20);
    graph->SetMarkerSize(0.8);
    graph->SetMarkerColor(kRed);
    graph->Draw("APL"); 

    // Initialize hardware IRQ listener
    RpiFastIrq irq_handler("/dev/rp1_gpio_irq");

    // Atomic increment in lock-free context
    auto my_irq_callback = [](const GpioIrqEvent& /*event*/) {
        g_pulse_count.fetch_add(1, std::memory_order_relaxed);
    };

    if (!irq_handler.start(my_irq_callback)) {
        std::cerr << "[Error] Failed to start IRQ listener.\n";
        return 1;
    }

    std::cout << "[System] ROOT GUI started. Press Ctrl+C in terminal or close the window to exit.\n";

    int time_sec = 0;
    auto next_tick = std::chrono::steady_clock::now() + std::chrono::seconds(1);

    // Main GUI and Data Polling Loop
    while (g_keep_running.load(std::memory_order_acquire)) {
        // Process ROOT GUI events to keep window responsive
        gSystem->ProcessEvents();

        auto now = std::chrono::steady_clock::now();
        if (now >= next_tick) {
            // Extract and reset the pulse counter atomically
            uint32_t current_cps = g_pulse_count.exchange(0, std::memory_order_relaxed);
            
            // Append point to the graph
            graph->SetPoint(graph->GetN(), time_sec, current_cps);
            time_sec++;

            // Rescale X axis to maintain a sliding 60-second window for real-time tracking
            graph->GetXaxis()->SetLimits(std::max(0, time_sec - 60), std::max(60, time_sec + 5));
            
            // Auto-scale Y axis based on the dynamic range of the current dataset
            graph->GetYaxis()->UnZoom(); 
            
            // Force redraw
            canvas->Modified();
            canvas->Update();

            next_tick += std::chrono::seconds(1);
        }

        // 20ms sleep to prevent the GUI polling loop from hogging the CPU core
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    irq_handler.stop();
    return 0;
}