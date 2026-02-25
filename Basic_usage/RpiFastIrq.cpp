// RpiFastIrq.cpp
#include "RpiFastIrq.hpp"
#include <iostream>
#include <fcntl.h>      // For open() flags
#include <unistd.h>     // For close(), sysconf()
#include <poll.h>       // For poll()
#include <system_error> // For std::system_category
#include <cstring>      // For strerror
#include <sys/mman.h>   // For mmap(), munmap()
#include <sched.h>      // For sched_setscheduler()

RpiFastIrq::RpiFastIrq(const std::string& device_path)
    : m_device_path(device_path), m_fd(-1), m_shared_buf(nullptr), m_mmap_size(0), m_running(false) {
}

RpiFastIrq::~RpiFastIrq() {
    stop(); // Ensure resources are cleaned up when the object is destroyed
}

bool RpiFastIrq::start(IrqCallback user_callback) {
    if (m_running) {
        std::cerr << "[RpiFastIrq] Already running.\n";
        return false;
    }

    // Open the character device file created by the kernel module
    m_fd = ::open(m_device_path.c_str(), O_RDONLY);
    if (m_fd < 0) {
        std::cerr << "[RpiFastIrq] Failed to open device: " << m_device_path 
                  << " Error: " << std::strerror(errno) << "\n";
        return false;
    }

    // Calculate page-aligned size for mmap
    long page_size = ::sysconf(_SC_PAGESIZE);
    m_mmap_size = (sizeof(SharedRingBuffer) + page_size - 1) & ~(page_size - 1);

    // Map kernel buffer into user space (Zero-Copy)
    m_shared_buf = static_cast<SharedRingBuffer*>(::mmap(NULL, m_mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, m_fd, 0));
    if (m_shared_buf == MAP_FAILED) {
        std::cerr << "[RpiFastIrq] mmap failed: " << std::strerror(errno) << "\n";
        ::close(m_fd);
        m_fd = -1;
        return false;
    }

    m_callback = std::move(user_callback);
    m_running = true;

    // Launch the listener in a background thread
    m_listener_thread = std::thread(&RpiFastIrq::listener_thread_func, this);

    return true;
}

void RpiFastIrq::stop() {
    if (!m_running) return;

    // Signal the background thread to terminate
    m_running = false;

    // Wait for the thread to finish execution
    if (m_listener_thread.joinable()) {
        m_listener_thread.join();
    }

    // Unmap shared memory
    if (m_shared_buf != nullptr && m_shared_buf != MAP_FAILED) {
        ::munmap(m_shared_buf, m_mmap_size);
        m_shared_buf = nullptr;
    }

    // Close the file descriptor
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
}

void RpiFastIrq::listener_thread_func() {
    // Elevate thread priority to Real-Time (SCHED_FIFO)
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (sched_setscheduler(0, SCHED_FIFO, &param) == -1) {
        std::cerr << "[RpiFastIrq] Warning: Failed to set SCHED_FIFO priority. Requires root privileges. Error: " 
                  << std::strerror(errno) << "\n";
    }

    struct pollfd pfd;
    pfd.fd = m_fd;
    pfd.events = POLLIN; // We are interested in data ready to be read

    // 100 milliseconds timeout. 
    // This allows poll() to wake up periodically to check the m_running flag 
    // so the thread can exit gracefully when stop() is called.
    const int timeout_ms = 100; 

    uint32_t local_tail = 0;

    while (m_running) {
        // Block until data is available, an error occurs, or timeout expires
        int ret = ::poll(&pfd, 1, timeout_ms);

        if (ret < 0) {
            // Error handling (ignore interrupted system calls)
            if (errno != EINTR) {
                std::cerr << "[RpiFastIrq] poll() error: " << std::strerror(errno) << "\n";
                break;
            }
        } else if (ret > 0) {
            // Data is ready to be read
            if (pfd.revents & POLLIN) {
                // Lock-free acquire barrier: ensures payload reads happen after head updates
                uint32_t current_head = __atomic_load_n(&m_shared_buf->head, __ATOMIC_ACQUIRE);
                
                while (local_tail != current_head) {
                    GpioIrqEvent event_data = m_shared_buf->events[local_tail % KBUF_SIZE];
                    
                    // Execute the user's callback function
                    if (m_callback) {
                        m_callback(event_data);
                    }
                    
                    local_tail++;
                }

                // Lock-free release barrier: updates tail in shared memory so kernel poll can sleep again
                __atomic_store_n(&m_shared_buf->tail, local_tail, __ATOMIC_RELEASE);
            }
        }
        // If ret == 0, it means the timeout expired. The loop will just check 
        // m_running and call poll() again.
    }
}