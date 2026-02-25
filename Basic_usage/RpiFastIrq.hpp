// RpiFastIrq.hpp
#pragma once

#include <functional>
#include <string>
#include <thread>
#include <atomic>
#include <cstdint>
#include <cstddef>

// Shared payload structure (must match the Kernel Module exactly)
struct GpioIrqEvent {
    uint64_t timestamp_ns;
    uint32_t event_counter;
    uint32_t pin_state;
};

#define KBUF_SIZE 256

// Mapped memory structure
struct SharedRingBuffer {
    uint32_t head;
    uint32_t tail;
    GpioIrqEvent events[KBUF_SIZE];
};

class RpiFastIrq {
public:
    using IrqCallback = std::function<void(const GpioIrqEvent&)>;

    explicit RpiFastIrq(const std::string& device_path = "/dev/rp1_gpio_irq");
    ~RpiFastIrq();

    // Prevent copying
    RpiFastIrq(const RpiFastIrq&) = delete;
    RpiFastIrq& operator=(const RpiFastIrq&) = delete;

    bool start(IrqCallback user_callback);
    void stop();

private:
    std::string m_device_path;
    int m_fd;
    SharedRingBuffer* m_shared_buf;
    size_t m_mmap_size;
    std::atomic<bool> m_running;
    IrqCallback m_callback;
    std::thread m_listener_thread;

    void listener_thread_func();
};