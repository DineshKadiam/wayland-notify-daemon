#include "config.h"

#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <pwd.h>

#define LOG_MODULE "config"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "tllist.h"

static char *
get_config_path_user_config(void)
{
    struct passwd *passwd = getpwuid(getuid());
    if (passwd == NULL) {
        LOG_ERRNO("failed to lookup user");
        return NULL;
    }

    const char *home_dir = passwd->pw_dir;
    LOG_DBG("user's home directory: %s", home_dir);

    int len = snprintf(NULL, 0, "%s/.config/fnottrc", home_dir);
    char *path = malloc(len + 1);
    snprintf(path, len + 1, "%s/.config/fnottrc", home_dir);
    return path;
}

static char *
get_config_path_xdg(void)
{
    const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    if (xdg_config_home == NULL)
        return NULL;

    int len = snprintf(NULL, 0, "%s/fnottrc", xdg_config_home);
    char *path = malloc(len + 1);
    snprintf(path, len + 1, "%s/fnottrc", xdg_config_home);
    return path;
}

static char *
get_config_path(void)
{
    struct stat st;

    char *config = get_config_path_xdg();
    if (config != NULL && stat(config, &st) == 0 && S_ISREG(st.st_mode))
        return config;
    free(config);

    /* 'Default' XDG_CONFIG_HOME */
    config = get_config_path_user_config();
    if (config != NULL && stat(config, &st) == 0 && S_ISREG(st.st_mode))
        return config;
    free(config);

    return NULL;
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
    int alpha_div = 0xffff / alpha;
    return (pixman_color_t){
        .red =   ((color >> 16 & 0xff) | (color >> 8 & 0xff00)) / alpha_div,
        .green = ((color >>  8 & 0xff) | (color >> 0 & 0xff00)) / alpha_div,
        .blue =  ((color >>  0 & 0xff) | (color << 8 & 0xff00)) / alpha_div,
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

static struct font *
str_to_font(const char *s, const char *path, int lineno)
{
    font_list_t font_names = tll_init();
    tll_push_back(font_names, s);

    struct font *font;
    if ((font = font_from_name(font_names, "")) == NULL)
        LOG_ERR("%s:%u: invalid font: %s", path, lineno, s);

    tll_free(font_names);
    return font;
}

static bool
parse_section_main(const char *key, const char *value, struct config *conf,
                   const char *path, unsigned lineno)
{
    if (strcmp(key, "max-width") == 0) {
        unsigned long w;
        if (!str_to_ulong(value, 10, &w)) {
            LOG_ERR("%s:%u: invalid max-width (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        conf->max_width = w;
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
        struct font *font;
        if ((font = str_to_font(value, path, lineno)) == NULL)
            return false;

        for (int i = 0; i < 3; i++) {
            struct urgency_config *urgency = &conf->by_urgency[i];
            struct font **f =
                strcmp(key, "title-font") == 0 ? &urgency->app.font :
                strcmp(key, "summary-font") == 0 ? &urgency->summary.font :
                strcmp(key, "body-font") == 0 ? &urgency->body.font :
                strcmp(key, "action-font") == 0 ? &urgency->action.font : NULL;

            assert(f != NULL);
            font_destroy(*f);
            *f = font_clone(font);
        }
        font_destroy(font);
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
    } section = SECTION_MAIN;

    /* Function pointer, called for each key/value line */
    typedef bool (*parser_fun_t)(
        const char *key, const char *value, struct config *conf,
        const char *path, unsigned lineno);

    /* Maps sections to line parser functions */
    static const parser_fun_t section_parser_map[] = {
        [SECTION_MAIN] = &parse_section_main,
    };

#if defined(_DEBUG) && defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
    static const char *const section_names[] = {
        [SECTION_MAIN] = "main",
    };
#endif

    unsigned lineno = 0;
    char *_line = NULL;

    while (true) {
        errno = 0;
        lineno++;

        _line = NULL;
        size_t count = 0;
        ssize_t ret = getline(&_line, &count, f);

        if (ret < 0) {
            free(_line);
            if (errno != 0) {
                LOG_ERRNO("failed to read from configuration");
                return false;
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
        if (line[0] == '\0' || line[0] == '#') {
            free(_line);
            continue;
        }

        /* Check for new section */
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (end == NULL) {
                LOG_ERR("%s:%d: syntax error: %s", path, lineno, line);
                goto err;
            }

            *end = '\0';

#if 0
            if (strcmp(&line[1], "colors") == 0)
                section = SECTION_COLORS;
            else if (strcmp(&line[1], "cursor") == 0)
                section = SECTION_CURSOR;
            else {
                LOG_ERR("%s:%d: invalid section name: %s", path, lineno, &line[1]);
                goto err;
            }
#endif
            free(_line);
            continue;
        }

        char *key = strtok(line, "=");
        char *value = strtok(NULL, "\n");

        /* Strip trailing whitespace from key (leading stripped earlier) */
        {
            assert(!isspace(*key));

            char *end = key + strlen(key) - 1;
            while (isspace(*end))
                end--;
            *(end + 1) = '\0';
        }

        if (value == NULL) {
            if (key != NULL && strlen(key) > 0 && key[0] != '#') {
                LOG_ERR("%s:%d: syntax error: %s", path, lineno, line);
                goto err;
            }

            free(_line);
            continue;
        }

        /* Strip leading whitespace from value (trailing stripped earlier) */
        {
            while (isspace(*value))
                value++;
            assert(!isspace(*(value + strlen(value) - 1)));
        }

        if (key[0] == '#') {
            free(_line);
            continue;
        }

        LOG_DBG("section=%s, key='%s', value='%s'",
                section_names[section], key, value);

        parser_fun_t section_parser = section_parser_map[section];
        assert(section_parser != NULL);

        if (!section_parser(key, value, conf, path, lineno))
            goto err;

        free(_line);
    }

    return true;

err:
    free(_line);
    return false;
}

bool
config_load(struct config *conf)
{
    /* Default configuration */
    font_list_t default_font_names = tll_init();
    tll_push_back(default_font_names, "sans serif");

    struct font *font_default;
    if ((font_default = font_from_name(default_font_names, "")) == NULL) {
        LOG_ERR("%s: failed to instantiate default font", tll_front(default_font_names));
        tll_free(default_font_names);
        return false;
    }
    tll_free(default_font_names);

    *conf = (struct config){
        .max_width = 400,
        .max_height = 200,
        .icon_theme_name = strdup("hicolor"),
        .max_icon_size = 32,
        .margin = {
            .top = 25,
            .right = 25,
            .bottom = 25,
            .left = 25,
        },
        .padding = {
            .top = 20,
            .right = 20,
            .bottom = 20,
            .left = 20,
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
                    .font = font_clone(font_default),
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                },
                .summary = {
                    .font = font_clone(font_default),
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                },
                .body = {
                    .font = font_clone(font_default),
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                },
                .action = {
                    .font = font_clone(font_default),
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                },
            },
            {
                .bg = {0x3f3f, 0x5f5f, 0x3f3f, 0xffff},
                .border = {
                    .color = {0x9090, 0x9090, 0x9090, 0xffff},
                    .size = 1,
                },
                .app = {
                    .font = font_clone(font_default),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .summary = {
                    .font = font_clone(font_default),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .body = {
                    .font = font_clone(font_default),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .action = {
                    .font = font_clone(font_default),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
            },
            {
                .bg = {0x6c6c, 0x3333, 0x3333, 0xffff},
                .border = {
                    .color = {0x9090, 0x9090, 0x9090, 0xffff},
                    .size = 1,
                },
                .app = {
                    .font = font_clone(font_default),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .summary = {
                    .font = font_clone(font_default),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .body = {
                    .font = font_clone(font_default),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .action = {
                    .font = font_clone(font_default),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
            },
        },
    };

    font_destroy(font_default);

    bool ret = false;

    char *path = get_config_path();
    if (path == NULL) {
        /* Default conf */
        LOG_WARN("no configuration found, using defaults");
        ret = true;
        goto out;
    }

    LOG_DBG("loading configuration from %s", path);

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        LOG_ERR("%s: failed to open", path);
        goto out;
    }

    ret = parse_config_file(f, conf, path);
    fclose(f);

out:
    free(path);
    return ret;
}

void
config_destroy(struct config conf)
{
    free(conf.icon_theme_name);
    for (int i  = 0; i < 3; i++) {
        font_destroy(conf.by_urgency[i].app.font);
        font_destroy(conf.by_urgency[i].summary.font);
        font_destroy(conf.by_urgency[i].body.font);
        font_destroy(conf.by_urgency[i].action.font);
    }
}
