/*
 * options.c - core option definitions and parsing.  See options.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "options.h"
#include "skin.h"
#include "util.h"

dlna_config g_cfg;

/* ------------------------------------------------------------------ */
/* Colours offered for the panels.  Keep the two tables in step. */

#define COLOR_VALUES \
   { "white",      "White" },       \
   { "light_gray", "Light Gray" },  \
   { "gray",       "Gray" },        \
   { "dark_gray",  "Dark Gray" },   \
   { "charcoal",   "Charcoal" },    \
   { "black",      "Black" },       \
   { "red",        "Red" },         \
   { "dark_red",   "Dark Red" },    \
   { "orange",     "Orange" },      \
   { "yellow",     "Yellow" },      \
   { "gold",       "Gold" },        \
   { "green",      "Green" },       \
   { "dark_green", "Dark Green" },  \
   { "teal",       "Teal" },        \
   { "cyan",       "Cyan" },        \
   { "light_blue", "Light Blue" },  \
   { "blue",       "Blue" },        \
   { "navy",       "Navy" },        \
   { "purple",     "Purple" },      \
   { "magenta",    "Magenta" },     \
   { "pink",       "Pink" },        \
   { "brown",      "Brown" },       \
   { "midnight",   "Midnight" },    \
   { "slate",      "Slate" },       \
   { NULL, NULL }

/* Extra choices offered before the colours where they make sense. */
#define TRANSPARENT_VALUE { "transparent", "Transparent (Nothing)" }
#define IMAGE_VALUE       { "image",       "Image" }

typedef struct named_color
{
   const char *key;
   uint32_t    rgb;
} named_color;

static const named_color colors[] = {
   { "white",      0xFFFFFF },
   { "light_gray", 0xC0C0C0 },
   { "gray",       0x808080 },
   { "dark_gray",  0x404040 },
   { "charcoal",   0x1C1C1C },
   { "black",      0x000000 },
   { "red",        0xE03030 },
   { "dark_red",   0x801818 },
   { "orange",     0xF08020 },
   { "yellow",     0xF0E040 },
   { "gold",       0xD4AF37 },
   { "green",      0x40C040 },
   { "dark_green", 0x186018 },
   { "teal",       0x108080 },
   { "cyan",       0x40E0F0 },
   { "light_blue", 0x7FB2FF },
   { "blue",       0x3060E0 },
   { "navy",       0x14204A },
   { "purple",     0x8040C0 },
   { "magenta",    0xE040E0 },
   { "pink",       0xF090C0 },
   { "brown",      0x805020 },
   { "midnight",   0x101318 },
   { "slate",      0x1E242C },
   { NULL,         0 }
};

#define BUTTON_VALUES \
   { "south", "South (Xbox A / PlayStation Cross / RetroPad B)" },   \
   { "east",  "East (Xbox B / PlayStation Circle / RetroPad A)" },   \
   { "north", "North (Xbox Y / PlayStation Triangle / RetroPad X)" },\
   { "west",  "West (Xbox X / PlayStation Square / RetroPad Y)" },   \
   { NULL, NULL }

/* ------------------------------------------------------------------ */
/* Definitions (core options v2) */

static struct retro_core_option_v2_category categories[] = {
   { "display",  "Display",
     "Canvas resolution, text size and subtitles." },
   { "controls", "Controls",
     "Which buttons do what in the browser and during playback." },
   { "browser",  "Browser",
     "How folders and media are sorted." },
   { "playback", "Playback",
     "How .m3u, .dlna and .json playlists are played." },
   { "colors",   "Colors",
     "Colors and background images of the browser, panels and playback." },
   { NULL, NULL, NULL }
};

static struct retro_core_option_v2_definition definitions[] = {
   {
      "dlna_resolution",
      "Display > Resolution", "Resolution",
      "Size of the canvas the core draws on.  The browser fits as many "
      "thumbnails (at least 256x256 pixels each) as this allows.  Pick one "
      "with the same shape as your screen.",
      NULL, "display",
      {
         { "640x480",   NULL }, { "800x600",   NULL }, { "854x480",   NULL },
         { "1024x600",  NULL }, { "1024x768",  NULL }, { "1280x720",  NULL },
         { "1280x800",  NULL }, { "1280x1024", NULL }, { "1366x768",  NULL },
         { "1440x900",  NULL }, { "1600x900",  NULL }, { "1600x1200", NULL },
         { "1680x1050", NULL }, { "1920x1080", NULL }, { "1920x1200", NULL },
         { "2560x1080", NULL }, { "2560x1440", NULL }, { "2560x1600", NULL },
         { "3440x1440", NULL }, { "3840x2160", NULL },
         { NULL, NULL }
      },
      "1280x720"
   },
   {
      "dlna_font_size",
      "Display > Font Size", "Font Size",
      "Height of the title text in pixels.  Thumbnails shrink to make room.",
      NULL, "display",
      {
         { "16", NULL }, { "18", NULL }, { "20", NULL }, { "22", NULL },
         { "24", NULL }, { "26", NULL }, { "28", NULL }, { "32", NULL },
         { "36", NULL }, { "40", NULL }, { "44", NULL }, { "48", NULL },
         { "56", NULL }, { "64", NULL }, { "72", NULL },
         { NULL, NULL }
      },
      "28"
   },
   {
      "dlna_subtitle_size",
      "Display > Subtitle Size", "Subtitle Size",
      "Scale of text subtitles relative to the default size.",
      NULL, "display",
      {
         { "75",  "75%" }, { "100", "100%" }, { "125", "125%" },
         { "150", "150%" }, { "200", "200%" },
         { NULL, NULL }
      },
      "100"
   },
   {
      "dlna_subtitles",
      "Display > Initial Subtitles", "Initial Subtitles",
      "Which subtitle track is shown when a video starts.",
      NULL, "display",
      {
         { "auto",  "Default/Forced Track" },
         { "off",   "Off" },
         { "first", "First Track" },
         { NULL, NULL }
      },
      "auto"
   },
   {
      "dlna_button_primary",
      "Controls > Open / Play Button", "Open / Play Button",
      "Enters the selected folder or plays the selected media.",
      NULL, "controls",
      { BUTTON_VALUES },
      "south"
   },
   {
      "dlna_button_back",
      "Controls > Back / Stop Button", "Back / Stop Button",
      "Goes up to the parent folder, or stops the media being shown.",
      NULL, "controls",
      { BUTTON_VALUES },
      "east"
   },
   {
      "dlna_button_title",
      "Controls > Show Title Button", "Show Title Button",
      "Shows the title of the media being played.",
      NULL, "controls",
      { BUTTON_VALUES },
      "west"
   },
   {
      "dlna_button_info",
      "Controls > Playback Info Button", "Playback Info Button",
      "Shows or hides the playback information panel.",
      NULL, "controls",
      { BUTTON_VALUES },
      "north"
   },
   {
      "dlna_dpad_tracks",
      "Controls > D-Pad During Playback", "D-Pad During Playback",
      "Which D-Pad directions change the subtitle track and which change "
      "the audio track while media is playing.",
      NULL, "controls",
      {
         { "subs_vertical",  "Up/Down: Subtitles, Left/Right: Audio" },
         { "audio_vertical", "Up/Down: Audio, Left/Right: Subtitles" },
         { NULL, NULL }
      },
      "subs_vertical"
   },
   {
      "dlna_swap_start_select",
      "Controls > Swap Start and Select", "Swap Start and Select",
      "Normally Select cycles the aspect ratio and Start pauses.  Enable "
      "to swap them.",
      NULL, "controls",
      {
         { "disabled", NULL }, { "enabled", NULL }, { NULL, NULL }
      },
      "disabled"
   },
   {
      "dlna_swap_shoulders",
      "Controls > Swap Shoulders and Triggers", "Swap Shoulders and Triggers",
      "Normally the shoulder buttons (L1/R1) set the A-B loop points and "
      "the triggers (L2/R2) seek / page through lists.  Enable to swap "
      "L1 with L2 and R1 with R2.",
      NULL, "controls",
      {
         { "disabled", NULL }, { "enabled", NULL }, { NULL, NULL }
      },
      "disabled"
   },
   {
      "dlna_play_order",
      "Playback > Ordering", "Ordering",
      "Play playlist entries in the order they are listed, or shuffled.",
      NULL, "playback",
      {
         { "normal", "Normal" },
         { "random", "Random" },
         { NULL, NULL }
      },
      "normal"
   },
   {
      "dlna_play_loop",
      "Playback > Looping", "Looping",
      "Individual Selection: play one entry (the first, or a random one) "
      "and return to RetroArch.  Playlist Once: play every entry once and "
      "return.  Repeat Entry: play one entry over and over.  Repeat All: "
      "play the whole list over and over.",
      NULL, "playback",
      {
         { "single",     "Individual Selection" },
         { "once",       "Playlist Once" },
         { "repeat_one", "Repeat Entry" },
         { "repeat_all", "Repeat All" },
         { NULL, NULL }
      },
      "once"
   },
   {
      "dlna_play_duration",
      "Playback > Picture Duration", "Picture Duration",
      "How long each picture in a playlist is shown before moving on.",
      NULL, "playback",
      {
         { "3", "3 seconds" }, { "5", "5 seconds" }, { "10", "10 seconds" },
         { "15", "15 seconds" }, { "20", "20 seconds" },
         { "30", "30 seconds" }, { "60", "1 minute" }, { "300", "5 minutes" },
         { NULL, NULL }
      },
      "10"
   },
   {
      "dlna_sort_order",
      "Browser > Sort Order", "Sort Order",
      "Alphabetical or reverse alphabetical.  Numbers inside names are "
      "compared by value, so \"Track 2\" comes before \"Track 10\".",
      NULL, "browser",
      {
         { "az", "Alphabetical (A-Z)" },
         { "za", "Reverse Alphabetical (Z-A)" },
         { NULL, NULL }
      },
      "az"
   },
   {
      "dlna_sort_folders",
      "Browser > Folder Placement", "Folder Placement",
      "List folders before media, or sort them together as if they were "
      "the same kind of content.",
      NULL, "browser",
      {
         { "first", "Folders First" },
         { "mixed", "Mixed With Media" },
         { NULL, NULL }
      },
      "first"
   },
   {
      "dlna_title_border",
      "Colors > Title Panel Border", "Title Panel Border",
      "Border of the title panel.  Image uses borders.png from the "
      "system/dlna folder (see Border Image Corner and Side).",
      NULL, "colors", { TRANSPARENT_VALUE, IMAGE_VALUE, COLOR_VALUES },
      "light_blue"
   },
   {
      "dlna_title_text",
      "Colors > Title Panel Text", "Title Panel Text",
      NULL, NULL, "colors", { COLOR_VALUES }, "white"
   },
   {
      "dlna_title_background",
      "Colors > Title Panel Background", "Title Panel Background",
      "Image uses title.png from the system/dlna folder (or a colour that "
      "contrasts with the text if it is missing).",
      NULL, "colors", { TRANSPARENT_VALUE, IMAGE_VALUE, COLOR_VALUES },
      "charcoal"
   },
   {
      "dlna_info_border",
      "Colors > Info Panel Border", "Info Panel Border",
      "Border of the playback information panel (and other pop-ups).  "
      "Image uses borders.png from the system/dlna folder.",
      NULL, "colors", { TRANSPARENT_VALUE, IMAGE_VALUE, COLOR_VALUES },
      "white"
   },
   {
      "dlna_info_text",
      "Colors > Info Panel Text", "Info Panel Text",
      NULL, NULL, "colors", { COLOR_VALUES }, "white"
   },
   {
      "dlna_info_background",
      "Colors > Info Panel Background", "Info Panel Background",
      "Also used for pop-up messages.  Image uses information.png from the "
      "system/dlna folder (or a colour that contrasts with the text if it "
      "is missing).",
      NULL, "colors", { TRANSPARENT_VALUE, IMAGE_VALUE, COLOR_VALUES },
      "navy"
   },
   {
      "dlna_selection_border",
      "Colors > Selection Box Border", "Selection Box Border",
      "Frame drawn around the selected thumbnail.  Image uses borders.png "
      "from the system/dlna folder.",
      NULL, "colors", { IMAGE_VALUE, COLOR_VALUES }, "light_blue"
   },
   {
      "dlna_selection_background",
      "Colors > Selection Box Background", "Selection Box Background",
      "Box drawn behind every thumbnail.  Transparent draws no box and puts "
      "the selection frame tightly around the picture itself.  Image uses "
      "selection.png from the system/dlna folder (or a colour that "
      "contrasts with the border if it is missing).",
      NULL, "colors", { TRANSPARENT_VALUE, IMAGE_VALUE, COLOR_VALUES },
      "slate"
   },
   {
      "dlna_border_corner",
      "Colors > Border Image Corner", "Border Image Corner",
      "For borders set to Image: the size in pixels of the squares cut from "
      "the four corners of borders.png.  This is also how thick the border "
      "is (scaled with the canvas: at 1280x720 one image pixel is one "
      "screen pixel).",
      NULL, "colors",
      {
         { "4", "4 pixels" }, { "6", "6 pixels" }, { "8", "8 pixels" },
         { "10", "10 pixels" }, { "12", "12 pixels" }, { "16", "16 pixels" },
         { "20", "20 pixels" }, { "24", "24 pixels" }, { "28", "28 pixels" },
         { "32", "32 pixels" },
         { NULL, NULL }
      },
      "8"
   },
   {
      "dlna_border_side",
      "Colors > Border Image Side", "Border Image Side",
      "For borders set to Image: the length in pixels of the pieces cut "
      "from the middle of each edge of borders.png and repeated along the "
      "sides.  An image made for exactly these sizes is Corner + Side + "
      "Corner pixels square (12 + 96 + 12 = 120).  Match Corner uses "
      "squares the size of the corners.",
      NULL, "colors",
      {
         { "corner", "Match Corner" },
         { "4", "4 pixels" }, { "8", "8 pixels" }, { "12", "12 pixels" },
         { "16", "16 pixels" }, { "24", "24 pixels" }, { "32", "32 pixels" },
         { "48", "48 pixels" }, { "64", "64 pixels" }, { "96", "96 pixels" },
         { "128", "128 pixels" }, { "192", "192 pixels" },
         { "256", "256 pixels" },
         { NULL, NULL }
      },
      "corner"
   },
   {
      "dlna_border_repeat",
      "Colors > Border Image Repeat", "Border Image Repeat",
      "For borders set to Image: how the edge pieces fill each side.  Fit "
      "stretches or squeezes them slightly so a whole number fits exactly "
      "between the corners.  Clip draws them at their real size and cuts "
      "the last one off, which keeps pixel art exact but can leave a seam "
      "next to the far corner.",
      NULL, "colors",
      {
         { "fit", "Fit" },
         { "clip", "Clip" },
         { NULL, NULL }
      },
      "fit"
   },
   {
      "dlna_arrow_color",
      "Colors > Scroll Arrows", "Scroll Arrows",
      "Arrows above and below the thumbnails showing that the list goes on.  "
      "Image uses arrows/backward.png (top) and arrows/forward.png (bottom) "
      "from the system/dlna folder.",
      NULL, "colors", { TRANSPARENT_VALUE, IMAGE_VALUE, COLOR_VALUES },
      "light_blue"
   },
   {
      "dlna_arrow_opacity",
      "Colors > Scroll Arrow Opacity", "Scroll Arrow Opacity",
      "How opaque the scroll arrows are.",
      NULL, "colors",
      {
         { "100", "100%" }, { "80", "80%" }, { "60", "60%" },
         { "40", "40%" }, { "25", "25%" },
         { NULL, NULL }
      },
      "60"
   },
   {
      "dlna_navigation_background",
      "Colors > Navigation Background", "Navigation Background",
      "Background of the browser.  Image uses navigation.png from the "
      "system/dlna folder, scaled to cover the screen.",
      NULL, "colors", { IMAGE_VALUE, COLOR_VALUES }, "midnight"
   },
   {
      "dlna_playback_background",
      "Colors > Playback Background", "Playback Background",
      "Shown around videos and pictures that do not fill the screen, and "
      "behind music.  Image uses playback.png from the system/dlna folder.",
      NULL, "colors", { IMAGE_VALUE, COLOR_VALUES }, "black"
   },
   {
      "dlna_visualization",
      "Colors > Visualization", "Visualization",
      "Colour of the spectrum bars shown over the cover art while music "
      "plays.  Transparent turns the visualization off.",
      NULL, "colors", { TRANSPARENT_VALUE, COLOR_VALUES }, "light_blue"
   },
   {
      "dlna_panel_opacity",
      "Colors > Panel Opacity", "Panel Opacity",
      "How opaque the panel backgrounds are.",
      NULL, "colors",
      {
         { "100", "100%" }, { "90", "90%" }, { "85", "85%" },
         { "75", "75%" }, { "60", "60%" }, { "50", "50%" },
         { NULL, NULL }
      },
      "85"
   },
   { NULL, NULL, NULL, NULL, NULL, NULL, { { NULL, NULL } }, NULL }
};

/* ------------------------------------------------------------------ */
/* Registration with fallbacks for older frontends */

static struct retro_core_option_definition *g_v1_defs;
static struct retro_variable              *g_v0_vars;
static char                              **g_v0_strings;
static size_t                              g_v0_count;

static size_t count_definitions(void)
{
   size_t n = 0;
   while (definitions[n].key)
      n++;
   return n;
}

void options_init(retro_environment_t cb)
{
   unsigned version = 0;
   size_t   n       = count_definitions();
   size_t   i, j;

   if (!cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version))
      version = 0;

   if (version >= 2)
   {
      struct retro_core_options_v2 opts;
      opts.categories  = categories;
      opts.definitions = definitions;
      if (cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &opts))
         return;
   }

   if (version >= 1)
   {
      /* v1: same thing without categories. */
      g_v1_defs = (struct retro_core_option_definition*)
         util_calloc(n + 1, sizeof(*g_v1_defs));
      if (g_v1_defs)
      {
         for (i = 0; i < n; i++)
         {
            g_v1_defs[i].key  = definitions[i].key;
            g_v1_defs[i].desc = definitions[i].desc;
            g_v1_defs[i].info = definitions[i].info;
            memcpy(g_v1_defs[i].values, definitions[i].values,
                  sizeof(g_v1_defs[i].values));
            g_v1_defs[i].default_value = definitions[i].default_value;
         }
         if (cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS, g_v1_defs))
            return;
      }
   }

   /* v0: "Description; default|value2|value3" strings. */
   g_v0_vars    = (struct retro_variable*)util_calloc(n + 1, sizeof(*g_v0_vars));
   g_v0_strings = (char**)util_calloc(n, sizeof(char*));
   if (!g_v0_vars || !g_v0_strings)
      return;
   g_v0_count = n;
   for (i = 0; i < n; i++)
   {
      const struct retro_core_option_v2_definition *d = &definitions[i];
      size_t len = strlen(d->desc) + 3;
      char  *s;
      for (j = 0; d->values[j].value; j++)
         len += strlen(d->values[j].value) + 1;
      s = (char*)malloc(len);
      if (!s)
         return;
      strcpy(s, d->desc);
      strcat(s, "; ");
      strcat(s, d->default_value);
      for (j = 0; d->values[j].value; j++)
      {
         if (strcmp(d->values[j].value, d->default_value) == 0)
            continue;
         strcat(s, "|");
         strcat(s, d->values[j].value);
      }
      g_v0_strings[i]    = s;
      g_v0_vars[i].key   = d->key;
      g_v0_vars[i].value = s;
   }
   cb(RETRO_ENVIRONMENT_SET_VARIABLES, g_v0_vars);
}

void options_deinit(void)
{
   size_t i;
   free(g_v1_defs);
   g_v1_defs = NULL;
   for (i = 0; i < g_v0_count; i++)
      free(g_v0_strings[i]);
   free(g_v0_strings);
   free(g_v0_vars);
   g_v0_strings = NULL;
   g_v0_vars    = NULL;
   g_v0_count   = 0;
}

/* ------------------------------------------------------------------ */
/* Reading */

static const char *default_of(const char *key)
{
   size_t i;
   for (i = 0; definitions[i].key; i++)
      if (strcmp(definitions[i].key, key) == 0)
         return definitions[i].default_value;
   return "";
}

static const char *get_var(retro_environment_t cb, const char *key)
{
   struct retro_variable var;
   var.key   = key;
   var.value = NULL;
   if (cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && *var.value)
      return var.value;
   return default_of(key);
}

static uint32_t parse_color(const char *s, const char *fallback)
{
   size_t i;
   if (strcmp(s, "transparent") == 0)
      return COLOR_TRANSPARENT;
   if (strcmp(s, "image") == 0)
      return COLOR_IMAGE;
   for (i = 0; colors[i].key; i++)
      if (strcmp(colors[i].key, s) == 0)
         return colors[i].rgb;
   for (i = 0; colors[i].key; i++)
      if (strcmp(colors[i].key, fallback) == 0)
         return colors[i].rgb;
   return 0xFFFFFF;
}

static unsigned parse_button(const char *s)
{
   if (strcmp(s, "east") == 0)
      return RETRO_DEVICE_ID_JOYPAD_A;
   if (strcmp(s, "north") == 0)
      return RETRO_DEVICE_ID_JOYPAD_X;
   if (strcmp(s, "west") == 0)
      return RETRO_DEVICE_ID_JOYPAD_Y;
   return RETRO_DEVICE_ID_JOYPAD_B; /* south */
}

int options_update(retro_environment_t cb)
{
   dlna_config old = g_cfg;
   dlna_config c   = g_cfg;
   const char *v;
   int         changed = 0;

   v = get_var(cb, "dlna_resolution");
   if (!util_parse_resolution(v, &c.width, &c.height)
         || c.width > CANVAS_MAX_WIDTH || c.height > CANVAS_MAX_HEIGHT)
   {
      c.width  = 1280;
      c.height = 720;
   }
   c.font_size = atoi(get_var(cb, "dlna_font_size"));
   if (c.font_size < 8 || c.font_size > 200)
      c.font_size = 28;
   c.subtitle_scale = atoi(get_var(cb, "dlna_subtitle_size"));
   if (c.subtitle_scale < 25 || c.subtitle_scale > 400)
      c.subtitle_scale = 100;
   v = get_var(cb, "dlna_subtitles");
   c.subtitles = strcmp(v, "off") == 0 ? SUBS_OFF
      : strcmp(v, "first") == 0 ? SUBS_FIRST : SUBS_AUTO;

   c.btn_primary = parse_button(get_var(cb, "dlna_button_primary"));
   c.btn_back    = parse_button(get_var(cb, "dlna_button_back"));
   c.btn_title   = parse_button(get_var(cb, "dlna_button_title"));
   c.btn_info    = parse_button(get_var(cb, "dlna_button_info"));
   c.dpad_subs_vertical =
      strcmp(get_var(cb, "dlna_dpad_tracks"), "audio_vertical") != 0;
   c.swap_start_select =
      strcmp(get_var(cb, "dlna_swap_start_select"), "enabled") == 0;
   c.swap_shoulders =
      strcmp(get_var(cb, "dlna_swap_shoulders"), "enabled") == 0;

   c.play_random = strcmp(get_var(cb, "dlna_play_order"), "random") == 0;
   v = get_var(cb, "dlna_play_loop");
   c.play_loop = strcmp(v, "single") == 0 ? LOOP_SINGLE
      : strcmp(v, "repeat_one") == 0 ? LOOP_REPEAT_ONE
      : strcmp(v, "repeat_all") == 0 ? LOOP_REPEAT_ALL : LOOP_ONCE;
   c.picture_seconds = atoi(get_var(cb, "dlna_play_duration"));
   if (c.picture_seconds < 1 || c.picture_seconds > 3600)
      c.picture_seconds = 10;

   c.sort_descending = strcmp(get_var(cb, "dlna_sort_order"), "za") == 0;
   c.folders_first   = strcmp(get_var(cb, "dlna_sort_folders"), "mixed") != 0;

   c.title_border = parse_color(get_var(cb, "dlna_title_border"), "light_blue");
   c.title_text   = parse_color(get_var(cb, "dlna_title_text"), "white");
   c.title_bg     = parse_color(get_var(cb, "dlna_title_background"), "charcoal");
   c.info_border  = parse_color(get_var(cb, "dlna_info_border"), "white");
   c.info_text    = parse_color(get_var(cb, "dlna_info_text"), "white");
   c.info_bg      = parse_color(get_var(cb, "dlna_info_background"), "navy");
   c.sel_border   = parse_color(get_var(cb, "dlna_selection_border"), "light_blue");
   c.sel_bg       = parse_color(get_var(cb, "dlna_selection_background"), "slate");
   c.nav_bg       = parse_color(get_var(cb, "dlna_navigation_background"), "midnight");
   c.play_bg      = parse_color(get_var(cb, "dlna_playback_background"), "black");
   c.vis_color    = parse_color(get_var(cb, "dlna_visualization"), "light_blue");
   /* The selection border cannot be transparent (it would be invisible). */
   if (COLOR_IS_TRANSPARENT(c.sel_border))
      c.sel_border = 0x7FB2FF;
   c.border_corner = atoi(get_var(cb, "dlna_border_corner"));
   if (c.border_corner < 2 || c.border_corner > 64)
      c.border_corner = 8;
   /* "corner" (Match Corner) is not a number, so atoi gives 0. */
   c.border_side = atoi(get_var(cb, "dlna_border_side"));
   if (c.border_side < 1 || c.border_side > 1024)
      c.border_side = c.border_corner;
   c.border_fit = strcmp(get_var(cb, "dlna_border_repeat"), "clip") != 0;
   c.arrow_color  = parse_color(get_var(cb, "dlna_arrow_color"), "light_blue");
   c.arrow_alpha  = (unsigned)(atoi(get_var(cb, "dlna_arrow_opacity")) * 255 / 100);
   if (c.arrow_alpha < 16 || c.arrow_alpha > 255)
      c.arrow_alpha = 153;
   c.panel_alpha  = (unsigned)(atoi(get_var(cb, "dlna_panel_opacity")) * 255 / 100);
   if (c.panel_alpha < 64 || c.panel_alpha > 255)
      c.panel_alpha = 217;

   g_cfg = c;

   if (c.width != old.width || c.height != old.height)
      changed |= OPT_CHANGED_GEOMETRY | OPT_CHANGED_LAYOUT;
   if (c.font_size != old.font_size)
      changed |= OPT_CHANGED_LAYOUT;
   if (c.sort_descending != old.sort_descending
         || c.folders_first != old.folders_first)
      changed |= OPT_CHANGED_SORT;
   if (c.btn_primary != old.btn_primary || c.btn_back != old.btn_back
         || c.btn_title != old.btn_title || c.btn_info != old.btn_info
         || c.dpad_subs_vertical != old.dpad_subs_vertical
         || c.swap_start_select != old.swap_start_select
         || c.swap_shoulders != old.swap_shoulders)
      changed |= OPT_CHANGED_INPUT;
   if (c.title_border != old.title_border || c.title_text != old.title_text
         || c.title_bg != old.title_bg || c.info_border != old.info_border
         || c.info_text != old.info_text || c.info_bg != old.info_bg
         || c.panel_alpha != old.panel_alpha
         || c.sel_border != old.sel_border || c.sel_bg != old.sel_bg
         || c.nav_bg != old.nav_bg || c.play_bg != old.play_bg
         || c.vis_color != old.vis_color
         || c.border_corner != old.border_corner
         || c.border_side != old.border_side
         || c.border_fit != old.border_fit
         || c.arrow_color != old.arrow_color || c.arrow_alpha != old.arrow_alpha
         || c.subtitle_scale != old.subtitle_scale)
      changed |= OPT_CHANGED_COLORS;
   return changed;
}
