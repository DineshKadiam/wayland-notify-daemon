#include "config.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <pwd.h>

#include <tllist.h>

#define LOG_MODULE "config"
#define LOG_ENABLE_DBG 0
#include "log.h"

static void
font_set_destroy(struct font_set *set)
{
    font_destroy(set->regular);
    font_destroy(set->bold);
    font_destroy(set->italic);
    font_destroy(set->bold_italic);

    set->regular = set->bold = set->italic = set->bold_italic = NULL;
}

static struct font_set
font_set_clone(const struct font_set *set)
{
    return (struct font_set) {
        .regular = font_clone(set->regular),
        .bold = font_clone(set->bold),
        .italic = font_clone(set->italic),
        .bold_italic = font_clone(set->bold_italic),
    };
}

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

static struct font_set
str_to_font(const char *s, const char *path, int lineno)
{
    const char *names[1] = {s};
    struct font *regular;
    struct font *bold;
    struct font *italic;
    struct font *bold_italic;

    if ((regular = font_from_name(names, 1, "dpi=96")) == NULL)
        LOG_ERR("%s:%u: invalid font: %s", path, lineno, s);

    bold = font_from_name(names, 1, "dpi=96:weight=bold");
    italic = font_from_name(names, 1, "dpi=96:slant=italic");
    bold_italic = font_from_name(names, 1, "dpi=96:weight=bold:slant=italic");

    return (struct font_set){regular, bold, italic, bold_italic};
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
        struct font_set fonts = str_to_font(value, path, lineno);
        if (fonts.regular == NULL)
            return false;

        struct font_set *f =
            strcmp(key, "title-font") == 0 ? &conf->app.fonts :
            strcmp(key, "summary-font") == 0 ? &conf->summary.fonts :
            strcmp(key, "body-font") == 0 ? &conf->body.fonts :
            strcmp(key, "action-font") == 0 ? &conf->action.fonts : NULL;

        assert(f != NULL);
        font_set_destroy(f);
        *f = fonts;
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

    else if (strcmp(key, "margin") == 0) {
        unsigned long m;
        if (!str_to_ulong(value, 10, &m)) {
            LOG_ERR("%s:%u: invalid margin (expected an integer): %s",
                    path, lineno, value);
            return false;
        }

        conf->margin = m;
    }

    else if (strcmp(key, "selection-helper") == 0) {
        free(conf->selection_helper);
        conf->selection_helper = strdup(value);
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
        struct font_set fonts = str_to_font(value, path, lineno);
        if (fonts.regular == NULL)
            return false;

        for (int i = 0; i < 3; i++) {
            struct urgency_config *urgency = &conf->by_urgency[i];
            struct font_set *f =
                strcmp(key, "title-font") == 0 ? &urgency->app.fonts :
                strcmp(key, "summary-font") == 0 ? &urgency->summary.fonts :
                strcmp(key, "body-font") == 0 ? &urgency->body.fonts :
                strcmp(key, "action-font") == 0 ? &urgency->action.fonts : NULL;

            assert(f != NULL);
            font_set_destroy(f);
            *f = font_set_clone(&fonts);
        }
        font_set_destroy(&fonts);
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
        SECTION_LOW,
        SECTION_NORMAL,
        SECTION_CRITICAL,
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

#if defined(LOG_ENABLE_DBG) && LOG_ENABLE_DBG
    static const char *const section_names[] = {
        [SECTION_MAIN] = "main",
        [SECTION_LOW] = "low",
        [SECTION_NORMAL] = "normal",
        [SECTION_CRITICAL] = "critical",
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

            if (strcmp(&line[1], "low") == 0)
                section = SECTION_LOW;
            else if (strcmp(&line[1], "normal") == 0)
                section = SECTION_NORMAL;
            else if (strcmp(&line[1], "critical") == 0)
                section = SECTION_CRITICAL;
            else {
                LOG_ERR("%s:%d: invalid section name: %s", path, lineno, &line[1]);
                goto err;
            }

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
    const char *default_font_names[1] = {"sans serif"};

    struct font_set font_default;
    if ((font_default.regular = font_from_name(default_font_names, 1, "")) == NULL) {
        LOG_ERR("%s: failed to instantiate default font", default_font_names[0]);
        return false;
    }
    font_default.bold = font_from_name(default_font_names, 1, "weight=bold");
    font_default.italic = font_from_name(default_font_names, 1, "slant=italic");
    font_default.bold_italic = font_from_name(default_font_names, 1, "weight=bold:slant=italic");

    *conf = (struct config){
        .output = NULL,
        .max_width = 400,
        .max_height = 200,
        .icon_theme_name = strdup("hicolor"),
        .max_icon_size = 32,
        .margin = 10,
        .by_urgency = {
            /* urgency == low */
            {
                .bg = {0x2b2b, 0x2b2b, 0x2b2b, 0xffff},
                .border = {
                    .color = {0x9090, 0x9090, 0x9090, 0xffff},
                    .size = 1,
                },
                .app = {
                    .fonts = font_set_clone(&font_default),
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                },
                .summary = {
                    .fonts = font_set_clone(&font_default),
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                },
                .body = {
                    .fonts = font_set_clone(&font_default),
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                },
                .action = {
                    .fonts = font_set_clone(&font_default),
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
                    .fonts = font_set_clone(&font_default),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .summary = {
                    .fonts = font_set_clone(&font_default),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .body = {
                    .fonts = font_set_clone(&font_default),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .action = {
                    .fonts = font_set_clone(&font_default),
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
                    .fonts = font_set_clone(&font_default),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .summary = {
                    .fonts = font_set_clone(&font_default),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .body = {
                    .fonts = font_set_clone(&font_default),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .action = {
                    .fonts = font_set_clone(&font_default),
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
            },
        },
        .selection_helper = strdup("dmenu"),
    };

    font_set_destroy(&font_default);

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

    for (size_t i = 0; i < 3; i++) {
        if (conf->by_urgency[i].bg.alpha != 0xffff)
            continue;

        LOG_DBG("enabling subpixel antialiasing for urgency=%s",
                i == 0 ? "low" : i == 1 ? "normal" : "critical");

        font_enable_subpixel_antialias(conf->by_urgency[i].app.fonts.regular);
        font_enable_subpixel_antialias(conf->by_urgency[i].app.fonts.bold);
        font_enable_subpixel_antialias(conf->by_urgency[i].app.fonts.italic);
        font_enable_subpixel_antialias(conf->by_urgency[i].app.fonts.bold_italic);
        font_enable_subpixel_antialias(conf->by_urgency[i].summary.fonts.regular);
        font_enable_subpixel_antialias(conf->by_urgency[i].summary.fonts.bold);
        font_enable_subpixel_antialias(conf->by_urgency[i].summary.fonts.italic);
        font_enable_subpixel_antialias(conf->by_urgency[i].summary.fonts.bold_italic);
        font_enable_subpixel_antialias(conf->by_urgency[i].body.fonts.regular);
        font_enable_subpixel_antialias(conf->by_urgency[i].body.fonts.bold);
        font_enable_subpixel_antialias(conf->by_urgency[i].body.fonts.italic);
        font_enable_subpixel_antialias(conf->by_urgency[i].body.fonts.bold_italic);
        font_enable_subpixel_antialias(conf->by_urgency[i].action.fonts.regular);
        font_enable_subpixel_antialias(conf->by_urgency[i].action.fonts.bold);
        font_enable_subpixel_antialias(conf->by_urgency[i].action.fonts.italic);
        font_enable_subpixel_antialias(conf->by_urgency[i].action.fonts.bold_italic);
    }

out:
    free(path);
    return ret;
}

void
config_destroy(struct config conf)
{
    free(conf.output);
    free(conf.icon_theme_name);
    for (int i  = 0; i < 3; i++) {
        font_destroy(conf.by_urgency[i].app.fonts.regular);
        font_destroy(conf.by_urgency[i].app.fonts.bold);
        font_destroy(conf.by_urgency[i].app.fonts.italic);
        font_destroy(conf.by_urgency[i].app.fonts.bold_italic);

        font_destroy(conf.by_urgency[i].summary.fonts.regular);
        font_destroy(conf.by_urgency[i].summary.fonts.bold);
        font_destroy(conf.by_urgency[i].summary.fonts.italic);
        font_destroy(conf.by_urgency[i].summary.fonts.bold_italic);

        font_destroy(conf.by_urgency[i].body.fonts.regular);
        font_destroy(conf.by_urgency[i].body.fonts.bold);
        font_destroy(conf.by_urgency[i].body.fonts.italic);
        font_destroy(conf.by_urgency[i].body.fonts.bold_italic);

        font_destroy(conf.by_urgency[i].action.fonts.regular);
        font_destroy(conf.by_urgency[i].action.fonts.bold);
        font_destroy(conf.by_urgency[i].action.fonts.italic);
        font_destroy(conf.by_urgency[i].action.fonts.bold_italic);
    }
    free(conf.selection_helper);
}
