#include "config.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>
#include <fcntl.h>

#include <tllist.h>

#define LOG_MODULE "config"
#define LOG_ENABLE_DBG 0
#include "log.h"
#include "tokenize.h"

struct config_file {
    char *path;
    int fd;
};

struct path_component {
    const char *component;
    int fd;
};
typedef tll(struct path_component) path_components_t;

static void
path_component_add(path_components_t *components, const char *comp, int fd)
{
    assert(comp != NULL);
    assert(fd >= 0);

    struct path_component pc = {.component = comp, .fd = fd};
    tll_push_back(*components, pc);
}

static void
path_component_destroy(struct path_component *component)
{
    assert(component->fd >= 0);
    close(component->fd);
}

static void
path_components_destroy(path_components_t *components)
{
    tll_foreach(*components, it) {
        path_component_destroy(&it->item);
        tll_remove(*components, it);
    }
}

static struct config_file
path_components_to_config_file(const path_components_t *components)
{
    if (tll_length(*components) == 0)
        goto err;

    size_t len = 0;
    tll_foreach(*components, it)
        len += strlen(it->item.component) + 1;

    char *path = malloc(len);
    if (path == NULL)
        goto err;

    size_t idx = 0;
    tll_foreach(*components, it) {
        strcpy(&path[idx], it->item.component);
        idx += strlen(it->item.component);
        path[idx++] = '/';
    }
    path[idx - 1] = '\0';  /* Strip last ’/’ */

    int fd_copy = dup(tll_back(*components).fd);
    if (fd_copy < 0) {
        free(path);
        goto err;
    }

    return (struct config_file){.path = path, .fd = fd_copy};

err:
    return (struct config_file){.path = NULL, .fd = -1};
}

static const char *
get_user_home_dir(void)
{
    const struct passwd *passwd = getpwuid(getuid());
    if (passwd == NULL)
        return NULL;
    return passwd->pw_dir;
}

static bool
try_open_file(path_components_t *components, const char *name)
{
    int parent_fd = tll_back(*components).fd;

    struct stat st;
    if (fstatat(parent_fd, name, &st, 0) == 0 && S_ISREG(st.st_mode)) {
        int fd = openat(parent_fd, name, O_RDONLY);
        if (fd >= 0) {
            path_component_add(components, name, fd);
            return true;
        }
    }

    return false;
}

static struct config_file
open_config(void)
{
    struct config_file ret = {.path = NULL, .fd = -1};


    path_components_t components = tll_init();

    const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    const char *user_home_dir = get_user_home_dir();
    char *xdg_config_dirs_copy = NULL;

    /* Use XDG_CONFIG_HOME, or ~/.config */
    if (xdg_config_home != NULL) {
        int fd = open(xdg_config_home, O_RDONLY);
        if (fd >= 0)
            path_component_add(&components, xdg_config_home, fd);
    } else if (user_home_dir != NULL) {
        int home_fd = open(user_home_dir, O_RDONLY);
        if (home_fd >= 0) {
            int config_fd = openat(home_fd, ".config", O_RDONLY);
            if (config_fd >= 0) {
                path_component_add(&components, user_home_dir, home_fd);
                path_component_add(&components, ".config", config_fd);
            } else
                close(home_fd);
        }
    }

    /* First look for fnott/fnott.ini */
    if (tll_length(components) > 0) {
        int fnott_fd = openat(tll_back(components).fd, "fnott", O_RDONLY);
        if (fnott_fd >= 0) {
            path_component_add(&components, "fnott", fnott_fd);

            if (try_open_file(&components, "fnott.ini"))
                goto done;

            struct path_component pc = tll_pop_back(components);
            path_component_destroy(&pc);
        }
    }

    /* Deprecated: look for fnottrc */
    if (tll_length(components) > 0) {
        if (try_open_file(&components, "fnottrc"))
            goto done;
    }

    /* Finally, try fnott/fnott.ini in all XDG_CONFIG_DIRS */
    const char *xdg_config_dirs = getenv("XDG_CONFIG_DIRS");
    xdg_config_dirs_copy = xdg_config_dirs != NULL
        ? strdup(xdg_config_dirs) : NULL;

    if (xdg_config_dirs_copy != NULL) {
        for (char *save = NULL,
                 *xdg_dir = strtok_r(xdg_config_dirs_copy, ":", &save);
             xdg_dir != NULL;
             xdg_dir = strtok_r(NULL, ":", &save))
        {
            path_components_destroy(&components);

            int xdg_fd = open(xdg_dir, O_RDONLY);
            if (xdg_fd < 0)
                continue;

            int fnott_fd = openat(xdg_fd, "fnott", O_RDONLY);
            if (fnott_fd < 0) {
                close(xdg_fd);
                continue;
            }

            assert(tll_length(components) == 0);
            path_component_add(&components, xdg_dir, xdg_fd);
            path_component_add(&components, "fnott", fnott_fd);

            if (try_open_file(&components, "fnott.ini"))
                goto done;
        }
    }

out:
    path_components_destroy(&components);
    free(xdg_config_dirs_copy);
    return ret;

done:
    assert(tll_length(components) > 0);
    ret = path_components_to_config_file(&components);
    goto out;
}

static bool
str_to_ulong(const char *s, int base, unsigned long *res)
{
    if (s == NULL)
        return false;

    errno = 0;
    char *end = NULL;

    *res = strtoul(s, &end, base);
    return errno == 0 && *end == '\0';
}

static inline pixman_color_t
color_hex_to_pixman_with_alpha(uint32_t color, uint16_t alpha)
{
    return (pixman_color_t){
        .red =   ((color >> 16 & 0xff) | (color >> 8 & 0xff00)) * alpha / 0xffff,
        .green = ((color >>  8 & 0xff) | (color >> 0 & 0xff00)) * alpha / 0xffff,
        .blue =  ((color >>  0 & 0xff) | (color << 8 & 0xff00)) * alpha / 0xffff,
        .alpha = alpha,
    };
}

static bool
str_to_color(const char *s, pixman_color_t *color, const char *path, int lineno)
{
    unsigned long value;
    if (!str_to_ulong(s, 16, &value)) {
        LOG_ERRNO("%s:%d: invalid color: %s", path, lineno, s);
        return false;
    }

    uint32_t rgb = value & 0xffffff;
    uint16_t alpha = value >> 24; alpha |= alpha << 8;

    *color = color_hex_to_pixman_with_alpha(rgb, alpha == 0 ? 0xffff : alpha);
    return true;
}

static bool
str_to_spawn_template(struct config *conf,
                      const char *s, struct config_spawn_template *template,
                      const char *path, int lineno)
{
    free(template->raw_cmd);
    free(template->argv);

    template->raw_cmd = NULL;
    template->argv = NULL;

    if (strlen(s) == 0)
        return true;

    char *raw_cmd = strdup(s);
    char **argv = NULL;

    if (!tokenize_cmdline(raw_cmd, &argv)) {
        LOG_ERR("%s:%d: syntax error in command line", path, lineno);
        return false;
    }

    template->raw_cmd = raw_cmd;
    template->argv = argv;
    return true;
}

static bool
parse_section_urgency(const char *key, const char *value,
                      struct urgency_config *conf,
                      const char *path, unsigned lineno)
{
    if (strcmp(key, "background") == 0) {
        pixman_color_t bg;
        if (!str_to_color(value, &bg, path, lineno))
            return false;

        conf->bg = bg;
    }

    else if (strcmp(key, "border-color") == 0) {
        pixman_color_t color;
        if (!str_to_color(value, &color, path, lineno))
            return false;

        conf->border.color = color;
    }

    else if (strcmp(key, "border-size") == 0) {
        unsigned long sz;
        if (!str_to_ulong(value, 10, &sz)) {
            LOG_ERR("%s:%u: invalid border-size (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        conf->border.size = sz;
    }

    else if (strcmp(key, "title-font") == 0 ||
             strcmp(key, "summary-font") == 0 ||
             strcmp(key, "body-font") == 0 ||
             strcmp(key, "action-font") == 0)
    {
        char **font =
            strcmp(key, "title-font") == 0 ? &conf->app.font :
            strcmp(key, "summary-font") == 0 ? &conf->summary.font :
            strcmp(key, "body-font") == 0 ? &conf->body.font :
            strcmp(key, "action-font") == 0 ? &conf->action.font : NULL;

        assert(font != NULL);
        free(*font);
        *font = strdup(value);
    }

    else if (strcmp(key, "title-color") == 0 ||
             strcmp(key, "summary-color") == 0 ||
             strcmp(key, "body-color") == 0 ||
             strcmp(key, "action-color") == 0)
    {
        pixman_color_t color;
        if (!str_to_color(value, &color, path, lineno))
            return false;

        pixman_color_t *c =
            strcmp(key, "title-color") == 0 ? &conf->app.color :
            strcmp(key, "summary-color") == 0 ? &conf->summary.color :
            strcmp(key, "body-color") == 0 ? &conf->body.color :
            strcmp(key, "action-color") == 0 ? &conf->action.color : NULL;

        assert(c != NULL);
        *c = color;
    }

    else if (strcmp(key, "max-timeout") == 0) {
        unsigned long max_timeout_secs;
        if (!str_to_ulong(value, 10, &max_timeout_secs)) {
            LOG_ERR("%s:%d: invalid max-timeout (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        conf->max_timeout_secs = max_timeout_secs;
    }

    else if (strcmp(key, "default-timeout") == 0) {
        unsigned long default_timeout_secs;
        if (!str_to_ulong(value, 10, &default_timeout_secs)) {
            LOG_ERR("%s:%d: invalid default-timeout (expected an integer): %s", path,
                    lineno, value);
            return false;
        }
        conf->default_timeout_secs = default_timeout_secs;
    }

    else if (strcmp(key, "sound-file") == 0) {
        free(conf->sound_file);
        conf->sound_file = strlen(value) > 0 ? strdup(value) : NULL;
    }

    else {
        LOG_ERR("%s:%u: invalid key: %s", path, lineno, key);
        return false;
    }

    return true;
}

static bool
parse_section_low(const char *key, const char *value, struct config *conf,
                  const char *path, unsigned lineno)
{
    return parse_section_urgency(
        key, value, &conf->by_urgency[0], path, lineno);
}

static bool
parse_section_normal(const char *key, const char *value, struct config *conf,
                     const char *path, unsigned lineno)
{
    return parse_section_urgency(
        key, value, &conf->by_urgency[1], path, lineno);
}

static bool
parse_section_critical(const char *key, const char *value, struct config *conf,
                       const char *path, unsigned lineno)
{
    return parse_section_urgency(
        key, value, &conf->by_urgency[2], path, lineno);
}

static bool
parse_section_main(const char *key, const char *value, struct config *conf,
                   const char *path, unsigned lineno)
{
    if (strcmp(key, "output") == 0) {
        free(conf->output);
        conf->output = strdup(value);
    }

    else if (strcmp(key, "max-width") == 0) {
        unsigned long w;
        if (!str_to_ulong(value, 10, &w)) {
            LOG_ERR("%s:%u: invalid max-width (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        conf->max_width = w;
    }

    else if (strcmp(key, "min-width") == 0) {
        unsigned long w;
        if (!str_to_ulong(value, 10, &w)) {
            LOG_ERR("%s:%u: invalid min-width (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        conf->min_width = w;
    }

    else if (strcmp(key, "max-height") == 0) {
        unsigned long h;
        if (!str_to_ulong(value, 10, &h)) {
            LOG_ERR("%s:%u: invalid max-height (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        conf->max_height = h;
    }

    else if (strcmp(key, "icon-theme") == 0) {
        free(conf->icon_theme_name);
        conf->icon_theme_name = strdup(value);
    }

    else if (strcmp(key, "max-icon-size") == 0) {
        unsigned long sz;
        if (!str_to_ulong(value, 10, &sz)) {
            LOG_ERR("%s:%u: invalid max-height (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        conf->max_icon_size = sz;
    }

    else if (strcmp(key, "stacking-order") == 0) {
        if (strcasecmp(value, "bottom-up") == 0)
            conf->stacking_order = STACK_BOTTOM_UP;
        else if (strcasecmp(value, "top-down") == 0)
            conf->stacking_order = STACK_TOP_DOWN;
        else {
            LOG_ERR("%s:%u: %s: invalid stacking-order value, must be one of "
                    "\"bottom-up\", "
                    "\"top-down\"",
                    path, lineno, value);
            return false;
        }
    }

    else if (strcmp(key, "anchor") == 0) {
        if (strcasecmp(value, "top-left") == 0)
            conf->anchor = ANCHOR_TOP_LEFT;
        else if (strcasecmp(value, "top-right") == 0)
            conf->anchor = ANCHOR_TOP_RIGHT;
        else if (strcasecmp(value, "bottom-left") == 0)
            conf->anchor = ANCHOR_BOTTOM_LEFT;
        else if (strcasecmp(value, "bottom-right") == 0)
            conf->anchor = ANCHOR_BOTTOM_RIGHT;
        else {
            LOG_ERR(
                "%s:%u: %s: invalid anchor value, must be one of "
                "\"top-left\", "
                "\"top-right\", "
                "\"bottom-left\" or "
                "\"bottom-right\"",
                path, lineno, value);
            return false;
        }
    }

    else if (strcmp(key, "edge-margin-vertical") == 0) {
        unsigned long m;
        if (!str_to_ulong(value, 10, &m)) {
            LOG_ERR(
                "%s:%u: invalid edge-margin-vertical (expected an integer): %s",
                path, lineno, value);
            return false;
        }

        conf->margins.vertical = m;
    }

    else if (strcmp(key, "edge-margin-horizontal") == 0) {
        unsigned long m;
        if (!str_to_ulong(value, 10, &m)) {
            LOG_ERR(
                "%s:%u: invalid edge-margin-horizontal (expected an integer): %s",
                path, lineno, value);
            return false;
        }

        conf->margins.horizontal = m;
    }

    else if (strcmp(key, "notification-margin") == 0) {
        unsigned long m;
        if (!str_to_ulong(value, 10, &m)) {
            LOG_ERR(
                "%s:%u: invalid nofication-margin (expected an integer): %s",
                path, lineno, value);
            return false;
        }

        conf->margins.between = m;
    }

    else if (strcmp(key, "selection-helper") == 0) {
        free(conf->selection_helper);
        conf->selection_helper = strdup(value);
    }

    else if (strcmp(key, "play-sound") == 0) {
        if (!str_to_spawn_template(conf, value, &conf->play_sound, path, lineno))
            return false;
    }

    else if (strcmp(key, "background") == 0) {
        pixman_color_t bg;
        if (!str_to_color(value, &bg, path, lineno))
            return false;

        for (int i = 0; i < 3; i++)
            conf->by_urgency[i].bg = bg;
    }

    else if (strcmp(key, "border-color") == 0) {
        pixman_color_t color;
        if (!str_to_color(value, &color, path, lineno))
            return false;

        for (int i = 0; i < 3; i++)
            conf->by_urgency[i].border.color = color;
    }

    else if (strcmp(key, "border-size") == 0) {
        unsigned long sz;
        if (!str_to_ulong(value, 10, &sz)) {
            LOG_ERR("%s:%u: invalid border-size (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        for (int i = 0; i < 3; i++)
            conf->by_urgency[i].border.size = sz;
    }

    else if (strcmp(key, "title-font") == 0 ||
             strcmp(key, "summary-font") == 0 ||
             strcmp(key, "body-font") == 0 ||
             strcmp(key, "action-font") == 0)
    {
        for (int i = 0; i < 3; i++) {
            struct urgency_config *urgency = &conf->by_urgency[i];
            char **font =
                strcmp(key, "title-font") == 0 ? &urgency->app.font :
                strcmp(key, "summary-font") == 0 ? &urgency->summary.font :
                strcmp(key, "body-font") == 0 ? &urgency->body.font :
                strcmp(key, "action-font") == 0 ? &urgency->action.font : NULL;

            assert(font != NULL);
            free(*font);
            *font = strdup(value);
        }
    }

    else if (strcmp(key, "title-color") == 0 ||
             strcmp(key, "summary-color") == 0 ||
             strcmp(key, "body-color") == 0 ||
             strcmp(key, "action-color") == 0)
    {
        pixman_color_t color;
        if (!str_to_color(value, &color, path, lineno))
            return false;

        for (int i = 0; i < 3; i++) {
            struct urgency_config *urgency = &conf->by_urgency[i];
            pixman_color_t *c =
                strcmp(key, "title-color") == 0 ? &urgency->app.color :
                strcmp(key, "summary-color") == 0 ? &urgency->summary.color :
                strcmp(key, "body-color") == 0 ? &urgency->body.color :
                strcmp(key, "action-color") == 0 ? &urgency->action.color : NULL;

            assert(c != NULL);
            *c = color;
        }
    }

    else if (strcmp(key, "max-timeout") == 0) {
        unsigned long max_timeout_secs;
        if (!str_to_ulong(value, 10, &max_timeout_secs)) {
            LOG_ERR("%s:%d: invalid max-timeout (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        for (int i = 0; i < 3; i++) {
            struct urgency_config *urgency = &conf->by_urgency[i];
            urgency->max_timeout_secs = max_timeout_secs;
        }
    }

    else if (strcmp(key, "default-timeout") == 0) {
        unsigned long default_timeout_secs;
        if (!str_to_ulong(value, 10, &default_timeout_secs)) {
             LOG_ERR("%s:%d: invalid default-timeout (expected an integer): %s", path,
                     lineno, value);
             return false;
        }

        for (int i = 0; i < 3; i++) {
            struct urgency_config *urgency = &conf->by_urgency[i];
            urgency->default_timeout_secs = default_timeout_secs;
        }
    }

    else if (strcmp(key, "sound-file") == 0) {
        for (int i = 0; i < 3; i++) {
            struct urgency_config *urgency = &conf->by_urgency[i];
            free(urgency->sound_file);
            urgency->sound_file = strlen(value) > 0 ? strdup(value) : NULL;
        }
    }

    else {
        LOG_ERR("%s:%u: invalid key: %s", path, lineno, key);
        return false;
    }

    return true;
}

static bool
parse_config_file(FILE *f, struct config *conf, const char *path)
{
    enum section {
        SECTION_MAIN,
        SECTION_LOW,
        SECTION_NORMAL,
        SECTION_CRITICAL,
        SECTION_COUNT,
    } section = SECTION_MAIN;

    /* Function pointer, called for each key/value line */
    typedef bool (*parser_fun_t)(
        const char *key, const char *value, struct config *conf,
        const char *path, unsigned lineno);

    /* Maps sections to line parser functions */
    static const parser_fun_t section_parser_map[] = {
        [SECTION_MAIN] = &parse_section_main,
        [SECTION_LOW] = &parse_section_low,
        [SECTION_NORMAL] = &parse_section_normal,
        [SECTION_CRITICAL] = &parse_section_critical,
    };

    static const char *const section_names[] = {
        [SECTION_MAIN] = "main",
        [SECTION_LOW] = "low",
        [SECTION_NORMAL] = "normal",
        [SECTION_CRITICAL] = "critical",
    };

    unsigned lineno = 0;
    char *_line = NULL;
    size_t count = 0;

    while (true) {
        errno = 0;
        lineno++;

        ssize_t ret = getline(&_line, &count, f);

        if (ret < 0) {
            if (errno != 0) {
                LOG_ERRNO("failed to read from configuration");
                goto err;
            }
            break;
        }

        /* Strip whitespace */
        char *line = _line;
        {
            while (isspace(*line))
                line++;
            if (line[0] != '\0') {
                char *end = line + strlen(line) - 1;
                while (isspace(*end))
                    end--;
                *(end + 1) = '\0';
            }
        }

        /* Empty line, or comment */
        if (line[0] == '\0' || line[0] == '#')
            continue;

        /* Split up into key/value pair + trailing comment */
        char *key_value = strtok(line, "#");
        char *comment __attribute__((unused)) = strtok(NULL, "\n");

        /* Check for new section */
        if (key_value[0] == '[') {
            char *end = strchr(key_value, ']');
            if (end == NULL) {
                LOG_ERR("%s:%d: syntax error: %s", path, lineno, key_value);
                goto err;
            }

            *end = '\0';

            for (section = SECTION_MAIN; section < SECTION_COUNT; ++section) {
                if (strcmp(&key_value[1], section_names[section]) == 0) {
                    break;
                };
            }

            if (section == SECTION_COUNT) {
                LOG_ERR("%s:%d: invalid section name: %s", path, lineno, &key_value[1]);
                goto err;
            }

            continue;
        }

        char *key = strtok(key_value, "=");
        if (key == NULL) {
            LOG_ERR("%s:%d: syntax error: no key specified", path, lineno);
            goto err;
        }

        char *value = strtok(NULL, "\n");
        if (value == NULL) {
            /* Empty value, i.e. "key=" */
            value = key + strlen(key);
        }

        /* Strip trailing whitespace from key (leading stripped earlier) */
        {
            assert(!isspace(*key));

            char *end = key + strlen(key) - 1;
            while (isspace(*end))
                end--;
            *(end + 1) = '\0';
        }

        /* Strip leading whitespace from value (trailing stripped earlier) */
        {
            while (isspace(*value))
                value++;

            if (value[0] != '\0') {
                char *end = value + strlen(value) - 1;
                while (isspace(*end))
                    end--;
                *(end + 1) = '\0';
            }
        }

        LOG_DBG("section=%s, key='%s', value='%s'",
                section_names[section], key, value);

        parser_fun_t section_parser = section_parser_map[section];
        assert(section_parser != NULL);

        if (!section_parser(key, value, conf, path, lineno))
            goto err;
    }

    free(_line);
    return true;

err:
    free(_line);
    return false;
}

bool
config_load(struct config *conf, const char *path)
{
    const char *const default_font_name = "sans serif";

    *conf = (struct config){
        .output = NULL,
        .min_width = 0,
        .max_width = 400,
        .max_height = 200,
        .icon_theme_name = strdup("hicolor"),
        .max_icon_size = 32,
        .stacking_order = STACK_BOTTOM_UP,
        .anchor = ANCHOR_TOP_RIGHT,
        .margins = {
            .vertical = 10,
            .horizontal = 10,
            .between = 10,
        },
        .by_urgency = {
            /* urgency == low */
            {
                .bg = {0x2b2b, 0x2b2b, 0x2b2b, 0xffff},
                .border = {
                    .color = {0x9090, 0x9090, 0x9090, 0xffff},
                    .size = 1,
                },
                .app = {
                    .font = strdup(default_font_name),
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                },
                .summary = {
                    .font = strdup(default_font_name),
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                },
                .body = {
                    .font = strdup(default_font_name),
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                },
                .action = {
                    .font = strdup(default_font_name),
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                },
                .max_timeout_secs = 0,
                .default_timeout_secs = 0,
            },
            {
                .bg = {0x3f3f, 0x5f5f, 0x3f3f, 0xffff},
                .border = {
                    .color = {0x9090, 0x9090, 0x9090, 0xffff},
                    .size = 1,
                },
                .app = {
                    .font = strdup(default_font_name),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .summary = {
                    .font = strdup(default_font_name),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .body = {
                    .font = strdup(default_font_name),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .action = {
                    .font = strdup(default_font_name),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .max_timeout_secs = 0,
                .default_timeout_secs = 0,
            },
            {
                .bg = {0x6c6c, 0x3333, 0x3333, 0xffff},
                .border = {
                    .color = {0x9090, 0x9090, 0x9090, 0xffff},
                    .size = 1,
                },
                .app = {
                    .font = strdup(default_font_name),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .summary = {
                    .font = strdup(default_font_name),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .body = {
                    .font = strdup(default_font_name),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .action = {
                    .font = strdup(default_font_name),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .max_timeout_secs = 0,
                .default_timeout_secs = 0,
            },
        },
        .selection_helper = strdup("dmenu"),
    };

    conf->play_sound.raw_cmd = strdup("aplay ${filename}");
    tokenize_cmdline(conf->play_sound.raw_cmd, &conf->play_sound.argv);

    bool ret = false;

    struct config_file conf_file = {.path = NULL, .fd = -1};
    if (path != NULL) {
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            LOG_ERRNO("%s: failed to open", path);
            goto out;
        }

        conf_file.path = strdup(path);
        conf_file.fd = fd;
    } else {
        conf_file = open_config();
        if (conf_file.fd < 0) {
            /* Default conf */
            LOG_WARN("no configuration found, using defaults");
            ret = true;
            goto out;
        }
    }

    assert(conf_file.path != NULL);
    assert(conf_file.fd >= 0);

    LOG_INFO("loading configuration from %s", conf_file.path);

    FILE *f = fdopen(conf_file.fd, "r");
    if (f == NULL) {
        LOG_ERR("%s: failed to open", conf_file.path);
        goto out;
    }

    ret = parse_config_file(f, conf, conf_file.path);
    fclose(f);

out:
    free(conf_file.path);
    return ret;
}

static void
free_spawn_template(struct config_spawn_template *template)
{
    free(template->raw_cmd);
    free(template->argv);
}

void
config_destroy(struct config conf)
{
    free(conf.output);
    free(conf.icon_theme_name);
    free(conf.selection_helper);
    free_spawn_template(&conf.play_sound);

    for (int i = 0; i < 3; i++) {
        struct urgency_config *uconf = &conf.by_urgency[i];
        free(uconf->app.font);
        free(uconf->summary.font);
        free(uconf->body.font);
        free(uconf->action.font);
        free(uconf->sound_file);
    }
}
