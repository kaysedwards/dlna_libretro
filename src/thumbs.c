/*
 * thumbs.c - see thumbs.h.
 */
#include <stdlib.h>
#include <string.h>

#include <rthreads/rthreads.h>

#include "thumbs.h"
#include "image.h"
#include "util.h"

#define THUMB_THREADS   3
#define THUMB_BUCKETS   4096
/* Memory budget for decoded thumbnails.  At 300x300 pixels this is about
 * 370 images; at 4K output (thumbnails around 700 pixels) about 70. */
#define THUMB_BUDGET    (128UL * 1024UL * 1024UL)

enum { TN_NEW = 0, TN_QUEUED, TN_LOADING, TN_READY, TN_FAILED };

typedef struct tnode
{
   struct tnode  *next;
   char          *url;
   int            state;
   gfx_image     *img;
   unsigned long  want;    /* stamp * 4 + priority of the latest request */
   unsigned long  shown;   /* stamp of the latest request */
} tnode;

static tnode        *g_buckets[THUMB_BUCKETS];
static slock_t      *g_lock;
static scond_t      *g_cond;
static sthread_t    *g_threads[THUMB_THREADS];
static volatile int  g_quit;
static int           g_size = 256;
static unsigned      g_generation;
static unsigned long g_stamp = 1;
static unsigned long g_bytes;
static int           g_dirty;

static unsigned hash_url(const char *s)
{
   unsigned long h = 5381;
   while (*s)
      h = h * 33 + (unsigned char)*s++;
   return (unsigned)(h % THUMB_BUCKETS);
}

static tnode *lookup(const char *url, int create)
{
   unsigned h = hash_url(url);
   tnode   *n;
   for (n = g_buckets[h]; n; n = n->next)
      if (strcmp(n->url, url) == 0)
         return n;
   if (!create)
      return NULL;
   n = (tnode*)calloc(1, sizeof(*n));
   if (!n)
      return NULL;
   n->url = util_strdup(url);
   if (!n->url)
   {
      free(n);
      return NULL;
   }
   n->next      = g_buckets[h];
   g_buckets[h] = n;
   return n;
}

static size_t image_bytes(const gfx_image *img)
{
   return img ? (size_t)img->width * img->height * 4 : 0;
}

/* Pick the queued node that was most recently asked for and is still
 * (nearly) on screen. */
static tnode *pick_job(void)
{
   tnode        *best = NULL;
   unsigned long min_want = (g_stamp > 2 ? g_stamp - 2 : 0) * 4;
   int           i;

   for (i = 0; i < THUMB_BUCKETS; i++)
   {
      tnode *n;
      for (n = g_buckets[i]; n; n = n->next)
         if (n->state == TN_QUEUED && n->want >= min_want
               && (!best || n->want > best->want))
            best = n;
   }
   return best;
}

static void loader_main(void *arg)
{
   (void)arg;
   slock_lock(g_lock);
   while (!g_quit)
   {
      tnode     *job = pick_job();
      char      *url;
      int        size;
      unsigned   gen;
      gfx_image *img;

      if (!job)
      {
         scond_wait(g_cond, g_lock);
         continue;
      }
      job->state = TN_LOADING;
      url        = util_strdup(job->url);
      size       = g_size;
      gen        = g_generation;
      slock_unlock(g_lock);

      img = url ? image_load(url, size, size, 1, &g_quit) : NULL;

      slock_lock(g_lock);
      /* Nodes are only freed in thumbs_deinit (after these threads are
       * joined), so 'job' is still valid here. */
      if (gen != g_generation || g_quit)
      {
         gfx_image_free(img);
         if (job->state == TN_LOADING)
            job->state = TN_NEW;
      }
      else if (img)
      {
         job->img   = img;
         job->state = TN_READY;
         g_bytes   += image_bytes(img);
         g_dirty    = 1;
      }
      else
      {
         job->state = TN_FAILED;
         g_dirty    = 1;
         dlna_log(LOG_DEBUG, "Thumbnail failed: %s\n", job->url);
      }
      free(url);
   }
   slock_unlock(g_lock);
}

int thumbs_init(void)
{
   int i;

   g_quit = 0;
   g_lock = slock_new();
   g_cond = scond_new();
   if (!g_lock || !g_cond)
      return 0;
   for (i = 0; i < THUMB_THREADS; i++)
      g_threads[i] = sthread_create(loader_main, NULL);
   return 1;
}

void thumbs_deinit(void)
{
   int i;

   if (!g_lock)
      return;
   slock_lock(g_lock);
   g_quit = 1;
   scond_broadcast(g_cond);
   slock_unlock(g_lock);
   for (i = 0; i < THUMB_THREADS; i++)
      if (g_threads[i])
      {
         sthread_join(g_threads[i]);
         g_threads[i] = NULL;
      }
   for (i = 0; i < THUMB_BUCKETS; i++)
   {
      tnode *n = g_buckets[i];
      while (n)
      {
         tnode *next = n->next;
         gfx_image_free(n->img);
         free(n->url);
         free(n);
         n = next;
      }
      g_buckets[i] = NULL;
   }
   g_bytes = 0;
   scond_free(g_cond);
   slock_free(g_lock);
   g_cond = NULL;
   g_lock = NULL;
}

void thumbs_set_size(int px)
{
   int i;

   if (!g_lock || px == g_size)
      return;
   slock_lock(g_lock);
   g_size = px;
   g_generation++;
   for (i = 0; i < THUMB_BUCKETS; i++)
   {
      tnode *n;
      for (n = g_buckets[i]; n; n = n->next)
      {
         gfx_image_free(n->img);
         n->img = NULL;
         if (n->state != TN_LOADING)
            n->state = TN_NEW;
      }
   }
   g_bytes = 0;
   slock_unlock(g_lock);
}

/* Free least recently shown images until we are under budget.  Called
 * with the lock held. */
static void evict(void)
{
   while (g_bytes > THUMB_BUDGET)
   {
      tnode *oldest = NULL;
      int    i;
      for (i = 0; i < THUMB_BUCKETS; i++)
      {
         tnode *n;
         for (n = g_buckets[i]; n; n = n->next)
            if (n->state == TN_READY && n->shown + 1 < g_stamp
                  && (!oldest || n->shown < oldest->shown))
               oldest = n;
      }
      if (!oldest)
         break; /* everything is on screen */
      g_bytes -= image_bytes(oldest->img);
      gfx_image_free(oldest->img);
      oldest->img   = NULL;
      oldest->state = TN_NEW;
   }
}

void thumbs_begin_frame(void)
{
   if (!g_lock)
      return;
   slock_lock(g_lock);
   g_stamp++;
   evict();
   slock_unlock(g_lock);
}

const gfx_image *thumbs_get(const char *url, int prefetch, int *failed)
{
   tnode           *n;
   const gfx_image *img = NULL;

   *failed = 0;
   if (!g_lock || !url || !*url)
   {
      *failed = 1;
      return NULL;
   }
   slock_lock(g_lock);
   n = lookup(url, 1);
   if (n)
   {
      n->want = g_stamp * 4 + (prefetch ? 1 : 2);
      if (!prefetch)
         n->shown = g_stamp;
      switch (n->state)
      {
         case TN_NEW:
            n->state = TN_QUEUED;
            scond_signal(g_cond);
            break;
         case TN_QUEUED:
            /* It may have gone stale (scrolled away and back) while the
             * loaders went to sleep; wake one up. */
            scond_signal(g_cond);
            break;
         case TN_READY:
            /* The image stays valid until the main thread itself calls
             * thumbs_set_size() or thumbs_begin_frame() (only those free
             * images), so handing out the pointer is safe. */
            img = n->img;
            break;
         case TN_FAILED:
            *failed = 1;
            break;
         default:
            break;
      }
   }
   slock_unlock(g_lock);
   return img;
}

int thumbs_take_dirty(void)
{
   int d;
   if (!g_lock)
      return 0;
   slock_lock(g_lock);
   d       = g_dirty;
   g_dirty = 0;
   slock_unlock(g_lock);
   return d;
}
