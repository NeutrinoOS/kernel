#include "../descriptor.hpp"

#include "../../drivers/log/logging.hpp"

namespace descriptor {

bool register_console_descriptor();
bool register_serial_descriptor();
bool register_keyboard_descriptor();
bool register_mouse_descriptor();
bool register_pipe_descriptor();
bool register_framebuffer_descriptor();
bool register_graphical_session_descriptor();
bool register_drm_descriptor();
bool register_render_descriptor();
bool register_block_device_descriptor();
bool register_shared_memory_descriptor();
bool register_vty_descriptor();
bool register_cpu_stats_descriptor();
bool register_cpu_info_descriptor();
bool register_task_stats_descriptor();
bool register_kernel_log_descriptor();
bool register_net_device_descriptor();
bool register_net_endpoint_descriptor();
bool register_pci_descriptor();
bool register_audio_output_descriptor();
bool register_sensor_descriptor();
bool register_service_registry_descriptor();

void register_builtin_types() {
    reset_block_device_registry();

    if (!register_console_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register console descriptor type");
    }
    if (!register_serial_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register serial descriptor type");
    }
    if (!register_keyboard_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register keyboard descriptor type");
    }
    if (!register_mouse_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register mouse descriptor type");
    }
    if (!register_pipe_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register pipe descriptor type");
    }
    if (!register_framebuffer_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register framebuffer descriptor type");
    }
    if (!register_graphical_session_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register graphical session descriptor type");
    }
    if (!register_drm_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register DRM descriptor type");
    }
    if (!register_render_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register render descriptor type");
    }
    if (!register_block_device_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register block device descriptor type");
    }
    if (!register_shared_memory_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register shared memory descriptor type");
    }
    if (!register_vty_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register vty descriptor type");
    }
    if (!register_cpu_stats_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register cpu stats descriptor type");
    }
    if (!register_cpu_info_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register cpu info descriptor type");
    }
    if (!register_task_stats_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register task stats descriptor type");
    }
    if (!register_kernel_log_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register kernel log descriptor type");
    }
    if (!register_net_device_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register net device descriptor type");
    }
    if (!register_net_endpoint_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register net endpoint descriptor type");
    }
    if (!register_pci_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register PCI descriptor type");
    }
    if (!register_audio_output_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register audio output descriptor type");
    }
    if (!register_sensor_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register sensor descriptor type");
    }
    if (!register_service_registry_descriptor()) {
        log_message(LogLevel::Warn,
                    "Descriptor: failed to register service registry type");
    }
}

}  // namespace descriptor
