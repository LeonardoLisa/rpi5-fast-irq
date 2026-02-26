/**
 * @file cps_root.cpp
 * @version 1.1.0
 * @date 2026-02-26
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
#include <TMath.h>
#include "RpiFastIrq.hpp"

std::atomic<bool> g_keep_running{true};

// Store the hard real-time data generated directly by the kernel
std::atomic<uint64_t> g_latest_timestamp_ns{0};
std::atomic<uint32_t> g_latest_event_counter{0};

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
    graph->SetTitle("Live Counts Per Second;Time (s);cps");
    graph->SetLineColor(kBlue);
    graph->SetLineWidth(2);
    graph->SetMarkerStyle(20);
    graph->SetMarkerSize(0.8);
    graph->SetMarkerColor(kRed);
    // Draw is deferred until the first point is added to avoid PaintGraph errors

    // Initialize hardware IRQ listener
    RpiFastIrq irq_handler("/dev/rp1_gpio_irq");

    // The callback no longer counts events manually. 
    // It simply stores the latest data packet certified by the kernel.
    auto my_irq_callback = [](const GpioIrqEvent& event) {
        g_latest_timestamp_ns.store(event.timestamp_ns, std::memory_order_relaxed);
        g_latest_event_counter.store(event.event_counter, std::memory_order_relaxed);
    };

    if (!irq_handler.start(my_irq_callback)) {
        std::cerr << "[Error] Failed to start IRQ listener.\n";
        return 1;
    }

    std::cout << "[System] ROOT GUI started. Press Ctrl+C in terminal or close the window to exit.\n";

    // Wait briefly to synchronize the first events
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    uint64_t prev_ts = g_latest_timestamp_ns.load(std::memory_order_relaxed);
    uint32_t prev_counter = g_latest_event_counter.load(std::memory_order_relaxed);

    int time_sec = 0;
    auto next_tick = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    bool first_draw = true;

    // Main GUI and Data Polling Loop
    while (g_keep_running.load(std::memory_order_acquire)) {
        // Process ROOT GUI events to keep window responsive
        gSystem->ProcessEvents();

        auto now = std::chrono::steady_clock::now();
        if (now >= next_tick) {
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
            
            // Append point to the graph
            graph->SetPoint(graph->GetN(), time_sec, current_cps);
            time_sec++;

            // Rescale X axis to maintain a sliding 60-second window for real-time tracking
            graph->GetXaxis()->SetLimits(std::max(0, time_sec - 60), std::max(60, time_sec + 5));
            
            // Apply dynamic symmetric offset to Y axis
            if (graph->GetN() > 0) {
                double min_y = TMath::MinElement(graph->GetN(), graph->GetY());
                double max_y = TMath::MaxElement(graph->GetN(), graph->GetY());
                
                double offset = (max_y - min_y) * 0.1;
                if (offset == 0) offset = max_y * 0.1; 
                if (offset == 0) offset = 1.0; 
                
                graph->GetYaxis()->SetRangeUser(min_y - offset, max_y + offset);

                // Draw only after the first point is available
                if (first_draw) {
                    graph->Draw("APL");
                    first_draw = false;
                }
            }
            
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