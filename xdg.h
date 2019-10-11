#pragma once

#include "tllist.h"

typedef tll(char *) xdg_data_dirs_t;

xdg_data_dirs_t xdg_data_dirs(void);
void xdg_data_dirs_destroy(xdg_data_dirs_t dirs);

const char *xdg_cache_dir(void);

#if 0
void xdg_find_programs(
    const char *icon_theme, int icon_size, struct application_list *applications);
#endif
