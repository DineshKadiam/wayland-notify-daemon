#include "icon.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>

#include <sys/stat.h>
#include <fcntl.h>

#define LOG_MODULE "icon"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "png-fnott.h"
#include "tllist.h"
#include "xdg.h"

typedef tll(char *) theme_names_t;

pixman_image_t *
icon_load(const char *name, int icon_size, icon_theme_list_t themes)
{
    if (name == NULL)
        return NULL;

    if (name[0] == '/') {
        LOG_DBG("loading %s directly", name);
        return png_load(name);
    }

    LOG_DBG("looking for %s (wanted size: %d)", name, icon_size);

    tll_foreach(themes, theme_it) {
        const struct icon_theme *theme = &theme_it->item;
        int min_diff = 10000;

        /* Assume sorted */
        for (size_t i = 0; i < 4; i++) {
            tll_foreach(theme->dirs, it) {
                const int size = it->item.size * it->item.scale;
                const int min_size = it->item.min_size * it->item.scale;
                const int max_size = it->item.max_size * it->item.scale;
                const bool scalable = it->item.scalable;

                const size_t len = strlen(theme->path) + 1 +
                    strlen(it->item.path) + 1 +
                    strlen(name) + strlen(".png") + 1;

                /* Check if a png/svg file exists at all */
                char *full_path = malloc(len);
                sprintf(full_path, "%s/%s/%s.png", theme->path, it->item.path, name);
                if (access(full_path, O_RDONLY) == -1) {
                    /* Also check for svg variant */
                    full_path[len - 4] = 's';
                    full_path[len - 3] = 'v';
                    full_path[len - 2] = 'g';
                    if (access(full_path, O_RDONLY) == -1) {
                        free(full_path);
                        continue;
                    }
                }

                const int diff = scalable ? 0 : abs(size - icon_size);
                if (i == 0 && diff != 0) {
                    /* Looking for *exactly* our wanted size */
                    if (diff < min_diff)
                        min_diff = diff;
                    free(full_path);
                    continue;
                } else if (i == 1 && diff != min_diff) {
                    /* Try the one which matches most closely */
                    free(full_path);
                    continue;
                } else if (i == 2 && (icon_size < min_size ||
                                      icon_size > max_size))
                {
                    /* Find one whose scalable range we're in */
                    free(full_path);
                    continue;
                } else {
                    /* Use anyone available */
                }

                /* TODO: SVG */

                pixman_image_t *pix = png_load(full_path);
                if (pix != NULL) {
                    if (scalable)
                        LOG_DBG("%s: %s: scalable", name, full_path);
                    else if (i == 0)
                        LOG_DBG("%s: %s: exact match", name, full_path);
                    else if (i == 1)
                        LOG_DBG("%s: %s: diff = %d", name, full_path, diff);
                    else if (i == 2)
                        LOG_DBG("%s: %s: range %d-%d",
                                name, full_path, min_size, max_size);
                    else
                        LOG_DBG("%s: %s: nothing else matched", name, full_path);

                    free(full_path);
                    return pix;
                }

                free(full_path);
            }
        }
    }

    xdg_data_dirs_t dirs = xdg_data_dirs();
    tll_foreach(dirs, it) {
        char path[strlen(it->item) + 1 +
                  strlen("pixmaps") + 1 +
                  strlen(name) + strlen(".png") + 1];

        /* TODO: SVG */

        /* No SVG, look for PNG instead */
        sprintf(path, "%s/pixmaps/%s.png", it->item, name);
        pixman_image_t *pix = png_load(path);
        if (pix != NULL) {
            LOG_DBG("%s: %s: pixmap", name, path);
            xdg_data_dirs_destroy(dirs);
            return pix;
        }
    }
    xdg_data_dirs_destroy(dirs);

    LOG_DBG("%s: no matches", name);
    return NULL;
}

static bool
dir_is_usable(const char *path, const char *context, const char *type)
{
    return path != NULL &&
        context != NULL &&
        strcasecmp(context, "applications") == 0;
}

static void
parse_theme(FILE *index, struct icon_theme *theme, theme_names_t *themes_to_load)
{
    char *section = NULL;
    int size = -1;
    int min_size = -1;
    int max_size = -1;
    int scale = 1;
    char *context = NULL;
    char *type = NULL;

    while (true) {
        char *line = NULL;
        size_t sz = 0;
        ssize_t len = getline(&line, &sz, index);

        if (len == -1) {
            free(line);
            break;
        }

        if (len == 0) {
            free(line);
            continue;
        }

        if (line[len - 1] == '\n') {
            line[len - 1] = '\0';
            len--;
        }

        if (len == 0) {
            free(line);
            continue;
        }

        if (line[0] == '[' && line[len - 1] == ']') {

            if (dir_is_usable(section, context, type)) {
                struct icon_dir dir = {
                    .path = section,
                    .size = size,
                    .min_size = min_size >= 0 ? min_size : size,
                    .max_size = max_size >= 0 ? max_size : size,
                    .scale = scale,
                    .scalable = strcasecmp(type, "scalable") == 0,
                };
                tll_push_back(theme->dirs, dir);
            } else
                free(section);

            free(context);
            free(type);

            size = min_size = max_size = -1;
            scale = 1;
            section = NULL;
            context = NULL;
            type = NULL;

            section = malloc(len - 2 + 1);
            memcpy(section, &line[1], len - 2);
            section[len - 2] = '\0';
            free(line);
            continue;
        }

        char *tok_ctx = NULL;

        const char *key = strtok_r(line, "=", &tok_ctx);
        char *value = strtok_r(NULL, "=", &tok_ctx);

        if (strcasecmp(key, "inherits") == 0) {
            char *ctx = NULL;
            for (const char *theme_name = strtok_r(value, ",", &ctx);
                 theme_name != NULL; theme_name = strtok_r(NULL, ",", &ctx))
            {
                tll_push_back(*themes_to_load, strdup(theme_name));
            }
        }

        if (strcasecmp(key, "size") == 0)
            sscanf(value, "%d", &size);

        else if (strcasecmp(key, "minsize") == 0)
            sscanf(value, "%d", &min_size);

        else if (strcasecmp(key, "maxsize") == 0)
            sscanf(value, "%d", &max_size);

        else if (strcasecmp(key, "scale") == 0)
            sscanf(value, "%d", &scale);

        else if (strcasecmp(key, "context") == 0)
            context = strdup(value);

        else if (strcasecmp(key, "type") == 0)
            type = strdup(value);

        free(line);
    }

    if (dir_is_usable(section, context, type)) {
        struct icon_dir dir = {
            .path = section,
            .size = size,
            .min_size = min_size >= 0 ? min_size : size,
            .max_size = max_size >= 0 ? max_size : size,
            .scale = scale
        };
        tll_push_back(theme->dirs, dir);
    } else
        free(section);

    free(context);
    free(type);
}

static bool
load_theme_in(const char *dir, struct icon_theme *theme,
              theme_names_t *themes_to_load)
{
    int theme_dir_fd = -1;
    int index_fd = -1;
    FILE *index = NULL;
    bool ret = false;

    theme_dir_fd = open(dir, O_RDONLY);
    if (theme_dir_fd == -1) {
        //LOG_WARN("%s: failed to open", dir);
        goto out;
    }

    index_fd = openat(theme_dir_fd, "index.theme", O_RDONLY);
    if (index_fd == -1) {
        LOG_DBG("%s/index.theme: failed to open", dir);
        goto out;
    }

    index = fdopen(index_fd, "r");
    assert(index != NULL);

    theme->path = strdup(dir);
    parse_theme(index, theme, themes_to_load);

    ret = true;

 out:
    if (index != NULL)
        fclose(index);
    if (index_fd != -1)
        close(index_fd);
    if (theme_dir_fd != -1)
        close(theme_dir_fd);
    return ret;
}

icon_theme_list_t
icon_load_theme(const char *name)
{
    /* List of themes; first item is the primary theme, subsequent
     * items are inherited items (i.e. fallback themes) */
    icon_theme_list_t themes = tll_init();

    /* List of themes to try to load. This list will be appended to as
     * we go, and find 'Inherits' values in the theme index files. */
    theme_names_t themes_to_load = tll_init();
    tll_push_back(themes_to_load, strdup(name));

    xdg_data_dirs_t dirs = xdg_data_dirs();

    while (tll_length(themes_to_load) > 0) {
        char *theme_name = tll_pop_front(themes_to_load);

        /*
         * Check if we've already loaded this theme. Example:
         * "Arc" inherits "Moka,Faba,elementary,Adwaita,ghome,hicolor
         * "Moka" inherits "Faba"
         * "Faba" inherits "elementary,gnome,hicolor"
         */
        bool theme_already_loaded = false;
        tll_foreach(themes, it) {
            if (strcasecmp(it->item.name, theme_name) == 0) {
                theme_already_loaded = true;
                break;
            }
        }

        if (theme_already_loaded) {
            free(theme_name);
            continue;
        }

        tll_foreach(dirs, dir_it) {
            char path[strlen(dir_it->item) + 1 +
                      strlen("icons") + 1 +
                      strlen(theme_name) + 1];
            sprintf(path, "%s/icons/%s", dir_it->item, theme_name);

            struct icon_theme theme = {0};
            if (load_theme_in(path, &theme, &themes_to_load)) {
                theme.name = strdup(theme_name);
                tll_push_back(themes, theme);
                break;
            }
        }

        free(theme_name);
    }

    xdg_data_dirs_destroy(dirs);
    return themes;
}

static void
theme_destroy(struct icon_theme theme)
{
    free(theme.name);
    free(theme.path);

    tll_foreach(theme.dirs, it) {
        free(it->item.path);
        tll_remove(theme.dirs, it);
    }
}

void
icon_themes_destroy(icon_theme_list_t themes)
{
    tll_foreach(themes, it) {
        theme_destroy(it->item);
        tll_remove(themes, it);
    }
}
