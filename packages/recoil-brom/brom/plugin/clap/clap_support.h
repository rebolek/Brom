#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <clap/clap.h>
#include <clap/ext/audio-ports.h>
#include <clap/ext/gui.h>
#include <clap/ext/note-ports.h>
#include <clap/ext/timer-support.h>
#include <clap/ext/preset-load.h>
#include <clap/factory/preset-discovery.h>
#include "../../dsp/audio.h"

typedef struct recoil_clap_gui_controller recoil_clap_gui_controller;

void recoil_clap_copy_audio(const clap_process_t *proc,
                            const double *src,
                            uint32_t frames,
                            uint32_t offset,
                            double gain,
                            bool bypass);

int32_t recoil_clap_read_effect_f32(const clap_process_t *proc,
                                    uint32_t port_index,
                                    uint32_t offset,
                                    uint32_t frames,
                                    uint32_t channels,
                                    bool main_port,
                                    brom_audio_f32 *block);
int32_t recoil_clap_read_effect_f64(const clap_process_t *proc,
                                    uint32_t port_index,
                                    uint32_t offset,
                                    uint32_t frames,
                                    uint32_t channels,
                                    bool main_port,
                                    brom_audio_f64 *block);
bool recoil_clap_write_effect_f32(const clap_process_t *proc,
                                  uint32_t offset,
                                  uint32_t frames,
                                  const brom_audio_f32 *block);
bool recoil_clap_write_effect_f64(const clap_process_t *proc,
                                  uint32_t offset,
                                  uint32_t frames,
                                  const brom_audio_f64 *block);
void recoil_clap_clear_effect_f32(brom_audio_f32 *block, uint32_t frames);
void recoil_clap_clear_effect_f64(brom_audio_f64 *block, uint32_t frames);

bool recoil_clap_stream_write_all(clap_ostream_t *stream,
                                  const void *src,
                                  uint64_t size);

bool recoil_clap_stream_read_all(clap_istream_t *stream,
                                 void *dst,
                                 uint64_t size);

const char *recoil_clap_gui_platform_api(void);
bool recoil_clap_gui_is_api_supported(const char *api, bool is_floating);
recoil_clap_gui_controller *recoil_clap_gui_create(const clap_host_t *host,
                                                   void *instance,
                                                   void *core,
                                                   uint32_t width,
                                                   uint32_t height);
void recoil_clap_gui_destroy(recoil_clap_gui_controller *controller);
bool recoil_clap_gui_set_parent(recoil_clap_gui_controller *controller,
                                const clap_window_t *window);
bool recoil_clap_gui_show(recoil_clap_gui_controller *controller);
bool recoil_clap_gui_hide(recoil_clap_gui_controller *controller);
void recoil_clap_gui_on_timer(recoil_clap_gui_controller *controller,
                              clap_id timer_id);
void recoil_clap_gui_on_main_thread(recoil_clap_gui_controller *controller);
void recoil_clap_preset_discovery_init(const char *id,
                                       const char *name,
                                       const char *vendor,
                                       uint32_t preset_count);
void *recoil_clap_preset_discovery_factory(void);
void *recoil_clap_preset_load_extension(void);
