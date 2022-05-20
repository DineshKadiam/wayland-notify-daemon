#include "icon.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <unistd.h>
#include <limits.h>

#include <sys/stat.h>
#include <fcntl.h>

#include <tllist.h>

#include <nanosvg.h>
#include <nanosvgrast.h>

#define LOG_MODULE "icon"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "png-fnott.h"
#include "svg.h"
#include "xdg.h"

enum icon_type { ICON_NONE, ICON_PNG, ICON_SVG };

typedef tll(char *) theme_names_t;

pixman_image_t *
icon_load(const char *name, int icon_size, const icon_theme_list_t *themes)
{
    pixman_image_t *pix = NULL;

    if (name[0] == '/') {
        if ((pix = svg_load(name, icon_size)) != NULL) {
            LOG_DBG("%s: absolute path SVG", name);
            return pix;
        }
        if ((pix = png_load(name)) != NULL) {
            LOG_DBG("%s: abslute path PNG", name);
            return pix;
        }
    }

    const size_t file_name_len = strlen(name) + 4;
    char file_name[file_name_len + 1];
    strcpy(file_name, name);
    strcat(file_name, ".xxx");

    struct {
        int diff;
        const struct xdg_data_dir *xdg_dir;
        const struct icon_theme *theme;
        const struct icon_dir *icon_dir;
        enum icon_type type;
    } min_diff = {.diff = INT_MAX};

    /* For details, see
     * https://specifications.freedesktop.org/icon-theme-spec/icon-theme-spec-latest.html#icon_lookup */

    xdg_data_dirs_t xdg_dirs = xdg_data_dirs();

    tll_foreach(*themes, theme_it) {
        const struct icon_theme *theme = &theme_it->item;

        /* Fallback icon to use if there aren’t any exact matches */
        /* Assume sorted */
        tll_foreach(theme->dirs, icon_dir_it) {
            const struct icon_dir *icon_dir = &icon_dir_it->item;

            char theme_relative_path[
                5 + 1 + /* “icons” */
                strlen(theme->name) + 1 +
                strlen(icon_dir->path) + 1];

            sprintf(theme_relative_path, "icons/%s/%s",
                    theme->name, icon_dir->path);

            tll_foreach(xdg_dirs, xdg_dir_it) {
                const struct xdg_data_dir *xdg_dir = &xdg_dir_it->item;

                const int scale = icon_dir->scale;
                const int size = icon_dir->size * scale;
                const int min_size = icon_dir->min_size * scale;
                const int max_size = icon_dir->max_size * scale;
                const int threshold = icon_dir->threshold * scale;
                const enum icon_dir_type type = icon_dir->type;

                bool is_exact_match = false;;
                int diff = INT_MAX;

                /* See if this directory is usable for the requested icon size */
                switch (type) {
                case ICON_DIR_FIXED:
                    is_exact_match = size == icon_size;
                    diff = abs(size - icon_size);
                    break;

                case ICON_DIR_THRESHOLD:
                    is_exact_match =
                        (size - threshold) <= icon_size &&
                        (size + threshold) >= icon_size;
                    diff = icon_size < (size - threshold)
                        ? (size - threshold) - icon_size
                        : icon_size - (size + threshold);
                    break;

                case ICON_DIR_SCALABLE:
                    is_exact_match =
                        min_size <= icon_size &&
                        max_size >= icon_size;
                    diff = icon_size < min_size
                        ? min_size - icon_size
                        : icon_size - max_size;
                    break;
                }

                int dir_fd = openat(
                    xdg_dir->fd, theme_relative_path, O_RDONLY | O_DIRECTORY);
                if (dir_fd < 0)
                    continue;

                if (!is_exact_match && min_diff.diff <= diff) {
                    close(dir_fd);
                    continue;
                }

                const size_t len = file_name_len;
                char *path = file_name;
                path[len - 4] = '.';
                path[len - 3] = 'p';
                path[len - 2] = 'n';
                path[len - 1] = 'g';

                if (faccessat(dir_fd, path, R_OK, 0) < 0) {
                    path[len - 3] = 's';
                    path[len - 2] = 'v';
                    path[len - 1] = 'g';
                    if (faccessat(dir_fd, path, R_OK, 0) < 0) {
                        close(dir_fd);
                        continue;
                    }
                }

                if (!is_exact_match) {
                    assert(diff < min_diff.diff);
                    min_diff.diff = diff;
                    min_diff.xdg_dir = xdg_dir;
                    min_diff.theme = theme;
                    min_diff.icon_dir = icon_dir;
                    min_diff.type = path[len - 3] == 's'
                        ? ICON_SVG : ICON_PNG;
                    close(dir_fd);
                    continue;
                }

                size_t path_len =
                    strlen(xdg_dir->path) + 1 +
                    5 + 1 + /* “icons” */
                    strlen(theme->name) + 1 +
                    strlen(icon_dir->path) + 1 +
                    len;
                char full_path[path_len + 1];

                sprintf(full_path, "%s/icons/%s/%s/%s",
                        xdg_dir->path, theme->name, icon_dir->path, path);

                if ((path[len - 3] == 's' &&
                     (pix = svg_load(full_path, icon_size)) != NULL) ||
                    (path[len - 3] == 'p' &&
                     (pix = png_load(full_path)) != NULL))
                {
                    LOG_DBG("%s: %s", icon.name, full_path);
                    close(dir_fd);
                    goto done;
                }

                close(dir_fd);
            }
        }

        /* Try loading fallbacks for those icons we didn’t find an
         * exact match */
        if (min_diff.type == ICON_NONE) {
            assert(min_diff.xdg_dir == NULL);
            continue;
        }

        size_t path_len =
            strlen(min_diff.xdg_dir->path) + 1 +
            5 + 1 + /* “icons” */
            strlen(min_diff.theme->name) + 1 +
            strlen(min_diff.icon_dir->path) + 1 +
            strlen(name) + 4;

        char full_path[path_len + 1];
        sprintf(full_path, "%s/icons/%s/%s/%s.%s",
                min_diff.xdg_dir->path,
                min_diff.theme->name,
                min_diff.icon_dir->path,
                name,
                min_diff.type == ICON_SVG ? "svg" : "png");

        if ((min_diff.type == ICON_SVG &&
             (pix = svg_load(full_path, icon_size)) != NULL) ||
            (min_diff.type == ICON_PNG &&
             (pix = png_load(full_path)) != NULL))
        {
            LOG_DBG("%s: %s (fallback)", icon.name, full_path);
            goto done;
        } else {
            /* Reset diff data, before checking the parent theme(s) */
            min_diff.diff = INT_MAX;
            min_diff.xdg_dir = NULL;
            min_diff.theme = NULL;
            min_diff.icon_dir = NULL;
            min_diff.type = ICON_NONE;
        }
    }

    /* Finally, look in XDG_DATA_DIRS/pixmaps */
    tll_foreach(xdg_dirs, it) {
        const size_t len =
            strlen(it->item.path) + 1 +
            strlen("pixmaps") + 1 +
            strlen(name) + strlen(".svg");
        char path[len + 1];

        /* Try SVG variant first */
        sprintf(path, "%s/pixmaps/%s.svg", it->item.path, name);
        if ((pix = svg_load(path, icon_size)) != NULL) {
            LOG_DBG("%s: %s (pixmaps)", icon.name, path);
            goto done;
        }

        /* No SVG, look for PNG instead */
        path[len - 3] = 'p';
        path[len - 2] = 'n';
        path[len - 1] = 'g';
        if ((pix = png_load(path)) != NULL) {
            LOG_DBG("%s: %s (pixmaps)", icon.name, path);
            goto done;
        }
    }

done:
    xdg_data_dirs_destroy(xdg_dirs);
    return pix;
}

static void
parse_theme(FILE *index, struct icon_theme *theme, theme_names_t *themes_to_load)
{
    char *section = NULL;
    int size = -1;
    int min_size = -1;
    int max_size = -1;
    int scale = 1;
    int threshold = 2;
    char *context = NULL;
    enum icon_dir_type type = ICON_DIR_THRESHOLD;

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

            tll_foreach(theme->dirs, it) {
                struct icon_dir *d = &it->item;

                if (section == NULL || strcmp(d->path, section) != 0)
                    continue;

                d->size = size;
                d->min_size = min_size >= 0 ? min_size : size;
                d->max_size = max_size >= 0 ? max_size : size;
                d->scale = scale;
                d->threshold = threshold;
                d->type = type;
            }

            free(section);
            free(context);

            size = min_size = max_size = -1;
            scale = 1;
            section = NULL;
            context = NULL;
            type = ICON_DIR_THRESHOLD;
            threshold = 2;

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

        if (strcasecmp(key, "directories") == 0) {
            char *save = NULL;
            for (const char *d = strtok_r(value, ",", &save);
                 d != NULL;
                 d = strtok_r(NULL, ",", &save))
            {
                struct icon_dir dir = {.path = strdup(d)};
                tll_push_back(theme->dirs, dir);
            }
        }

        else if (strcasecmp(key, "size") == 0)
            sscanf(value, "%d", &size);

        else if (strcasecmp(key, "minsize") == 0)
            sscanf(value, "%d", &min_size);

        else if (strcasecmp(key, "maxsize") == 0)
            sscanf(value, "%d", &max_size);

        else if (strcasecmp(key, "scale") == 0)
            sscanf(value, "%d", &scale);

        else if (strcasecmp(key, "context") == 0)
            context = strdup(value);

        else if (strcasecmp(key, "threshold") == 0)
            sscanf(value, "%d", &threshold);

        else if (strcasecmp(key, "type") == 0) {
            if (strcasecmp(value, "fixed") == 0)
                type = ICON_DIR_FIXED;
            else if (strcasecmp(value, "scalable") == 0)
                type = ICON_DIR_SCALABLE;
            else if (strcasecmp(value, "threshold") == 0)
                type = ICON_DIR_THRESHOLD;
            else {
                LOG_WARN(
                    "ignoring unrecognized icon theme directory type: %s",
                    value);
            }
        }

        free(line);
    }

    tll_foreach(theme->dirs, it) {
        struct icon_dir *d = &it->item;

        if (section == NULL || strcmp(d->path, section) != 0)
            continue;

        d->size = size;
        d->min_size = min_size >= 0 ? min_size : size;
        d->max_size = max_size >= 0 ? max_size : size;
        d->scale = scale;
        d->threshold = threshold;
        d->type = type;
    }

    tll_foreach(theme->dirs, it) {
        if (it->item.size == 0) {
            free(it->item.path);
            tll_remove(theme->dirs, it);
        }
    }

    free(section);
    free(context);
}

static bool
load_theme_in(const char *dir, struct icon_theme *theme,
              theme_names_t *themes_to_load)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/index.theme", dir);

    FILE *index = fopen(path, "r");
    if (index == NULL)
        return false;

    parse_theme(index, theme, themes_to_load);
    fclose(index);
    return true;
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
            char path[strlen(dir_it->item.path) + 1 +
                      strlen("icons") + 1 +
                      strlen(theme_name) + 1];
            sprintf(path, "%s/icons/%s", dir_it->item.path, theme_name);

            struct icon_theme theme = {0};
            if (load_theme_in(path, &theme, &themes_to_load)) {
                theme.name = strdup(theme_name);
                tll_push_back(themes, theme);
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
