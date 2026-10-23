/*
 * font.c - FreeType text rendering with fallbacks.  See font.h.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "font.h"
#include "util.h"

#define MAX_FACES      12
#define GLYPH_BUCKETS  2048
#define GLYPH_CACHE_MAX 6000

typedef struct font_face
{
   FT_Face face;
   int     cur_px;
   char    path[512];
} font_face;

typedef struct glyph
{
   struct glyph  *next;
   int            face;     /* -1 = built-in bitmap font */
   unsigned long  index;    /* FreeType glyph index or ASCII code */
   int            px;
   int            width, height, pitch;
   int            left, top;  /* bearing: bitmap offset from pen/baseline */
   int            advance;
   unsigned char *bitmap;
} glyph;

static FT_Library g_lib;
static int        g_lib_ok;
static font_face  g_faces[MAX_FACES];
static int        g_num_faces;
static glyph     *g_buckets[GLYPH_BUCKETS];
static int        g_glyph_count;

/* ------------------------------------------------------------------ */
/* Built-in 5x7 bitmap font for printable ASCII (32..126).  Each glyph is
 * seven rows; bit 4 of each byte is the leftmost pixel.  This is only used
 * when no TrueType/OpenType font could be loaded at all. */
static const unsigned char builtin_font[95][7] = {
   {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
   {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, /* ! */
   {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00}, /* " */
   {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A}, /* # */
   {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04}, /* $ */
   {0x18,0x19,0x02,0x04,0x08,0x13,0x03}, /* % */
   {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}, /* & */
   {0x0C,0x04,0x08,0x00,0x00,0x00,0x00}, /* ' */
   {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, /* ( */
   {0x08,0x04,0x02,0x02,0x02,0x04,0x08}, /* ) */
   {0x00,0x04,0x15,0x0E,0x15,0x04,0x00}, /* * */
   {0x00,0x04,0x04,0x1F,0x04,0x04,0x00}, /* + */
   {0x00,0x00,0x00,0x00,0x0C,0x04,0x08}, /* , */
   {0x00,0x00,0x00,0x1F,0x00,0x00,0x00}, /* - */
   {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, /* . */
   {0x00,0x01,0x02,0x04,0x08,0x10,0x00}, /* / */
   {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
   {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
   {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
   {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E}, /* 3 */
   {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
   {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* 5 */
   {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
   {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
   {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
   {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* 9 */
   {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, /* : */
   {0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08}, /* ; */
   {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, /* < */
   {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00}, /* = */
   {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, /* > */
   {0x0E,0x11,0x01,0x02,0x04,0x00,0x04}, /* ? */
   {0x0E,0x11,0x01,0x0D,0x15,0x15,0x0E}, /* @ */
   {0x0E,0x11,0x11,0x11,0x1F,0x11,0x11}, /* A */
   {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
   {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
   {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, /* D */
   {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
   {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
   {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, /* G */
   {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
   {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
   {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, /* J */
   {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
   {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* L */
   {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* M */
   {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, /* N */
   {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
   {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
   {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* Q */
   {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
   {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* S */
   {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
   {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
   {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
   {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, /* W */
   {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
   {0x11,0x11,0x11,0x0A,0x04,0x04,0x04}, /* Y */
   {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /* Z */
   {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E}, /* [ */
   {0x00,0x10,0x08,0x04,0x02,0x01,0x00}, /* backslash */
   {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E}, /* ] */
   {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, /* ^ */
   {0x00,0x00,0x00,0x00,0x00,0x00,0x1F}, /* _ */
   {0x08,0x04,0x02,0x00,0x00,0x00,0x00}, /* ` */
   {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F}, /* a */
   {0x10,0x10,0x16,0x19,0x11,0x11,0x1E}, /* b */
   {0x00,0x00,0x0E,0x10,0x10,0x11,0x0E}, /* c */
   {0x01,0x01,0x0D,0x13,0x11,0x11,0x0F}, /* d */
   {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E}, /* e */
   {0x06,0x09,0x08,0x1C,0x08,0x08,0x08}, /* f */
   {0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E}, /* g */
   {0x10,0x10,0x16,0x19,0x11,0x11,0x11}, /* h */
   {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E}, /* i */
   {0x02,0x00,0x06,0x02,0x02,0x12,0x0C}, /* j */
   {0x10,0x10,0x12,0x14,0x18,0x14,0x12}, /* k */
   {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}, /* l */
   {0x00,0x00,0x1A,0x15,0x15,0x11,0x11}, /* m */
   {0x00,0x00,0x16,0x19,0x11,0x11,0x11}, /* n */
   {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E}, /* o */
   {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10}, /* p */
   {0x00,0x00,0x0D,0x13,0x0F,0x01,0x01}, /* q */
   {0x00,0x00,0x16,0x19,0x10,0x10,0x10}, /* r */
   {0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E}, /* s */
   {0x08,0x08,0x1C,0x08,0x08,0x09,0x06}, /* t */
   {0x00,0x00,0x11,0x11,0x11,0x13,0x0D}, /* u */
   {0x00,0x00,0x11,0x11,0x11,0x0A,0x04}, /* v */
   {0x00,0x00,0x11,0x11,0x15,0x15,0x0A}, /* w */
   {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}, /* x */
   {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E}, /* y */
   {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}, /* z */
   {0x02,0x04,0x04,0x08,0x04,0x04,0x02}, /* { */
   {0x04,0x04,0x04,0x04,0x04,0x04,0x04}, /* | */
   {0x08,0x04,0x04,0x02,0x04,0x04,0x08}, /* } */
   {0x00,0x00,0x08,0x15,0x02,0x00,0x00}  /* ~ */
};

static int builtin_scale(int px)
{
   int s = (px + 4) / 8;
   return s < 1 ? 1 : s;
}

/* ------------------------------------------------------------------ */
/* Face loading */

static int face_loaded(const char *path)
{
   int i;
   for (i = 0; i < g_num_faces; i++)
      if (strcmp(g_faces[i].path, path) == 0)
         return 1;
   return 0;
}

static int try_load_face(const char *path)
{
   FT_Face face;

   if (!g_lib_ok || g_num_faces >= MAX_FACES || !path || !*path)
      return 0;
   if (face_loaded(path) || !util_file_exists(path))
      return 0;
   if (FT_New_Face(g_lib, path, 0, &face) != 0)
      return 0;
   /* Bitmap-only fonts (such as colour emoji fonts) cannot be scaled to
    * arbitrary sizes; skip them. */
   if (!FT_IS_SCALABLE(face))
   {
      FT_Done_Face(face);
      return 0;
   }
   if (FT_Select_Charmap(face, FT_ENCODING_UNICODE) != 0)
   {
      FT_Done_Face(face);
      return 0;
   }
   g_faces[g_num_faces].face   = face;
   g_faces[g_num_faces].cur_px = 0;
   strncpy(g_faces[g_num_faces].path, path, sizeof(g_faces[0].path) - 1);
   g_faces[g_num_faces].path[sizeof(g_faces[0].path) - 1] = '\0';
   g_num_faces++;
   dlna_log(LOG_INFO, "Loaded font %s\n", path);
   return 1;
}

/* Try a file relative to a directory. */
static int try_load_rel(const char *dir, const char *rel)
{
   char path[1024];
   if (!dir || !*dir)
      return 0;
   util_path_join(path, sizeof(path), dir, rel);
   return try_load_face(path);
}

/* RetroArch keeps its fonts in its "assets" directory.  A core cannot ask
 * the frontend where that is, so we look in the places it normally lives:
 * next to the system directory, in the per-user config directory and in
 * the system wide install locations. */
static void assets_dirs(const char *system_dir, char dirs[][1024], int *count)
{
   const char *home = getenv("HOME");
   int         n    = 0;

   if (system_dir && *system_dir)
   {
      util_path_join(dirs[n++], 1024, system_dir, "../assets");
   }
   if (home)
   {
      util_path_join(dirs[n++], 1024, home, ".config/retroarch/assets");
      util_path_join(dirs[n++], 1024, home,
            "Library/Application Support/RetroArch/assets");
   }
#ifdef _WIN32
   {
      const char *appdata = getenv("APPDATA");
      if (appdata)
         util_path_join(dirs[n++], 1024, appdata, "RetroArch\\assets");
   }
#endif
   strcpy(dirs[n++], "/usr/share/libretro/assets");
   strcpy(dirs[n++], "/usr/share/retroarch/assets");
   strcpy(dirs[n++], "/usr/local/share/libretro/assets");
   strcpy(dirs[n++], "/storage/emulated/0/RetroArch/assets");
   *count = n;
}

static const char *const retroarch_primary_fonts[] = {
   "pkg/osd-font.ttf",
   "ozone/regular.ttf",
   "xmb/monochrome/font.ttf",
   "glui/font.ttf",
   NULL
};

static const char *const retroarch_fallback_fonts[] = {
   "pkg/fallback-font.ttf",
   "pkg/chinese-fallback-font.ttf",
   "pkg/korean-fallback-font.ttf",
   NULL
};

static const char *const system_primary_fonts[] = {
   "/usr/share/fonts/TTF/DejaVuSans.ttf",
   "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
   "/usr/share/fonts/dejavu/DejaVuSans.ttf",
   "/usr/share/fonts/noto/NotoSans-Regular.ttf",
   "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
   "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
   "/system/fonts/Roboto-Regular.ttf",
   "/System/Library/Fonts/Helvetica.ttc",
   "C:\\Windows\\Fonts\\segoeui.ttf",
   "C:\\Windows\\Fonts\\arial.ttf",
   NULL
};

/* Fonts with wide symbol / CJK coverage, used for fallback glyphs. */
static const char *const system_fallback_fonts[] = {
   "/usr/share/fonts/noto/NotoSansSymbols2-Regular.ttf",
   "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf",
   "/usr/share/fonts/google-noto/NotoSansSymbols2-Regular.ttf",
   "/usr/share/fonts/noto/NotoSansSymbols-Regular.ttf",
   "/usr/share/fonts/truetype/noto/NotoSansSymbols-Regular.ttf",
   "/usr/share/fonts/TTF/DejaVuSans.ttf",
   "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
   "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
   "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
   "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
   "/system/fonts/NotoSansSymbols-Regular-Subsetted.ttf",
   "/system/fonts/NotoSansCJK-Regular.ttc",
   "/System/Library/Fonts/Apple Symbols.ttf",
   "/System/Library/Fonts/Hiragino Sans GB.ttc",
   "C:\\Windows\\Fonts\\seguisym.ttf",
   "C:\\Windows\\Fonts\\seguiemj.ttf",
   "C:\\Windows\\Fonts\\msyh.ttc",
   NULL
};

int font_init(const char *system_dir)
{
   char dirs[8][1024];
   char dlna_dir[1024];
   int  ndirs = 0;
   int  i, j;

   font_deinit();
   if (FT_Init_FreeType(&g_lib) != 0)
   {
      dlna_log(LOG_ERROR, "FreeType failed to initialise; using built-in font\n");
      return 0;
   }
   g_lib_ok = 1;

   /* 1 + 2: the user's own font. */
   util_path_join(dlna_dir, sizeof(dlna_dir), system_dir, "dlna");
   if (!try_load_rel(dlna_dir, "font.ttf"))
      try_load_rel(dlna_dir, "font.otf");

   /* 3: RetroArch's fonts. */
   assets_dirs(system_dir, dirs, &ndirs);
   for (i = 0; g_num_faces == 0 && i < ndirs; i++)
      for (j = 0; g_num_faces == 0 && retroarch_primary_fonts[j]; j++)
         try_load_rel(dirs[i], retroarch_primary_fonts[j]);

   /* 4: operating system fonts. */
   for (i = 0; g_num_faces == 0 && system_primary_fonts[i]; i++)
      try_load_face(system_primary_fonts[i]);

   /* Fallbacks for glyph coverage. */
   for (i = 0; i < ndirs; i++)
      for (j = 0; retroarch_fallback_fonts[j]; j++)
         try_load_rel(dirs[i], retroarch_fallback_fonts[j]);
   for (i = 0; system_fallback_fonts[i]; i++)
      try_load_face(system_fallback_fonts[i]);

   if (!g_num_faces)
      dlna_log(LOG_WARN, "No usable font found; using built-in bitmap font\n");
   return g_num_faces > 0;
}

void font_flush_cache(void)
{
   int i;
   for (i = 0; i < GLYPH_BUCKETS; i++)
   {
      glyph *g = g_buckets[i];
      while (g)
      {
         glyph *next = g->next;
         free(g->bitmap);
         free(g);
         g = next;
      }
      g_buckets[i] = NULL;
   }
   g_glyph_count = 0;
}

void font_deinit(void)
{
   int i;

   font_flush_cache();
   for (i = 0; i < g_num_faces; i++)
      FT_Done_Face(g_faces[i].face);
   g_num_faces = 0;
   if (g_lib_ok)
      FT_Done_FreeType(g_lib);
   g_lib_ok = 0;
}

const char *font_primary_name(void)
{
   return g_num_faces ? g_faces[0].path : "built-in";
}

/* ------------------------------------------------------------------ */
/* Glyph lookup and caching */

static void set_px(int face, int px)
{
   if (g_faces[face].cur_px != px)
   {
      FT_Set_Pixel_Sizes(g_faces[face].face, 0, (FT_UInt)px);
      g_faces[face].cur_px = px;
   }
}

/* Find which face has a code point.  Returns the face index (or -1 when no
 * face has it) and the glyph index via *gi. */
static int find_glyph(long cp, unsigned long *gi)
{
   int i;
   for (i = 0; i < g_num_faces; i++)
   {
      FT_UInt idx = FT_Get_Char_Index(g_faces[i].face, (FT_ULong)cp);
      if (idx)
      {
         *gi = idx;
         return i;
      }
   }
   return -1;
}

int font_has_glyph(long codepoint)
{
   unsigned long gi;
   return find_glyph(codepoint, &gi) >= 0;
}

static glyph *build_builtin_glyph(long cp, int px)
{
   glyph               *g;
   int                  scale = builtin_scale(px);
   int                  idx   = (cp >= 32 && cp <= 126) ? (int)cp - 32 : '?' - 32;
   const unsigned char *rows  = builtin_font[idx];
   int                  x, y;

   g = (glyph*)calloc(1, sizeof(*g));
   if (!g)
      return NULL;
   g->face    = -1;
   g->index   = (unsigned long)cp;
   g->px      = px;
   g->width   = 5 * scale;
   g->height  = 7 * scale;
   g->pitch   = g->width;
   g->left    = 0;
   g->top     = 7 * scale;
   g->advance = 6 * scale;
   g->bitmap  = (unsigned char*)calloc((size_t)g->pitch * g->height, 1);
   if (!g->bitmap)
   {
      free(g);
      return NULL;
   }
   for (y = 0; y < g->height; y++)
      for (x = 0; x < g->width; x++)
         if (rows[y / scale] & (0x10 >> (x / scale)))
            g->bitmap[y * g->pitch + x] = 255;
   return g;
}

static glyph *build_ft_glyph(int face, unsigned long gi, int px)
{
   FT_GlyphSlot slot;
   FT_Bitmap   *bm;
   glyph       *g;
   int          x, y;

   set_px(face, px);
   if (FT_Load_Glyph(g_faces[face].face, (FT_UInt)gi,
            FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT) != 0)
      return NULL;
   slot = g_faces[face].face->glyph;
   if (slot->format != FT_GLYPH_FORMAT_BITMAP
         && FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0)
      return NULL;
   bm = &slot->bitmap;

   g = (glyph*)calloc(1, sizeof(*g));
   if (!g)
      return NULL;
   g->face    = face;
   g->index   = gi;
   g->px      = px;
   g->width   = (int)bm->width;
   g->height  = (int)bm->rows;
   g->pitch   = g->width;
   g->left    = slot->bitmap_left;
   g->top     = slot->bitmap_top;
   g->advance = (int)((slot->advance.x + 32) >> 6);
   if (g->width > 0 && g->height > 0)
   {
      g->bitmap = (unsigned char*)calloc((size_t)g->pitch * g->height, 1);
      if (!g->bitmap)
      {
         free(g);
         return NULL;
      }
      for (y = 0; y < g->height; y++)
      {
         const unsigned char *src = bm->buffer + (long)y * bm->pitch;
         unsigned char       *dst = g->bitmap + y * g->pitch;
         for (x = 0; x < g->width; x++)
         {
            switch (bm->pixel_mode)
            {
               case FT_PIXEL_MODE_MONO:
                  dst[x] = (src[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
                  break;
               case FT_PIXEL_MODE_BGRA:
                  dst[x] = src[x * 4 + 3];
                  break;
               case FT_PIXEL_MODE_GRAY:
               default:
                  dst[x] = src[x];
                  break;
            }
         }
      }
   }
   return g;
}

static glyph *get_glyph(long cp, int px)
{
   unsigned long gi = 0;
   int           face;
   unsigned      h;
   glyph        *g;

   face = find_glyph(cp, &gi);
   if (face < 0)
   {
      if (g_num_faces > 0)
      {
         /* Not in any font: use the primary font's "missing" box. */
         face = 0;
         gi   = 0;
      }
      else
         gi = (unsigned long)cp; /* built-in */
   }

   h = (unsigned)((gi * 2654435761UL) ^ ((unsigned long)face * 977UL)
         ^ ((unsigned long)px * 131UL)) % GLYPH_BUCKETS;
   for (g = g_buckets[h]; g; g = g->next)
      if (g->face == face && g->index == gi && g->px == px)
         return g;

   if (g_glyph_count >= GLYPH_CACHE_MAX)
   {
      font_flush_cache();
      h = (unsigned)((gi * 2654435761UL) ^ ((unsigned long)face * 977UL)
            ^ ((unsigned long)px * 131UL)) % GLYPH_BUCKETS;
   }

   if (face < 0)
      g = build_builtin_glyph(cp, px);
   else
      g = build_ft_glyph(face, gi, px);
   if (!g)
      return NULL;
   g->next      = g_buckets[h];
   g_buckets[h] = g;
   g_glyph_count++;
   return g;
}

/* ------------------------------------------------------------------ */
/* Metrics and drawing */

int font_ascent(int px)
{
   if (!g_num_faces)
      return 7 * builtin_scale(px);
   set_px(0, px);
   return (int)((g_faces[0].face->size->metrics.ascender + 63) >> 6);
}

int font_descent(int px)
{
   if (!g_num_faces)
      return builtin_scale(px);
   set_px(0, px);
   return (int)((-g_faces[0].face->size->metrics.descender + 63) >> 6);
}

int font_line_height(int px)
{
   int h;
   if (!g_num_faces)
      return 9 * builtin_scale(px);
   set_px(0, px);
   h = (int)((g_faces[0].face->size->metrics.height + 63) >> 6);
   if (h < font_ascent(px) + font_descent(px))
      h = font_ascent(px) + font_descent(px);
   return h;
}

int font_text_width(const char *utf8, int px)
{
   int  w = 0;
   long cp;

   if (!utf8)
      return 0;
   while ((cp = util_utf8_next(&utf8)) >= 0)
   {
      glyph *g = get_glyph(cp, px);
      if (g)
         w += g->advance;
   }
   return w;
}

void font_draw(gfx_surface *s, const char *utf8, int x, int baseline, int px,
      uint32_t rgb, unsigned alpha)
{
   long cp;

   if (!utf8)
      return;
   while ((cp = util_utf8_next(&utf8)) >= 0)
   {
      glyph *g = get_glyph(cp, px);
      if (!g)
         continue;
      if (g->bitmap)
         gfx_blend_mask(s, g->bitmap, g->width, g->height, g->pitch,
               x + g->left, baseline - g->top, rgb, alpha);
      x += g->advance;
   }
}

void font_draw_outlined(gfx_surface *s, const char *utf8, int x, int baseline,
      int px, uint32_t rgb, uint32_t outline_rgb, int outline)
{
   int dx, dy;

   if (outline > 0)
   {
      for (dy = -outline; dy <= outline; dy++)
         for (dx = -outline; dx <= outline; dx++)
         {
            if (!dx && !dy)
               continue;
            /* Round the corners of the outline a little. */
            if (dx * dx + dy * dy > outline * outline + 1)
               continue;
            font_draw(s, utf8, x + dx, baseline + dy, px, outline_rgb, 255);
         }
   }
   font_draw(s, utf8, x, baseline, px, rgb, 255);
}

int font_ellipsize(const char *utf8, int px, int max_w, char *out, size_t size)
{
   static const char ellipsis[] = "\xE2\x80\xA6"; /* U+2026 */
   const char       *p;
   const char       *dots;
   int               w, dots_w;
   size_t            len;

   if (!utf8)
      utf8 = "";
   w = font_text_width(utf8, px);
   if (w <= max_w)
   {
      strncpy(out, utf8, size - 1);
      out[size - 1] = '\0';
      return w;
   }

   /* Use "..." if no font has the ellipsis character. */
   dots   = font_has_glyph(0x2026) ? ellipsis : "...";
   dots_w = font_text_width(dots, px);
   w      = 0;
   p      = utf8;
   for (;;)
   {
      const char *prev = p;
      long        cp   = util_utf8_next(&p);
      glyph      *g;
      if (cp < 0)
         break;
      g = get_glyph(cp, px);
      if (g && w + g->advance + dots_w > max_w)
      {
         p = prev;
         break;
      }
      if (g)
         w += g->advance;
   }
   len = (size_t)(p - utf8);
   if (len + strlen(dots) + 1 > size)
      len = size > strlen(dots) + 1 ? size - strlen(dots) - 1 : 0;
   memcpy(out, utf8, len);
   strcpy(out + len, dots);
   return w + dots_w;
}
