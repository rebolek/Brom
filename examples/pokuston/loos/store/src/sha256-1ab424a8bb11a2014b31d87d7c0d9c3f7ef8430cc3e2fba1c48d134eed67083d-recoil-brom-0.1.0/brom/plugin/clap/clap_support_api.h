#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

typedef struct clap_process clap_process_t;
typedef struct clap_input_events clap_input_events_t;
typedef struct clap_output_events clap_output_events_t;
typedef struct clap_param_info clap_param_info_t;
typedef struct clap_audio_port_info clap_audio_port_info_t;
typedef struct clap_note_port_info clap_note_port_info_t;
typedef struct clap_ostream clap_ostream_t;
typedef struct clap_istream clap_istream_t;
typedef struct clap_host clap_host_t;
typedef struct clap_window clap_window_t;

typedef uint32_t clap_id;
typedef struct recoil_clap_gui_controller recoil_clap_gui_controller;
typedef struct recoil_clap_preset_mailbox recoil_clap_preset_mailbox;
typedef struct recoil_clap_preset_snapshot recoil_clap_preset_snapshot;

void recoil_clap_copy_audio(const clap_process_t *proc,
                            const double *src,
                            uint32_t frames,
                            uint32_t offset,
                            double gain,
                            bool bypass);

void recoil_clap_copy_audio_stereo(const clap_process_t *proc,
                                   const double *src_l,
                                   const double *src_r,
                                   uint32_t frames,
                                   uint32_t offset,
                                   double gain,
                                   bool bypass);

bool recoil_clap_stream_write_all(clap_ostream_t *stream,
                                  const void *src,
                                  uint64_t size);

bool recoil_clap_stream_read_all(clap_istream_t *stream,
                                 void *dst,
                                 uint64_t size);
int64_t recoil_clap_read_file(const char *path, void *dst, uint64_t capacity);
int64_t recoil_clap_stream_read_to_end(clap_istream_t *stream,
                                       void *dst,
                                       uint64_t capacity);

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

/* Tell a controller's editor which factory preset is now committed, or -1 for
   a patch that is not one.  Main thread only. */
void recoil_clap_gui_preset_changed(recoil_clap_gui_controller *controller,
                                    int32_t index);

void recoil_clap_preset_discovery_init(const char *id,
                                       const char *name,
                                       const char *vendor,
                                       uint32_t preset_count);
bool recoil_clap_preset_discovery_add(uint32_t index,
                                      const char *name,
                                      const char *key,
                                      const char *description);
bool recoil_clap_preset_discovery_add_user(uint32_t index,
                                           const char *name,
                                           const char *key,
                                           const char *description,
                                           const char *file_path);
bool recoil_clap_preset_discovery_add_feature(uint32_t index,
                                              const char *feature);
void *recoil_clap_preset_discovery_factory(void);
void *recoil_clap_preset_load_extension(void);

recoil_clap_preset_mailbox *recoil_clap_preset_mailbox_create(void);
void recoil_clap_preset_mailbox_destroy(recoil_clap_preset_mailbox *mailbox);
recoil_clap_preset_snapshot *recoil_clap_preset_snapshot_create(uint32_t count,
                                                                int32_t index);
void *recoil_clap_preset_snapshot_values(recoil_clap_preset_snapshot *snapshot);
void recoil_clap_preset_mailbox_publish(recoil_clap_preset_mailbox *mailbox,
                                        recoil_clap_preset_snapshot *snapshot);
void recoil_clap_preset_mailbox_cancel(recoil_clap_preset_mailbox *mailbox);
recoil_clap_preset_snapshot *recoil_clap_preset_mailbox_take(
    recoil_clap_preset_mailbox *mailbox);
void recoil_clap_preset_mailbox_commit(recoil_clap_preset_mailbox *mailbox,
                                       recoil_clap_preset_snapshot *snapshot);
void recoil_clap_preset_mailbox_reject(recoil_clap_preset_mailbox *mailbox,
                                       recoil_clap_preset_snapshot *snapshot);
int32_t recoil_clap_preset_mailbox_current(
    const recoil_clap_preset_mailbox *mailbox);
void recoil_clap_preset_mailbox_clear(recoil_clap_preset_mailbox *mailbox);
bool recoil_clap_preset_mailbox_active_matches(
    const recoil_clap_preset_mailbox *mailbox,
    uint32_t param_id,
    double value);
uint32_t recoil_clap_preset_mailbox_take_completions(
    recoil_clap_preset_mailbox *mailbox);
void recoil_clap_preset_mailbox_reclaim(recoil_clap_preset_mailbox *mailbox);
