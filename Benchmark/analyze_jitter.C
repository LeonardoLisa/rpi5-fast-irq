/*
 * -- Usage --
 * to load workspace:conda activate science
 * start root: root -l 'analyze_jitter.C("deltaevents_181210_23022026.dat")'
 * to exit: .q
 */

/*
 * ============================================================================
 * NOTE ON DATA QUANTIZATION (20 ns DISCRETE STEPS)
 * ============================================================================
 * High-resolution histograms (e.g., bin widths < 20 ns) will exhibit a 
 * comb-like structure with discrete 20 ns intervals. 
 * * This is a hardware-level measurement limit, not CPU execution time, bus 
 * latency, or a software bug. The kernel API ktime_get_ns() on the BCM2712 
 * SoC reads the ARM Generic Timer (System Timer). This specific hardware 
 * counter is driven by a fixed 50 MHz hardware clock. 
 * * Period = 1 / 50 MHz = 20 ns.
 * * Therefore, all recorded timestamps and calculated deltas are inherently 
 * quantized to multiples of 20 ns.
 * ============================================================================
 */

#include <TCanvas.h>
#include <TH1D.h>
#include <TStyle.h>
#include <fstream>
#include <string>
#include <vector>
#include <iostream>

void analyze_jitter(const char* filename) {
    // Visual style: 'e'=Entries, 'm'=Mean, 'r'=RMS, 'u'=Underflow, 'o'=Overflow
    gStyle->SetOptStat("emruo");

    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open file " << filename << std::endl;
        return;
    }

    std::vector<double> data;
    std::string line;
    double min_val = 1e18, max_val = 0;
    int count = 0; // Counter to track loaded values

    // Load data (strictly limited to the first 10000 valid entries)
    while (std::getline(file, line) && count < 10000) {
        if (line.empty() || line[0] == '#') continue;
        double val = std::stod(line);
        data.push_back(val);
        if (val < min_val) min_val = val;
        if (val > max_val) max_val = val;
        count++; // Increment counter upon valid data extraction
    }
    file.close();

    if (data.empty()) return;

    // Calculate median to filter out dropped events
    std::vector<double> sorted = data;
    std::sort(sorted.begin(), sorted.end());
    double median = sorted[sorted.size() / 2];

    std::vector<double> nominal_data;
    nominal_data.reserve(data.size());
    for (double d : data) {
        // Accept only deltas between 0.5x and 1.5x the median
        if (d > median * 0.5 && d < median * 1.5) {
            nominal_data.push_back(d);
        }
    }

    // Calculate Mean and Standard Deviation on nominal data
    double sum = 0;
    for (double d : nominal_data) sum += d;
    double mean = sum / nominal_data.size();

    double sq_sum = 0;
    for (double d : nominal_data) sq_sum += (d - mean) * (d - mean);
    double sigma = std::sqrt(sq_sum / nominal_data.size());

    if (sigma == 0) sigma = 1000;

    // Center X-axis: mean +/- N sigma
    double plot_min = mean - (2.0 * sigma);
    double plot_max = mean + (2.0 * sigma);

    // Histogram
    TCanvas *c1 = new TCanvas("c1", "Jitter Analysis", 800, 600);
    TH1D *h1 = new TH1D("h1", Form("Time Deltas Distribution;Delta Time [ns]; #"), 400, plot_min, plot_max);

    for (double d : data) {
        h1->Fill(d);
    }

    h1->SetFillColor(kBlue-7);
    h1->SetLineColor(kBlue+2);
    h1->Draw();

    // Print data
    std::cout << "--- ROOT Jitter Analysis ---" << std::endl;
    std::cout << "File: " << filename << std::endl;
    std::cout << "Mean: " << h1->GetMean() << " ns" << std::endl;
    std::cout << "StdDev: " << h1->GetRMS() << " ns" << std::endl;
    std::cout << "Entries: " << h1->GetEntries() << std::endl;
    
    // Retrieve out-of-bounds data points
    double underflow_count = h1->GetBinContent(0);
    double overflow_count = h1->GetBinContent(h1->GetNbinsX() + 1);

    std::cout << "Underflow (early events): " << underflow_count << std::endl;
    std::cout << "Overflow (late/dropped events): " << overflow_count << std::endl;
    std::cout << "Total Out of Bounds: " << underflow_count + overflow_count << std::endl;

    c1->SaveAs(Form("%s.png", filename));
}