/*
 * dlna.h - everything that talks UPnP / DLNA.
 *
 * Built on pupnp (libupnp).  Discovery runs in pupnp's own threads; every
 * other network operation (browsing folders, resolving dlna:// paths) runs
 * on a private worker thread so that retro_run() never blocks.  The main
 * thread submits requests (each gets a numeric token) and polls for
 * results once per frame.
 */
#ifndef DLNA_DLNA_H
#define DLNA_DLNA_H

#include <stddef.h>
#include "util.h"

typedef enum entry_kind
{
   ENT_SERVER = 0,  /* a media server (top level list)               */
   ENT_TYPEROOT,    /* the virtual Audio / Picture / Video entries    */
   ENT_FOLDER,      /* a DIDL-Lite container                          */
   ENT_MEDIA        /* a playable item                                */
} entry_kind;

typedef struct dlna_entry
{
   entry_kind kind;
   media_type media;     /* ENT_MEDIA: its type.  ENT_TYPEROOT: the tree. */
   char      *title;
   char      *id;        /* DIDL object id (servers: their UDN)          */
   char      *parent_id; /* DIDL parentID, may be NULL                    */
   char      *url;       /* ENT_MEDIA: the resource to play               */
   char      *thumb_url; /* a thumbnail / icon / album art, may be NULL   */
   double     duration;  /* seconds, negative when unknown                */
   int        server;    /* index of the server this came from            */
   long       order;     /* position as delivered by the server           */
} dlna_entry;

dlna_entry *dlna_entry_new(entry_kind kind);
dlna_entry *dlna_entry_copy(const dlna_entry *e);
void        dlna_entry_free(dlna_entry *e);
void        dlna_entries_free(dlna_entry **list, size_t count);

/* Start/stop the UPnP stack and the worker thread.  system_dir is used to
 * find the optional <system>/dlna/servers.txt (one device description URL
 * per line, for networks where multicast discovery does not work). */
int  dlna_init(const char *system_dir);
void dlna_deinit(void);

/* Send a fresh SSDP search for media servers. */
void dlna_search(void);

/* The list of discovered servers.  dlna_servers_version() changes every
 * time a server is added; dlna_servers_list() returns a fresh array of
 * ENT_SERVER entries that the caller must free with dlna_entries_free(). */
unsigned     dlna_servers_version(void);
dlna_entry **dlna_servers_list(size_t *count);

/* Copies the friendly name of a server into buf. */
void dlna_server_name(int server, char *buf, size_t size);

/* The name to use for a server in a dlna:// path: the part of its name
 * before a ':' ("subaru" for "subaru: minidlna") when no other known server
 * shares it, otherwise the full name. */
void dlna_server_uri_name(int server, char *buf, size_t size);

/* The UI tells us how big thumbnails are drawn so that, for pictures, we
 * can pick the server-provided resolution closest to it. */
void dlna_set_thumb_hint(int px);

/* Requests.  Each returns a token (> 0) that identifies its results, or 0
 * on failure.
 *
 * dlna_browse(): list a container.  If container_id is NULL the root of
 * the given tree (MT_VIDEO / MT_AUDIO / MT_IMAGE) is located first.  When
 * 'tree' is not MT_NONE only media of that type (plus folders) is listed.
 *
 * dlna_resolve(): turn a "dlna://server/folder/.../file.ext" URI into a
 * starting folder (and optionally a media item to play). */
int  dlna_browse(int server, const char *container_id, media_type tree);
int  dlna_resolve(const char *uri);
void dlna_cancel(int token);

typedef enum dlna_result_kind
{
   RES_PROGRESS = 0,  /* browse in progress: count/total updated */
   RES_BROWSE,        /* browse finished: entries delivered      */
   RES_RESOLVE,       /* resolve finished                        */
   RES_ERROR          /* request failed: see 'error'             */
} dlna_result_kind;

typedef struct dlna_result
{
   int              token;
   dlna_result_kind kind;

   /* RES_PROGRESS / RES_BROWSE */
   dlna_entry     **entries;
   size_t           count;
   size_t           total;
   char            *container_id;  /* the container actually listed */

   /* RES_RESOLVE: server and top folder.  container_id NULL with
    * tree == MT_NONE means "show the Audio/Picture/Video chooser". */
   int              server;
   media_type       tree;
   char            *title;
   dlna_entry      *media;         /* item to start playing, or NULL */
   media_type       division;      /* what the folder mostly holds   */

   char            *error;
} dlna_result;

dlna_result *dlna_poll(void);
void         dlna_result_free(dlna_result *r);

#endif
