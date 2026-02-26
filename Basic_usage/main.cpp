/**
 * @file main.cpp
 * @brief Example application using the RpiFastIrq library with a Lock-Free Ring Buffer.
 */

#include <iostream>
#include <atomic>
#include <csignal>
#include <thread>
#include <chrono>
#include "RpiFastIrq.hpp"

// ============================================================================
// LOCK-FREE RING BUFFER IMPLEMENTATION
// ============================================================================
// This allows the high-priority IRQ thread to pass data to the main printing 
// thread without using mutexes, preventing any I/O blocking or latency.
template <typename T, size_t Size>
class LockFreeRingBuffer {
private:
    T m_data[Size];
    std::atomic<size_t> m_head{0}; // Written by Producer (IRQ Callback)
    std::atomic<size_t> m_tail{0}; // Read by Consumer (Main Loop)

public:
    // Pushes an item into the buffer. Returns false if full.
    bool push(const T& item) {
        size_t current_head = m_head.load(std::memory_order_relaxed);
        
        // Check if buffer is full
        if (current_head - m_tail.load(std::memory_order_acquire) >= Size) {
            return false; // Buffer overflow! Event dropped.
        }
        
        m_data[current_head % Size] = item;
        
        // Release memory order ensures the data write completes before the head increments
        m_head.store(current_head + 1, std::memory_order_release);
        return true;
    }

    // Pops an item from the buffer. Returns false if empty.
    bool pop(T& item) {
        size_t current_tail = m_tail.load(std::memory_order_relaxed);
        
        // Check if buffer is empty
        if (current_tail == m_head.load(std::memory_order_acquire)) {
            return false; 
        }
        
        item = m_data[current_tail % Size];
        
        // Release memory order ensures the data read completes before the tail increments
        m_tail.store(current_tail + 1, std::memory_order_release);
        return true;
    }
};

// ============================================================================
// GLOBAL STATE & SIGNAL HANDLING
// ============================================================================

// Global buffer to hold up to 1024 pending interrupts
LockFreeRingBuffer<GpioIrqEvent, 1024> g_event_buffer;

// Flag to keep the main application running
std::atomic<bool> g_keep_running{true};

// Handle Ctrl+C (SIGINT) to shut down gracefully
void signal_handler([[maybe_unused]] int signum) {
    std::cout << "\n[Main] Shutdown signal received. Exiting safely...\n";
    g_keep_running = false;
}

// ============================================================================
// MAIN APPLICATION
// ============================================================================

int main() {
    // Register the signal handler for graceful exit
    std::signal(SIGINT, signal_handler);

    std::cout << "[Main] Initializing High-Performance IRQ Listener...\n";

    // Instantiate the library
    RpiFastIrq irq_handler("/dev/rp1_gpio_irq");

    // Define the callback function (The Producer)
    // IMPORTANT: This executes in the context of the background poll() thread.
    // It must be extremely fast. No std::cout, no disk writes, no heavy math.
    auto my_irq_callback = [](const GpioIrqEvent& event) {
        // Instantly push to the lock-free buffer. 
        if (!g_event_buffer.push(event)) {
            // If the main thread is too slow and the buffer fills up, 
            // we have no choice but to drop the event to maintain low latency.
            // (In a real scenario, you might want to log this failure later).
        }
    };

    // Start listening for hardware interrupts
    if (!irq_handler.start(my_irq_callback)) {
        std::cerr << "[Main] Failed to start IRQ listener. Is the kernel module loaded?\n";
        return 1;
    }

    std::cout << "[Main] Listening for interrupts on CPU 3. Press Ctrl+C to stop.\n";
    std::cout << "--------------------------------------------------------------\n";
    std::cout << "EVENT #\t\tTIMESTAMP (ns)\n";
    std::cout << "--------------------------------------------------------------\n";

    // The Consumer Loop (Main Thread)
    GpioIrqEvent received_event;
    
    while (g_keep_running) {
        // Try to pop an event from the buffer
        if (g_event_buffer.pop(received_event)) {
            // We got an event! We can print it here safely without blocking the ISR.
            std::cout << received_event.event_counter << "\t\t"
                      << received_event.timestamp_ns << "\n";
        } else {
            // Buffer is empty. Sleep for a short time to avoid 100% CPU usage
            // on the main thread while waiting for interrupts.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Stop the listener thread safely
    irq_handler.stop();
    std::cout << "[Main] Application terminated successfully.\n";

    return 0;
}