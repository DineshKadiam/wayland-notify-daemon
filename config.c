#include "config.h"

#define LOG_MODULE "config"
#define LOG_ENABLE_DBG 1
#include "log.h"
#include "tllist.h"

static struct font font_default;

bool
config_load(struct config *conf)
{
    /* Default configuration */
    font_list_t default_font_names = tll_init();
    tll_push_back(default_font_names, "sans serif");

    if (!font_from_name(default_font_names, "", &font_default)) {
        LOG_ERR("%s: failed to instantiate default font", tll_front(default_font_names));
        tll_free(default_font_names);
        return false;
    }
    tll_free(default_font_names);

    *conf = (struct config){
        .max_width = 400,
        .max_height = 200,
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
                    .font = &font_default,
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                },
                .summary = {
                    .font = &font_default,
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                },
                .body = {
                    .font = &font_default,
                    .color = {0x8888, 0x8888, 0x8888, 0xffff},
                },
                .action = {
                    .font = &font_default,
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
                    .font = &font_default,
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .summary = {
                    .font = &font_default,
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .body = {
                    .font = &font_default,
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .action = {
                    .font = &font_default,
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
                    .font = &font_default,
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .summary = {
                    .font = &font_default,
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .body = {
                    .font = &font_default,
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
                .action = {
                    .font = &font_default,
                    .color = {0xffff, 0xffff, 0xffff, 0xffff},
                },
            },
        },
    };

    return true;
}

void
config_destroy(struct config conf)
{
    font_destroy(&font_default);
}
