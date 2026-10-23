#pragma once

#include <stdbool.h>

void *recoil_clap_editor_create(void *instance, void *core);
void recoil_clap_editor_destroy(void *editor);
bool recoil_clap_editor_attach(void *editor, void *native_parent, int width, int height);
void recoil_clap_editor_detach(void *editor);
void recoil_clap_editor_tick(void *editor);
