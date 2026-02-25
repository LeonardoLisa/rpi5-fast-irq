/**
 * @file rpi_fast_irq.c
 * @brief High-Performance GPIO Interrupt Handler for Raspberry Pi 5
 * * ============================================================================
 * INSTALLATION & CONFIGURATION GUIDE
 * ============================================================================
 * * 1. ISOLATE THE CPU (Host OS Configuration):
 * Open /boot/firmware/cmdline.txt and append the following to the end 
 * of the line (do not create a new line):
 * isolcpus=3
 * Reboot the Raspberry Pi: sudo reboot
 * * 2. FIND THE CORRECT GPIO BASE FOR RP1 (Raspberry Pi 5 Specific):
 * On the RPi 5, the GPIOs are handled by the RP1 southbridge. The standard
 * GPIO 17 might not be logically mapped to "17" in the kernel.
 * Run: cat /sys/class/gpio/gpiochip* /label
 * Find the chip labeled "pinctrl-rp1". Let's say its base is 512.
 * Your logical GPIO will be: Base (512) + Pin (17) = 529.
 * Update the GPIO_PIN macro below if necessary.
 * * 3. COMPILE THE MODULE:
 * Run: make
 * * 4. INSTALL THE MODULE:
 * sudo insmod rpi_fast_irq.ko
 * * 5. VERIFY INSTALLATION:
 * dmesg | tail -n 20
 * ls -l /dev/rp1_gpio_irq
 * * 6. UNINSTALL:
 * sudo rmmod rpi_fast_irq
 * ============================================================================
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/ktime.h>
#include <linux/cpumask.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>

#define DEVICE_NAME "rp1_gpio_irq"
#define CLASS_NAME  "rp1_irq_class"

// --- CONFIGURATION ---
// Note: Change this to (BASE + 17) if your RP1 gpiochip starts at a different base offset.
#define GPIO_PIN 588 
#define TARGET_CPU 3

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Custom RPi5 LKM");
MODULE_DESCRIPTION("High-Performance GPIO IRQ Handler with CPU Affinity");
MODULE_VERSION("1.0");

// --- SHARED DATA STRUCTURE ---
// Must match the C++ User Space structure exactly
struct GpioIrqEvent {
    u64 timestamp_ns;
    u32 event_counter;
    u32 pin_state;
};

#define KBUF_SIZE 256

struct SharedRingBuffer {
    u32 head;
    u32 tail;
    struct GpioIrqEvent events[KBUF_SIZE];
};

// --- GLOBAL VARIABLES ---
static int major_num;
static struct class* irq_class = NULL;
static struct device* irq_device = NULL;
static struct cdev irq_cdev;

static unsigned int irq_number;
static u32 total_interrupts = 0;

static struct SharedRingBuffer *shared_buf = NULL;
static DECLARE_WAIT_QUEUE_HEAD(wq);

// --- INTERRUPT SERVICE ROUTINE (ISR) ---
static irqreturn_t gpio_isr(int irq, void *dev_id) {
    u64 ts = ktime_get_ns();
    int state = gpio_get_value(GPIO_PIN);
    u32 current_head;

    total_interrupts++;

    // Lock-free read of the current head
    current_head = shared_buf->head;
    
    // Write payload
    shared_buf->events[current_head % KBUF_SIZE].timestamp_ns = ts;
    shared_buf->events[current_head % KBUF_SIZE].event_counter = total_interrupts;
    shared_buf->events[current_head % KBUF_SIZE].pin_state = state;
    
    // Memory barrier: ensure payload is written to memory before head is updated
    smp_store_release(&shared_buf->head, current_head + 1);

    // Wake up the user space thread sleeping on poll()
    wake_up_interruptible(&wq);

    return IRQ_HANDLED;
}

// --- FILE OPERATIONS ---

static int dev_open(struct inode *inodep, struct file *filep) {
    pr_info("[%s] Device opened by user space.\n", DEVICE_NAME);
    return 0;
}

static int dev_release(struct inode *inodep, struct file *filep) {
    pr_info("[%s] Device closed by user space.\n", DEVICE_NAME);
    return 0;
}

static int dev_mmap(struct file *filep, struct vm_area_struct *vma) {
    unsigned long size = vma->vm_end - vma->vm_start;
    unsigned long expected_size = PAGE_ALIGN(sizeof(struct SharedRingBuffer));

    if (size > expected_size) {
        pr_err("[%s] mmap size %lu exceeds allocated size %lu\n", DEVICE_NAME, size, expected_size);
        return -EINVAL;
    }

    // Map the vmalloc area to user space
    if (remap_vmalloc_range(vma, shared_buf, 0)) {
        pr_err("[%s] mmap remap_vmalloc_range failed\n", DEVICE_NAME);
        return -EAGAIN;
    }

    return 0;
}

static __poll_t dev_poll(struct file *filep, poll_table *wait) {
    __poll_t mask = 0;
    
    poll_wait(filep, &wq, wait);
    
    // Data is ready to read if head != tail using lock-free read
    if (smp_load_acquire(&shared_buf->head) != smp_load_acquire(&shared_buf->tail)) {
        mask |= POLLIN | POLLRDNORM; 
    }
    
    return mask;
}

static struct file_operations fops = {
    .open = dev_open,
    .mmap = dev_mmap,
    .poll = dev_poll,
    .release = dev_release,
    .owner = THIS_MODULE
};

// --- MODULE INIT & EXIT ---

static int __init rpi_fast_irq_init(void) {
    int result;
    dev_t dev_num;
    struct cpumask affinity_mask;
    unsigned long buffer_size = PAGE_ALIGN(sizeof(struct SharedRingBuffer));

    pr_info("[%s] Initializing kernel module...\n", DEVICE_NAME);

    // Allocate memory accessible via mmap to user space
    shared_buf = vmalloc_user(buffer_size);
    if (!shared_buf) {
        pr_err("[%s] Failed to allocate shared buffer\n", DEVICE_NAME);
        return -ENOMEM;
    }
    shared_buf->head = 0;
    shared_buf->tail = 0;

    // 1. Setup Character Device
    result = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    major_num = MAJOR(dev_num);
    if (result < 0) {
        pr_err("[%s] Failed to allocate major number\n", DEVICE_NAME);
        goto r_vmalloc;
    }

    cdev_init(&irq_cdev, &fops);
    irq_cdev.owner = THIS_MODULE;
    cdev_add(&irq_cdev, dev_num, 1);

    irq_class = class_create(CLASS_NAME);
    irq_device = device_create(irq_class, NULL, dev_num, NULL, DEVICE_NAME);
    pr_info("[%s] Created device node /dev/%s\n", DEVICE_NAME, DEVICE_NAME);

    // 2. Setup GPIO
    if (!gpio_is_valid(GPIO_PIN)) {
        pr_err("[%s] Invalid GPIO pin: %d\n", DEVICE_NAME, GPIO_PIN);
        goto r_device;
    }

    gpio_request(GPIO_PIN, "sysfs");
    gpio_direction_input(GPIO_PIN);
    
    irq_number = gpio_to_irq(GPIO_PIN);
    pr_info("[%s] GPIO %d mapped to IRQ: %d\n", DEVICE_NAME, GPIO_PIN, irq_number);

    // 3. Request IRQ (Rising Edge)
    result = request_irq(irq_number,
                         (irq_handler_t) gpio_isr,
                         IRQF_TRIGGER_RISING,
                         "rpi_fast_gpio_handler",
                         NULL);

    if (result) {
        pr_err("[%s] Failed to request IRQ %d\n", DEVICE_NAME, irq_number);
        goto r_gpio;
    }

    // 4. Enforce CPU Affinity (Lock IRQ to CPU 3)
    cpumask_clear(&affinity_mask);
    cpumask_set_cpu(TARGET_CPU, &affinity_mask);
    result = irq_set_affinity_hint(irq_number, &affinity_mask);
    
    if (result) {
        pr_warn("[%s] Failed to set IRQ affinity to CPU %d. Error: %d\n", DEVICE_NAME, TARGET_CPU, result);
    } else {
        pr_info("[%s] Successfully pinned IRQ %d to CPU %d\n", DEVICE_NAME, irq_number, TARGET_CPU);
    }

    return 0;

r_gpio:
    gpio_free(GPIO_PIN);
r_device:
    device_destroy(irq_class, dev_num);
    class_destroy(irq_class);
    cdev_del(&irq_cdev);
    unregister_chrdev_region(dev_num, 1);
r_vmalloc:
    vfree(shared_buf);
    return -1;
}

static void __exit rpi_fast_irq_exit(void) {
    dev_t dev_num = MKDEV(major_num, 0);

    // Clear affinity and free IRQ
    irq_set_affinity_hint(irq_number, NULL);
    free_irq(irq_number, NULL);
    
    // Free GPIO
    gpio_free(GPIO_PIN);

    // Destroy device
    device_destroy(irq_class, dev_num);
    class_destroy(irq_class);
    cdev_del(&irq_cdev);
    unregister_chrdev_region(dev_num, 1);

    // Free shared memory buffer
    if (shared_buf) {
        vfree(shared_buf);
    }

    pr_info("[%s] Module successfully unloaded.\n", DEVICE_NAME);
}

module_init(rpi_fast_irq_init);
module_exit(rpi_fast_irq_exit);