# Raspberry Pi 5 High-Performance GPIO Interrupt Handler

## 📖 Project Overview
This project provides a complete, near-real-time Linux interrupt handling system specifically designed for the Raspberry Pi 5 (BCM2712 / RP1 architecture). 

Standard Linux user-space GPIO libraries suffer from high latency and non-deterministic jitter due to the Completely Fair Scheduler (CFS). This project solves that by using a custom **Linux Kernel Module (LKM)** to catch the hardware interrupt in Ring 0, paired with CPU core isolation (`isolcpus`) and IRQ affinity. The kernel then instantly wakes up a **C++ User Space Application** via a `poll()` wait queue, safely passing the data to the main thread using a Lock-Free Ring Buffer.

---

## 🏗️ Architecture Architecture
1. **Hardware:** A signal hits the Raspberry Pi 5 GPIO header (routed through the RP1 southbridge via PCIe).
2. **Kernel Space (LKM):** The ISR (Interrupt Service Routine) fires on an isolated CPU core (CPU 3). It records a high-precision nanosecond timestamp and wakes up the user space.
3. **User Space Interface:** A C++ background thread sleeps on a `/dev/rp1_gpio_irq` character device using `poll()`, consuming 0% CPU until the exact microsecond the interrupt arrives.
4. **Data Handling:** The C++ callback pushes the event data into a Single-Producer Single-Consumer (SPSC) Lock-Free Ring Buffer, allowing the main application to read and print the data without blocking the interrupt listener.

---

## ⚙️ System Requirements & Preparation

To achieve true low-latency and low-jitter performance, you must configure the Raspberry Pi OS to isolate a CPU core and prevent the system from moving interrupts around.

### 1. Isolate CPU Core 3
We need to tell the Linux scheduler to keep standard processes off CPU 3.
1. Edit the boot configuration file:
   ```bash
   sudo nano /boot/firmware/cmdline.txt