/*
 * dlna.c - UPnP discovery, ContentDirectory browsing and dlna:// paths.
 * See dlna.h for the overview.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <upnp.h>
#include <upnptools.h>
#include <ixml.h>

#include <rthreads/rthreads.h>

#include "dlna.h"
#include "util.h"

/* pupnp 1.18 removed the 'const' from the event argument of the client
 * callback.  UPNP_VERSION is (major * 100 + minor) * 100 + patch. */
#if defined(UPNP_VERSION) && UPNP_VERSION >= 11800
#define UPNP_EVENT_CONST
#else
#define UPNP_EVENT_CONST const
#endif

#define MAX_SERVERS       64
#define BROWSE_PAGE       200
#define BROWSE_MAX_ITEMS  100000
#define SEARCH_MX         3      /* seconds servers may wait to answer */
#define RESOLVE_WAIT_S    15     /* how long a dlna:// URI waits for its server */
#define BFS_MAX_DEPTH     3
#define BFS_MAX_BROWSES   48
#define MAX_SOAP_RESPONSE (16UL * 1024UL * 1024UL)

static const char *const MEDIA_SERVER_TARGET =
   "urn:schemas-upnp-org:device:MediaServer:1";

typedef struct server
{
   char *udn;
   char *name;
   char *location;
   char *base_url;
   char *control_url;
   char *service_type;
   char *icon_url;
   char *host;          /* "address:port" from the location URL */
   char *roots[MT_COUNT];
   int   can_search;    /* ContentDirectory::Search on titles:
                           0 = not asked yet, 1 = yes, -1 = no */
} server;

enum { REQ_BROWSE, REQ_RESOLVE };

typedef struct request
{
   struct request *next;
   int             token;
   int             kind;
   volatile int    cancelled;
   int             server;
   char           *container_id;
   media_type      tree;
   char           *uri;
} request;

typedef struct result_node
{
   struct result_node *next;
   dlna_result        *result;
} result_node;

static UpnpClient_Handle g_client = -1;
static int               g_upnp_ok;
static slock_t          *g_lock;
static scond_t          *g_cond;
static sthread_t        *g_thread;
/* Set when shutting down.  Read without the lock by pupnp's callback
 * threads as a cheap "stop early" hint; nothing depends on seeing it
 * immediately because dlna_deinit() joins every thread afterwards.  (C90
 * has no atomics; a volatile int is the traditional stand-in.  Thread
 * sanitizers will flag this as a race, which is expected.) */
static volatile int      g_quit;
static server           *g_servers[MAX_SERVERS];
static int               g_num_servers;
static unsigned          g_version;
static ptr_array         g_seen_locations;  /* char*, guarded by g_lock */
static request          *g_queue;
static request          *g_current;
static result_node      *g_results_head;
static result_node      *g_results_tail;
static int               g_next_token = 1;
static volatile int      g_thumb_hint = 256;
static char              g_system_dir[1024];

/* ------------------------------------------------------------------ */
/* Entries */

dlna_entry *dlna_entry_new(entry_kind kind)
{
   dlna_entry *e = (dlna_entry*)calloc(1, sizeof(*e));
   if (e)
   {
      e->kind     = kind;
      e->duration = -1.0;
      e->server   = -1;
   }
   return e;
}

dlna_entry *dlna_entry_copy(const dlna_entry *e)
{
   dlna_entry *c;
   if (!e)
      return NULL;
   c = dlna_entry_new(e->kind);
   if (!c)
      return NULL;
   *c           = *e;
   c->title     = util_strdup(e->title);
   c->id        = util_strdup(e->id);
   c->url       = util_strdup(e->url);
   c->thumb_url = util_strdup(e->thumb_url);
   c->parent_id = util_strdup(e->parent_id);
   return c;
}

void dlna_entry_free(dlna_entry *e)
{
   if (!e)
      return;
   free(e->title);
   free(e->id);
   free(e->url);
   free(e->thumb_url);
   free(e->parent_id);
   free(e);
}

void dlna_entries_free(dlna_entry **list, size_t count)
{
   size_t i;
   if (!list)
      return;
   for (i = 0; i < count; i++)
      dlna_entry_free(list[i]);
   free(list);
}

void dlna_result_free(dlna_result *r)
{
   if (!r)
      return;
   dlna_entries_free(r->entries, r->count);
   free(r->container_id);
   free(r->title);
   dlna_entry_free(r->media);
   free(r->error);
   free(r);
}

/* ------------------------------------------------------------------ */
/* Small XML helpers on top of ixml */

/* Element name without any namespace prefix ("dc:title" -> "title"). */
static const char *local_name(IXML_Node *n)
{
   const char *name = ixmlNode_getNodeName(n);
   const char *colon;
   if (!name)
      return "";
   colon = strrchr(name, ':');
   return colon ? colon + 1 : name;
}

static int is_element(IXML_Node *n, const char *name)
{
   return n && ixmlNode_getNodeType(n) == eELEMENT_NODE
      && strcmp(local_name(n), name) == 0;
}

/* First direct child element with the given local name. */
static IXML_Node *xml_child(IXML_Node *parent, const char *name)
{
   IXML_Node *n;
   if (!parent)
      return NULL;
   for (n = ixmlNode_getFirstChild(parent); n; n = ixmlNode_getNextSibling(n))
      if (is_element(n, name))
         return n;
   return NULL;
}

/* Depth first search for an element with the given local name. */
static IXML_Node *xml_find(IXML_Node *node, const char *name)
{
   IXML_Node *n;
   if (!node)
      return NULL;
   for (n = ixmlNode_getFirstChild(node); n; n = ixmlNode_getNextSibling(n))
   {
      IXML_Node *hit;
      if (is_element(n, name))
         return n;
      hit = xml_find(n, name);
      if (hit)
         return hit;
   }
   return NULL;
}

/* Text content of an element ("" when empty). */
static const char *xml_text(IXML_Node *el)
{
   IXML_Node *n;
   if (!el)
      return "";
   for (n = ixmlNode_getFirstChild(el); n; n = ixmlNode_getNextSibling(n))
   {
      unsigned short type = ixmlNode_getNodeType(n);
      if (type == eTEXT_NODE || type == eCDATA_SECTION_NODE)
      {
         const char *v = ixmlNode_getNodeValue(n);
         return v ? v : "";
      }
   }
   return "";
}

static const char *xml_attr(IXML_Node *el, const char *name)
{
   const char *v;
   if (!el)
      return NULL;
   v = ixmlElement_getAttribute((IXML_Element*)el, name);
   return (v && *v) ? v : NULL;
}

/* Text of a child element, trimmed and copied (NULL if missing/empty). */
static char *child_text_dup(IXML_Node *parent, const char *name)
{
   char *s = util_strdup(xml_text(xml_child(parent, name)));
   util_trim(s);
   if (s && !*s)
   {
      free(s);
      s = NULL;
   }
   return s;
}

/* ------------------------------------------------------------------ */
/* URLs */

static char *resolve_url(const char *base, const char *rel)
{
   char *abs_url = NULL;
   char *copy;

   if (!rel || !*rel)
      return NULL;
   if (strstr(rel, "://") || !base)
      return util_strdup(rel);
   if (UpnpResolveURL2(base, rel, &abs_url) != UPNP_E_SUCCESS || !abs_url)
      return util_strdup(rel);
   /* UpnpResolveURL2 allocates with malloc; copy so all our strings are
    * freed the same way. */
   copy = util_strdup(abs_url);
   free(abs_url);
   return copy;
}

static char *host_from_url(const char *url)
{
   const char *p = strstr(url, "://");
   size_t      len;
   if (!p)
      return NULL;
   p  += 3;
   len = strcspn(p, "/?#");
   return util_strndup(p, len);
}

/* ------------------------------------------------------------------ */
/* Results queue (worker -> main) */

static void post_result(dlna_result *r)
{
   result_node *n = (result_node*)calloc(1, sizeof(*n));
   if (!n)
   {
      dlna_result_free(r);
      return;
   }
   n->result = r;
   slock_lock(g_lock);
   if (g_results_tail)
      g_results_tail->next = n;
   else
      g_results_head = n;
   g_results_tail = n;
   slock_unlock(g_lock);
}

static void post_error(int token, const char *fmt, const char *arg)
{
   dlna_result *r = (dlna_result*)calloc(1, sizeof(*r));
   char         safe[384];
   char         buf[512];
   if (!r)
      return;
   r->token = token;
   r->kind  = RES_ERROR;
   /* fmt is always a short literal with one %s; clamp the argument so the
    * sprintf below cannot overflow (C90 has no snprintf). */
   strncpy(safe, arg ? arg : "", sizeof(safe) - 1);
   safe[sizeof(safe) - 1] = '\0';
   sprintf(buf, fmt, safe);
   r->error = util_strdup(buf);
   dlna_log(LOG_WARN, "%s\n", buf);
   post_result(r);
}

dlna_result *dlna_poll(void)
{
   result_node *n;
   dlna_result *r = NULL;

   if (!g_lock)
      return NULL;
   slock_lock(g_lock);
   n = g_results_head;
   if (n)
   {
      g_results_head = n->next;
      if (!g_results_head)
         g_results_tail = NULL;
   }
   slock_unlock(g_lock);
   if (n)
   {
      r = n->result;
      free(n);
   }
   return r;
}

/* ------------------------------------------------------------------ */
/* Servers */

static void server_free(server *s)
{
   int i;
   if (!s)
      return;
   free(s->udn);
   free(s->name);
   free(s->location);
   free(s->base_url);
   free(s->control_url);
   free(s->service_type);
   free(s->icon_url);
   free(s->host);
   for (i = 0; i < MT_COUNT; i++)
      free(s->roots[i]);
   free(s);
}

/* Servers are never removed while the core runs, so a pointer obtained
 * here stays valid until dlna_deinit(). */
static server *get_server(int index)
{
   server *s = NULL;
   slock_lock(g_lock);
   if (index >= 0 && index < g_num_servers)
      s = g_servers[index];
   slock_unlock(g_lock);
   return s;
}

static int has_content_directory(IXML_Node *device)
{
   IXML_Node *list = xml_child(device, "serviceList");
   IXML_Node *n;
   for (n = list ? ixmlNode_getFirstChild(list) : NULL; n;
         n = ixmlNode_getNextSibling(n))
      if (is_element(n, "service")
            && strstr(xml_text(xml_child(n, "serviceType")), "ContentDirectory"))
         return 1;
   return 0;
}

/* Find the MediaServer device in a description (it may be embedded in
 * another device's deviceList). */
static IXML_Node *find_media_device(IXML_Node *node, int require_type)
{
   IXML_Node *n;
   for (n = ixmlNode_getFirstChild(node); n; n = ixmlNode_getNextSibling(n))
   {
      IXML_Node *hit;
      if (is_element(n, "device") && has_content_directory(n)
            && (!require_type
               || strstr(xml_text(xml_child(n, "deviceType")), "MediaServer")))
         return n;
      hit = find_media_device(n, require_type);
      if (hit)
         return hit;
   }
   return NULL;
}

/* Pick the nicest icon: prefer PNG, then the largest up to 512 pixels. */
static char *pick_icon(IXML_Node *device, const char *base)
{
   IXML_Node  *list = xml_child(device, "iconList");
   IXML_Node  *n;
   const char *best = NULL;
   long        best_score = -1;

   for (n = list ? ixmlNode_getFirstChild(list) : NULL; n;
         n = ixmlNode_getNextSibling(n))
   {
      const char *url, *mime;
      long        width, score;
      if (!is_element(n, "icon"))
         continue;
      url = xml_text(xml_child(n, "url"));
      if (!*url)
         continue;
      mime  = xml_text(xml_child(n, "mimetype"));
      width = atol(xml_text(xml_child(n, "width")));
      score = width > 512 ? 512 - (width - 512) / 4 : width;
      if (util_strcasestr(mime, "png"))
         score += 1;
      if (score > best_score)
      {
         best_score = score;
         best       = url;
      }
   }
   return best ? resolve_url(base, best) : NULL;
}

static int location_seen(const char *location, int add)
{
   size_t i;
   for (i = 0; i < g_seen_locations.count; i++)
      if (strcmp((const char*)g_seen_locations.items[i], location) == 0)
         return 1;
   if (add)
      ptr_array_push(&g_seen_locations, util_strdup(location));
   return 0;
}

static void forget_location(const char *location)
{
   size_t i;
   for (i = 0; i < g_seen_locations.count; i++)
      if (strcmp((const char*)g_seen_locations.items[i], location) == 0)
      {
         free(g_seen_locations.items[i]);
         g_seen_locations.items[i] =
            g_seen_locations.items[--g_seen_locations.count];
         return;
      }
}

/* Download and parse a device description; add the server if it has a
 * ContentDirectory.  Safe to call from any thread. */
static void add_server_from_location(const char *location)
{
   IXML_Document *doc = NULL;
   IXML_Node     *root, *device, *services, *n;
   server        *s;
   const char    *url_base;
   int            i, rc;

   slock_lock(g_lock);
   if (location_seen(location, 1))
   {
      slock_unlock(g_lock);
      return;
   }
   slock_unlock(g_lock);

   rc = UpnpDownloadXmlDoc(location, &doc);
   if (rc != UPNP_E_SUCCESS || !doc)
   {
      dlna_log(LOG_WARN, "Could not fetch device description %s (%d)\n",
            location, rc);
      slock_lock(g_lock);
      forget_location(location); /* allow a retry on the next search */
      slock_unlock(g_lock);
      return;
   }

   root   = (IXML_Node*)doc;
   device = find_media_device(root, 1);
   if (!device)
      device = find_media_device(root, 0);
   if (!device)
   {
      ixmlDocument_free(doc);
      return;
   }

   s = (server*)calloc(1, sizeof(*s));
   if (!s)
   {
      ixmlDocument_free(doc);
      return;
   }
   url_base    = xml_text(xml_find(root, "URLBase"));
   s->location = util_strdup(location);
   s->base_url = util_strdup(*url_base ? url_base : location);
   s->udn      = child_text_dup(device, "UDN");
   s->name     = child_text_dup(device, "friendlyName");
   s->icon_url = pick_icon(device, s->base_url);
   s->host     = host_from_url(location);
   if (!s->name)
      s->name = util_strdup(s->host ? s->host : location);
   if (!s->udn)
      s->udn = util_strdup(location);

   services = xml_child(device, "serviceList");
   for (n = services ? ixmlNode_getFirstChild(services) : NULL; n;
         n = ixmlNode_getNextSibling(n))
   {
      const char *type;
      if (!is_element(n, "service"))
         continue;
      type = xml_text(xml_child(n, "serviceType"));
      if (strstr(type, "ContentDirectory"))
      {
         s->service_type = util_strdup(type);
         s->control_url  = resolve_url(s->base_url,
               xml_text(xml_child(n, "controlURL")));
         break;
      }
   }
   ixmlDocument_free(doc);

   if (!s->control_url || !s->service_type)
   {
      server_free(s);
      return;
   }

   slock_lock(g_lock);
   for (i = 0; i < g_num_servers; i++)
      if (strcmp(g_servers[i]->udn, s->udn) == 0)
         break;
   if (i < g_num_servers || g_num_servers >= MAX_SERVERS)
   {
      slock_unlock(g_lock);
      server_free(s);
      return;
   }
   g_servers[g_num_servers++] = s;
   g_version++;
   slock_unlock(g_lock);
   dlna_log(LOG_INFO, "Found media server \"%s\" at %s\n", s->name, location);
}

unsigned dlna_servers_version(void)
{
   unsigned v;
   if (!g_lock)
      return 0;
   slock_lock(g_lock);
   v = g_version;
   slock_unlock(g_lock);
   return v;
}

dlna_entry **dlna_servers_list(size_t *count)
{
   dlna_entry **list;
   int          i;

   *count = 0;
   if (!g_lock)
      return NULL;
   slock_lock(g_lock);
   list = (dlna_entry**)util_calloc((size_t)g_num_servers, sizeof(*list));
   if (list)
   {
      for (i = 0; i < g_num_servers; i++)
      {
         dlna_entry *e = dlna_entry_new(ENT_SERVER);
         if (!e)
            break;
         e->title     = util_strdup(g_servers[i]->name);
         e->id        = util_strdup(g_servers[i]->udn);
         e->thumb_url = util_strdup(g_servers[i]->icon_url);
         e->server    = i;
         e->order     = i;
         list[(*count)++] = e;
      }
   }
   slock_unlock(g_lock);
   return list;
}

void dlna_server_name(int index, char *buf, size_t size)
{
   server *s = get_server(index);
   buf[0] = '\0';
   if (s && size)
   {
      strncpy(buf, s->name, size - 1);
      buf[size - 1] = '\0';
   }
}

static int server_matches(const server *s, const char *name);

void dlna_server_uri_name(int index, char *buf, size_t size)
{
   char short_name[256];
   int  i, clash = 0;
   const char *colon;
   size_t len;

   buf[0] = '\0';
   slock_lock(g_lock);
   if (index >= 0 && index < g_num_servers && size)
   {
      const char *name = g_servers[index]->name;
      strncpy(buf, name, size - 1);
      buf[size - 1] = '\0';
      colon = strchr(name, ':');
      if (colon)
      {
         len = (size_t)(colon - name);
         if (len >= sizeof(short_name))
            len = sizeof(short_name) - 1;
         memcpy(short_name, name, len);
         short_name[len] = '\0';
         util_trim(short_name);
         for (i = 0; i < g_num_servers; i++)
            if (i != index && server_matches(g_servers[i], short_name))
               clash = 1;
         if (*short_name && !clash)
         {
            strncpy(buf, short_name, size - 1);
            buf[size - 1] = '\0';
         }
      }
   }
   slock_unlock(g_lock);
}

void dlna_set_thumb_hint(int px)
{
   g_thumb_hint = px > 0 ? px : 256;
}

/* Does a user supplied server name refer to this server?  We accept the
 * full friendly name, the part before a ':' ("subaru" for
 * "subaru: minidlna"), the host[:port] and the UDN. */
static int server_matches_name(const server *s, const char *name);

/* Like server_matches_name(), but "name:port" (e.g. "subaru:5080", from a
 * playlist that gives a port) also requires the server's port to match. */
static int server_matches(const server *s, const char *name)
{
   const char *colon = strrchr(name, ':');
   const char *sport;
   char        base[256];
   size_t      len;

   if (server_matches_name(s, name))
      return 1;
   if (!colon || !colon[1] || strspn(colon + 1, "0123456789") != strlen(colon + 1))
      return 0;
   len = (size_t)(colon - name);
   if (len == 0 || len >= sizeof(base))
      return 0;
   memcpy(base, name, len);
   base[len] = '\0';
   sport = s->host ? strrchr(s->host, ':') : NULL;
   if (!sport || strcmp(sport + 1, colon + 1) != 0)
      return 0;
   return server_matches_name(s, base);
}

static int server_matches_name(const server *s, const char *name)
{
   char        prefix[256];
   const char *colon;
   size_t      len;

   if (util_strcasecmp(s->name, name) == 0)
      return 1;
   colon = strchr(s->name, ':');
   if (colon)
   {
      len = (size_t)(colon - s->name);
      if (len >= sizeof(prefix))
         len = sizeof(prefix) - 1;
      memcpy(prefix, s->name, len);
      prefix[len] = '\0';
      util_trim(prefix);
      if (util_strcasecmp(prefix, name) == 0)
         return 1;
   }
   if (s->host)
   {
      if (util_strcasecmp(s->host, name) == 0)
         return 1;
      len = strcspn(s->host, ":");
      if (strlen(name) == len && util_strncasecmp(s->host, name, len) == 0)
         return 1;
   }
   if (util_strcasecmp(s->udn, name) == 0)
      return 1;
   if (util_starts_with_nocase(s->udn, "uuid:")
         && util_strcasecmp(s->udn + 5, name) == 0)
      return 1;
   return 0;
}

static int find_server_by_name(const char *name)
{
   int i, found = -1;
   slock_lock(g_lock);
   for (i = 0; i < g_num_servers; i++)
      if (server_matches(g_servers[i], name))
      {
         found = i;
         break;
      }
   slock_unlock(g_lock);
   return found;
}

/* ------------------------------------------------------------------ */
/* UPnP callback (runs on pupnp's threads) */

static int upnp_callback(Upnp_EventType type, UPNP_EVENT_CONST void *event,
      void *cookie)
{
   const UpnpDiscovery *d;
   const char          *location;
   const char          *dtype;
   const char          *stype;

   (void)cookie;
   if (g_quit)
      return 0;
   if (type != UPNP_DISCOVERY_SEARCH_RESULT
         && type != UPNP_DISCOVERY_ADVERTISEMENT_ALIVE)
      return 0;

   d = (const UpnpDiscovery*)event;
   if (UpnpDiscovery_get_ErrCode(d) != UPNP_E_SUCCESS)
      return 0;
   location = UpnpDiscovery_get_Location_cstr(d);
   dtype    = UpnpDiscovery_get_DeviceType_cstr(d);
   stype    = UpnpDiscovery_get_ServiceType_cstr(d);
   if (!location || !*location)
      return 0;
   /* Advertisements arrive for every device and service; only follow the
    * ones that look like media servers. */
   if (type == UPNP_DISCOVERY_ADVERTISEMENT_ALIVE
         && !(dtype && strstr(dtype, "MediaServer"))
         && !(stype && strstr(stype, "ContentDirectory")))
      return 0;
   add_server_from_location(location);
   return 0;
}

void dlna_search(void)
{
   if (!g_upnp_ok)
      return;
   UpnpSearchAsync(g_client, SEARCH_MX, MEDIA_SERVER_TARGET, NULL);
}

/* ------------------------------------------------------------------ */
/* ContentDirectory::Browse */

/* One page of ContentDirectory::Browse (criteria == NULL: list the
 * children of object_id) or ContentDirectory::Search (search below
 * object_id for objects matching criteria). */
static int cd_page(server *srv, const char *object_id, const char *criteria,
      long start, long count, char **didl, long *returned, long *total)
{
   IXML_Document *action = NULL;
   IXML_Document *resp   = NULL;
   char           sbuf[32], cbuf[32];
   const char    *st = srv->service_type;
   int            rc;

   *didl     = NULL;
   *returned = 0;
   *total    = 0;
   sprintf(sbuf, "%ld", start);
   sprintf(cbuf, "%ld", count);

   /* Argument order matters to some servers: it must match the SCPD. */
   if (criteria)
   {
      action = UpnpMakeAction("Search", st, 0, NULL);
      UpnpAddToAction(&action, "Search", st, "ContainerID", object_id);
      UpnpAddToAction(&action, "Search", st, "SearchCriteria", criteria);
      UpnpAddToAction(&action, "Search", st, "Filter", "*");
      UpnpAddToAction(&action, "Search", st, "StartingIndex", sbuf);
      UpnpAddToAction(&action, "Search", st, "RequestedCount", cbuf);
      UpnpAddToAction(&action, "Search", st, "SortCriteria", "");
   }
   else
   {
      action = UpnpMakeAction("Browse", st, 0, NULL);
      UpnpAddToAction(&action, "Browse", st, "ObjectID", object_id);
      UpnpAddToAction(&action, "Browse", st, "BrowseFlag", "BrowseDirectChildren");
      UpnpAddToAction(&action, "Browse", st, "Filter", "*");
      UpnpAddToAction(&action, "Browse", st, "StartingIndex", sbuf);
      UpnpAddToAction(&action, "Browse", st, "RequestedCount", cbuf);
      UpnpAddToAction(&action, "Browse", st, "SortCriteria", "");
   }

   rc = UpnpSendAction(g_client, srv->control_url, st, NULL, action, &resp);
   ixmlDocument_free(action);
   if (rc != UPNP_E_SUCCESS)
   {
      if (resp)
         ixmlDocument_free(resp);
      return rc;
   }
   if (!resp)
      return UPNP_E_BAD_RESPONSE;

   *didl     = util_strdup(xml_text(xml_find((IXML_Node*)resp, "Result")));
   *returned = atol(xml_text(xml_find((IXML_Node*)resp, "NumberReturned")));
   *total    = atol(xml_text(xml_find((IXML_Node*)resp, "TotalMatches")));
   ixmlDocument_free(resp);
   return UPNP_E_SUCCESS;
}

/* One <res> element. */
typedef struct res_info
{
   const char *url;
   char        mime[64];
   const char *extra;     /* 4th protocolInfo field (DLNA flags) */
   int         width, height;
   double      duration;
   int         converted; /* DLNA.ORG_CI=1: transcoded by the server */
   int         thumbnail; /* DLNA profile ends in _TN */
} res_info;

#define MAX_RES 16

static int parse_res(IXML_Node *el, res_info *r)
{
   const char *pi = xml_attr(el, "protocolInfo");
   const char *p;
   const char *res;
   int         field = 0;
   size_t      len;

   memset(r, 0, sizeof(*r));
   r->duration = -1.0;
   r->url      = xml_text(el);
   while (*r->url == ' ' || *r->url == '\n' || *r->url == '\t')
      r->url++;
   if (!*r->url)
      return 0;

   /* protocolInfo = protocol:network:contentFormat:additionalInfo */
   p = pi ? pi : "";
   while (*p && field < 3)
   {
      len = strcspn(p, ":");
      if (field == 0 && len && util_strncasecmp(p, "http-get", len) != 0)
         return 0; /* rtsp, internal, ... we can only do HTTP */
      if (field == 2)
      {
         if (len >= sizeof(r->mime))
            len = sizeof(r->mime) - 1;
         memcpy(r->mime, p, len);
         r->mime[len] = '\0';
      }
      p += len;
      if (*p == ':')
         p++;
      field++;
   }
   r->extra     = p;
   r->converted = strstr(r->extra, "DLNA.ORG_CI=1") != NULL;
   r->thumbnail = strstr(r->extra, "_TN") != NULL;

   res = xml_attr(el, "resolution");
   if (res)
      util_parse_resolution(res, &r->width, &r->height);
   if (xml_attr(el, "duration"))
      r->duration = util_parse_duration(xml_attr(el, "duration"));
   return 1;
}

static media_type res_type(const res_info *r)
{
   media_type t = util_media_type_from_mime(r->mime);
   if (t == MT_NONE)
   {
      char ext[16];
      t = util_media_type_from_extension(util_extension(r->url, ext, sizeof(ext)));
   }
   return t;
}

/* Size used to compare image resources: the longer side, 160 for DLNA
 * thumbnails without a resolution, "huge" for unknown originals. */
static long res_size(const res_info *r)
{
   if (r->width > 0 && r->height > 0)
      return r->width > r->height ? r->width : r->height;
   return r->thumbnail ? 160 : 100000;
}

static dlna_entry *parse_item(IXML_Node *el, server *srv, int server_index)
{
   res_info    res[MAX_RES];
   int         nres = 0;
   IXML_Node  *n;
   dlna_entry *e;
   const char *cls;
   const char *art;
   media_type  type = MT_NONE;
   int         main_res = -1;
   int         i;

   for (n = ixmlNode_getFirstChild(el); n && nres < MAX_RES;
         n = ixmlNode_getNextSibling(n))
      if (is_element(n, "res") && parse_res(n, &res[nres]))
         nres++;
   if (!nres)
      return NULL;

   /* 1. What kind of item is it? */
   cls = xml_text(xml_child(el, "class"));
   if (util_starts_with_nocase(cls, "object.item.videoItem"))
      type = MT_VIDEO;
   else if (util_starts_with_nocase(cls, "object.item.audioItem"))
      type = MT_AUDIO;
   else if (util_starts_with_nocase(cls, "object.item.imageItem"))
      type = MT_IMAGE;
   for (i = 0; type == MT_NONE && i < nres; i++)
      if (!res[i].thumbnail)
         type = res_type(&res[i]);
   if (type == MT_NONE)
      return NULL;

   /* 2. Which resource is the media itself? */
   if (type == MT_IMAGE)
   {
      /* The original (not converted) file, largest first. */
      long best = -1;
      for (i = 0; i < nres; i++)
      {
         long score;
         if (res_type(&res[i]) != MT_IMAGE)
            continue;
         score = res_size(&res[i]) + (res[i].converted ? 0 : 1000000L);
         if (res[i].thumbnail)
            score -= 2000000L;
         if (score > best)
         {
            best     = score;
            main_res = i;
         }
      }
   }
   else
   {
      for (i = 0; i < nres && main_res < 0; i++)
         if (res_type(&res[i]) == type && !res[i].converted)
            main_res = i;
      for (i = 0; i < nres && main_res < 0; i++)
         if (res_type(&res[i]) == type)
            main_res = i;
      for (i = 0; i < nres && main_res < 0; i++)
         if (res_type(&res[i]) != MT_IMAGE)
            main_res = i;
   }
   if (main_res < 0)
      main_res = 0;

   e = dlna_entry_new(ENT_MEDIA);
   if (!e)
      return NULL;
   e->media    = type;
   e->server   = server_index;
   e->id       = util_strdup(xml_attr(el, "id"));
   e->parent_id = util_strdup(xml_attr(el, "parentID"));
   e->title    = child_text_dup(el, "title");
   e->url      = resolve_url(srv->base_url, res[main_res].url);
   e->duration = res[main_res].duration;

   /* 3. Thumbnail. */
   art = xml_text(xml_child(el, "albumArtURI"));
   if (*art)
      e->thumb_url = resolve_url(srv->base_url, art);
   else if (type == MT_IMAGE)
   {
      /* Smallest server-made version at least as big as we draw it,
       * otherwise the biggest smaller one, otherwise the image itself. */
      int  above = -1, below = -1;
      long hint  = g_thumb_hint;
      for (i = 0; i < nres; i++)
      {
         long sz;
         if (res_type(&res[i]) != MT_IMAGE || i == main_res)
            continue;
         sz = res_size(&res[i]);
         if (sz >= hint)
         {
            if (above < 0 || sz < res_size(&res[above]))
               above = i;
         }
         else if (below < 0 || sz > res_size(&res[below]))
            below = i;
      }
      if (above >= 0)
         e->thumb_url = resolve_url(srv->base_url, res[above].url);
      else if (below >= 0)
         e->thumb_url = resolve_url(srv->base_url, res[below].url);
      else
         e->thumb_url = util_strdup(e->url);
   }
   else
   {
      for (i = 0; i < nres; i++)
         if (i != main_res && util_media_type_from_mime(res[i].mime) == MT_IMAGE)
         {
            e->thumb_url = resolve_url(srv->base_url, res[i].url);
            break;
         }
   }

   if (!e->title)
   {
      /* Fall back to the file name. */
      const char *slash = strrchr(e->url, '/');
      e->title = util_strdup(slash ? slash + 1 : e->url);
   }
   return e;
}

static dlna_entry *parse_container(IXML_Node *el, server *srv, int server_index)
{
   dlna_entry *e = dlna_entry_new(ENT_FOLDER);
   const char *art;
   if (!e)
      return NULL;
   e->server = server_index;
   e->id     = util_strdup(xml_attr(el, "id"));
   e->parent_id = util_strdup(xml_attr(el, "parentID"));
   e->title  = child_text_dup(el, "title");
   art       = xml_text(xml_child(el, "albumArtURI"));
   if (*art)
      e->thumb_url = resolve_url(srv->base_url, art);
   if (!e->id)
   {
      dlna_entry_free(e);
      return NULL;
   }
   if (!e->title)
      e->title = util_strdup(e->id);
   return e;
}

/* Parse a DIDL-Lite document, appending entries to 'out'. */
static void parse_didl(const char *didl, server *srv, int server_index,
      media_type filter, ptr_array *out, long *order)
{
   IXML_Document *doc;
   IXML_Node     *root, *n;

   if (!didl || !*didl)
      return;
   doc = ixmlParseBuffer(didl);
   if (!doc)
   {
      dlna_log(LOG_WARN, "Could not parse DIDL-Lite from %s\n", srv->name);
      return;
   }
   root = xml_find((IXML_Node*)doc, "DIDL-Lite");
   for (n = root ? ixmlNode_getFirstChild(root) : NULL; n;
         n = ixmlNode_getNextSibling(n))
   {
      dlna_entry *e = NULL;
      if (is_element(n, "container"))
         e = parse_container(n, srv, server_index);
      else if (is_element(n, "item"))
      {
         e = parse_item(n, srv, server_index);
         if (e && filter != MT_NONE && e->media != filter)
         {
            dlna_entry_free(e);
            e = NULL;
         }
      }
      if (e)
      {
         e->order = (*order)++;
         if (!ptr_array_push(out, e))
            dlna_entry_free(e);
      }
   }
   ixmlDocument_free(doc);
}

/* Browse all children of a container, following paging.  Returns 0 on
 * success or a pupnp error code.  Progress results are posted when
 * 'progress_token' is non-zero. */
static int cd_all(server *srv, int server_index, const char *object_id,
      const char *criteria, long max_items, media_type filter, request *req,
      int progress_token, ptr_array *out)
{
   long start = 0;
   long order = 0;

   for (;;)
   {
      char *didl;
      long  returned, total;
      int   rc;

      if (g_quit || (req && req->cancelled))
         return UPNP_E_CANCELED;
      rc = cd_page(srv, object_id, criteria, start, BROWSE_PAGE, &didl,
            &returned, &total);
      if (rc != UPNP_E_SUCCESS)
         return rc;
      parse_didl(didl, srv, server_index, filter, out, &order);
      free(didl);

      start += returned;
      if (returned <= 0 || (total > 0 && start >= total)
            || start >= max_items)
         break;
      if (progress_token)
      {
         dlna_result *r = (dlna_result*)calloc(1, sizeof(*r));
         if (r)
         {
            r->token = progress_token;
            r->kind  = RES_PROGRESS;
            r->count = (size_t)start;
            r->total = total > 0 ? (size_t)total : 0;
            post_result(r);
         }
      }
   }
   return UPNP_E_SUCCESS;
}

static int browse_all(server *srv, int server_index, const char *object_id,
      media_type filter, request *req, int progress_token, ptr_array *out)
{
   return cd_all(srv, server_index, object_id, NULL, BROWSE_MAX_ITEMS, filter,
         req, progress_token, out);
}

static void free_ptr_entries(ptr_array *a)
{
   size_t i;
   for (i = 0; i < a->count; i++)
      dlna_entry_free((dlna_entry*)a->items[i]);
   ptr_array_clear(a);
}

/* ------------------------------------------------------------------ */
/* Locating the Video / Music / Pictures trees */

static int title_is_tree(const char *title, media_type tree, int exact)
{
   static const char *const video[] = { "video", "videos", "movies", "movie",
      "films", "film", NULL };
   static const char *const audio[] = { "music", "audio", NULL };
   static const char *const image[] = { "pictures", "picture", "photos",
      "photo", "images", "image", NULL };
   const char *const *list;

   switch (tree)
   {
      case MT_VIDEO: list = video; break;
      case MT_AUDIO: list = audio; break;
      case MT_IMAGE: list = image; break;
      default:       return 0;
   }
   for (; *list; list++)
   {
      if (exact ? util_strcasecmp(title, *list) == 0
                : util_strcasestr(title, *list) != NULL)
         return 1;
   }
   return 0;
}

/* Returns a malloc'ed object id for the root of a tree.  Servers do not
 * mark these in any standard way, so we look for a top level container
 * with a recognisable name ("Video", "Music", "Pictures", ...) and fall
 * back to the server root (with type filtering) when there is none. */
static char *find_tree_root(server *srv, int server_index, media_type tree,
      request *req)
{
   ptr_array   kids;
   char       *found = NULL;
   size_t      i;
   int         pass;

   slock_lock(g_lock);
   if (srv->roots[tree])
      found = util_strdup(srv->roots[tree]);
   slock_unlock(g_lock);
   if (found)
      return found;

   memset(&kids, 0, sizeof(kids));
   if (browse_all(srv, server_index, "0", MT_NONE, req, 0, &kids)
         == UPNP_E_SUCCESS)
   {
      for (pass = 0; pass < 2 && !found; pass++)
         for (i = 0; i < kids.count && !found; i++)
         {
            dlna_entry *e = (dlna_entry*)kids.items[i];
            if (e->kind == ENT_FOLDER && title_is_tree(e->title, tree, pass == 0))
               found = util_strdup(e->id);
         }
   }
   free_ptr_entries(&kids);
   if (!found)
      found = util_strdup("0");

   slock_lock(g_lock);
   util_replace(&srv->roots[tree], found);
   slock_unlock(g_lock);
   return found;
}

/* ------------------------------------------------------------------ */
/* Request processing */

static void process_browse(request *req)
{
   server      *srv = get_server(req->server);
   ptr_array    out;
   char        *cid;
   dlna_result *r;
   int          rc;

   if (!srv)
   {
      post_error(req->token, "Unknown media server%s", "");
      return;
   }
   if (req->container_id)
      cid = util_strdup(req->container_id);
   else
      cid = find_tree_root(srv, req->server, req->tree, req);

   memset(&out, 0, sizeof(out));
   rc = browse_all(srv, req->server, cid, req->tree, req, req->token, &out);
   if (rc != UPNP_E_SUCCESS)
   {
      free_ptr_entries(&out);
      free(cid);
      if (rc != UPNP_E_CANCELED)
         post_error(req->token, "Browsing failed: %s", UpnpGetErrorMessage(rc));
      return;
   }

   r = (dlna_result*)calloc(1, sizeof(*r));
   if (!r)
   {
      free_ptr_entries(&out);
      free(cid);
      return;
   }
   r->token        = req->token;
   r->kind         = RES_BROWSE;
   r->entries      = (dlna_entry**)out.items; /* hand over the array */
   r->count        = out.count;
   r->total        = out.count;
   r->container_id = cid;
   r->tree         = req->tree;
   r->server       = req->server;
   post_result(r);
}

/* Does an item match the last component of a dlna:// path?  Accepts the
 * title as is, the title plus the resource's extension, and the file
 * name at the end of the resource URL. */
static int item_matches(const dlna_entry *e, const char *name)
{
   char        stem[512];
   const char *dot;
   const char *slash;
   size_t      len;

   if (util_strcasecmp(e->title, name) == 0)
      return 1;
   dot = strrchr(name, '.');
   if (dot)
   {
      len = (size_t)(dot - name);
      if (len < sizeof(stem))
      {
         memcpy(stem, name, len);
         stem[len] = '\0';
         if (util_strcasecmp(e->title, stem) == 0)
            return 1;
      }
   }
   if (e->url)
   {
      slash = strrchr(e->url, '/');
      if (slash)
      {
         len = strcspn(slash + 1, "?#");
         if (strlen(name) == len && util_strncasecmp(slash + 1, name, len) == 0)
            return 1;
      }
   }
   return 0;
}

/* Wait (re-searching now and then) until a server with this name shows
 * up.  Returns its index or -1. */
static int wait_for_server(const char *name, request *req)
{
   time_t start = time(NULL);
   time_t last_search = start;
   int    index;

   while ((index = find_server_by_name(name)) < 0)
   {
      time_t now = time(NULL);
      if (g_quit || req->cancelled || now - start >= RESOLVE_WAIT_S)
         return -1;
      if (now - last_search >= 3)
      {
         dlna_search();
         last_search = now;
      }
      slock_lock(g_lock);
      scond_wait_timeout(g_cond, g_lock, 250000);
      slock_unlock(g_lock);
   }
   return index;
}

/* ---- dlna:// resolution -------------------------------------------
 *
 * A path is resolved against the WHOLE server, not just its Video, Music
 * and Pictures trees: servers often show a folder in only some of their
 * views (MiniDLNA, for instance, may list a folder under "Browse Folders"
 * but not under "Video").
 *
 *  1. Find every container called like the first component, anywhere on
 *     the server: with ContentDirectory::Search when the server supports
 *     searching titles, otherwise by browsing breadth first from the root.
 *     Containers directly under the root count too, so the server's own
 *     "Video" / "Music" / "Pictures" trees can be named in a path.
 *  2. From each of those, follow the remaining components as direct
 *     children.  The last one may be a media item.
 *  3. If several candidates survive (the same folder name usually exists
 *     once per media type), pick the one whose content is of the preferred
 *     type: the file extension's type if there is one, otherwise video,
 *     then audio, then pictures.  Ties go to containers directly under the
 *     root, then to the order the server returned them in.
 */

#define MAX_CANDIDATES    32
#define SEARCH_MAX_ITEMS  500
#define BFS_ALL_DEPTH     4
#define BFS_ALL_BROWSES   200
#define SNIFF_BROWSES     8

typedef struct candidate
{
   dlna_entry *folder;     /* the folder to show (top folder)        */
   dlna_entry *media;      /* the item to play, or NULL              */
   int         root_level; /* found directly under the server root   */
   media_type  type;       /* what it contains (filled in by ranking) */
} candidate;

/* Is ContentDirectory::Search usable for titles on this server? */
static int server_can_search(server *srv)
{
   IXML_Document *action, *resp = NULL;
   const char    *caps;
   int            rc, ok = 0;

   if (srv->can_search)
      return srv->can_search > 0;
   action = UpnpMakeAction("GetSearchCapabilities", srv->service_type, 0, NULL);
   rc     = UpnpSendAction(g_client, srv->control_url, srv->service_type, NULL,
         action, &resp);
   ixmlDocument_free(action);
   if (rc == UPNP_E_SUCCESS && resp)
   {
      caps = xml_text(xml_find((IXML_Node*)resp, "SearchCaps"));
      ok   = strstr(caps, "dc:title") != NULL || strcmp(caps, "*") == 0;
   }
   if (resp)
      ixmlDocument_free(resp);
   srv->can_search = ok ? 1 : -1;
   dlna_log(LOG_INFO, "%s %s title searches\n", srv->name,
         ok ? "supports" : "does not support");
   return ok;
}

/* Does an entry match a path component?  Containers match by title; items
 * also match with the extension, or by the file name in their URL. */
static int entry_matches(const dlna_entry *e, const char *name, int want_item)
{
   if (want_item)
      return e->kind == ENT_MEDIA && item_matches(e, name);
   return e->kind == ENT_FOLDER && util_strcasecmp(e->title, name) == 0;
}

/* Append a copy of e unless an entry with the same id is already there. */
static void add_unique(ptr_array *out, const dlna_entry *e)
{
   size_t i;
   if (out->count >= MAX_CANDIDATES || !e->id)
      return;
   for (i = 0; i < out->count; i++)
      if (strcmp(((dlna_entry*)out->items[i])->id, e->id) == 0)
         return;
   ptr_array_push(out, dlna_entry_copy(e));
}

/* Escape a string for use inside "..." in a UPnP search criteria. */
static void criteria_quote(char *out, size_t size, const char *s)
{
   size_t n = 0;
   while (*s && n + 3 < size)
   {
      if (*s == '"' || *s == '\\')
         out[n++] = '\\';
      out[n++] = *s++;
   }
   out[n] = '\0';
}

/* Use ContentDirectory::Search to find every container (or item) called
 * 'name'.  Returns 0 if searching failed (the caller then browses). */
static int search_all(server *srv, int si, const char *name, int want_item,
      request *req, ptr_array *out)
{
   char       quoted[600], criteria[800], stem[512];
   const char *dot;
   ptr_array  hits;
   size_t     i;
   int        rc;

   /* Item titles usually lack the extension, so search for the stem. */
   strncpy(stem, name, sizeof(stem) - 1);
   stem[sizeof(stem) - 1] = '\0';
   dot = strrchr(stem, '.');
   if (want_item && dot && dot != stem)
      stem[dot - stem] = '\0';
   criteria_quote(quoted, sizeof(quoted), stem);
   /* "contains" rather than "=": servers differ in case sensitivity, and
    * we check for an exact (case-insensitive) match below anyway. */
   sprintf(criteria, "upnp:class derivedfrom \"%s\" and dc:title contains \"%s\"",
         want_item ? "object.item" : "object.container", quoted);

   memset(&hits, 0, sizeof(hits));
   rc = cd_all(srv, si, "0", criteria, SEARCH_MAX_ITEMS, MT_NONE, req, 0, &hits);
   if (rc != UPNP_E_SUCCESS)
   {
      dlna_log(LOG_WARN, "Search on %s failed (%s); browsing instead\n",
            srv->name, UpnpGetErrorMessage(rc));
      srv->can_search = -1;
      free_ptr_entries(&hits);
      return 0;
   }
   for (i = 0; i < hits.count; i++)
      if (entry_matches((dlna_entry*)hits.items[i], name, want_item))
         add_unique(out, (dlna_entry*)hits.items[i]);
   free_ptr_entries(&hits);
   return 1;
}

/* Browse breadth first from the server root collecting every match.  Once
 * a match turns up, the rest of that depth is finished and the search
 * stops, so the shallowest matches win. */
static void bfs_all(server *srv, int si, const char *name, int want_item,
      request *req, ptr_array *out)
{
   ptr_array queue;   /* char* object ids         */
   ptr_array depths;  /* their depth, as integers */
   size_t    head = 0, i;
   int       browses = 0, found_depth = -1;

   memset(&queue, 0, sizeof(queue));
   memset(&depths, 0, sizeof(depths));
   ptr_array_push(&queue, util_strdup("0"));
   ptr_array_push(&depths, (void*)0);

   while (head < queue.count && browses < BFS_ALL_BROWSES
         && !(req && req->cancelled))
   {
      const char *id = (const char*)queue.items[head];
      /* Depths are small integers smuggled through a void pointer. */
      int         depth = (int)(size_t)depths.items[head];
      ptr_array   kids;

      head++;
      if (found_depth >= 0 && depth > found_depth)
         break;
      memset(&kids, 0, sizeof(kids));
      browses++;
      if (browse_all(srv, si, id, MT_NONE, req, 0, &kids) == UPNP_E_SUCCESS)
         for (i = 0; i < kids.count; i++)
         {
            dlna_entry *e = (dlna_entry*)kids.items[i];
            if (entry_matches(e, name, want_item))
            {
               add_unique(out, e);
               found_depth = depth;
            }
            else if (e->kind == ENT_FOLDER && depth + 1 < BFS_ALL_DEPTH
                  && found_depth < 0)
            {
               ptr_array_push(&queue, util_strdup(e->id));
               ptr_array_push(&depths, (void*)(size_t)(depth + 1));
            }
         }
      free_ptr_entries(&kids);
   }
   for (i = 0; i < queue.count; i++)
      free(queue.items[i]);
   ptr_array_clear(&queue);
   ptr_array_clear(&depths);
}

/* Find a direct child of 'parent' called 'name'. */
static dlna_entry *find_child(server *srv, int si, const char *parent,
      const char *name, int want_item, request *req)
{
   ptr_array   kids;
   dlna_entry *hit = NULL;
   size_t      i;

   memset(&kids, 0, sizeof(kids));
   if (browse_all(srv, si, parent, MT_NONE, req, 0, &kids) == UPNP_E_SUCCESS)
      for (i = 0; i < kids.count && !hit; i++)
         if (entry_matches((dlna_entry*)kids.items[i], name, want_item))
            hit = dlna_entry_copy((dlna_entry*)kids.items[i]);
   free_ptr_entries(&kids);
   return hit;
}

/* Every object called 'name' anywhere on the server, best guesses first:
 * matches directly under the root, then search/browse results. */
static void find_everywhere(server *srv, int si, const char *name,
      int want_item, request *req, ptr_array *out, size_t *root_count)
{
   ptr_array kids;
   size_t    i;

   memset(&kids, 0, sizeof(kids));
   if (browse_all(srv, si, "0", MT_NONE, req, 0, &kids) == UPNP_E_SUCCESS)
      for (i = 0; i < kids.count; i++)
         if (entry_matches((dlna_entry*)kids.items[i], name, want_item))
            add_unique(out, (dlna_entry*)kids.items[i]);
   free_ptr_entries(&kids);
   *root_count = out->count;

   if (!(server_can_search(srv) && search_all(srv, si, name, want_item, req, out)))
      bfs_all(srv, si, name, want_item, req, out);
}

/* What kind of media does a folder hold?  Looks at its items, and if it
 * only has subfolders, at a few of those (a handful of browses at most).
 * Video beats audio beats pictures. */
static media_type sniff_folder(server *srv, int si, const char *id, request *req)
{
   ptr_array  queue;
   size_t     head = 0, i;
   int        browses = 0;
   media_type found = MT_NONE;

   memset(&queue, 0, sizeof(queue));
   ptr_array_push(&queue, util_strdup(id));
   while (head < queue.count && browses < SNIFF_BROWSES && found != MT_VIDEO)
   {
      ptr_array kids;
      int       subfolders = 0;

      memset(&kids, 0, sizeof(kids));
      browses++;
      /* Only the first page: enough to see what kind of folder it is,
       * and cheap even for folders with tens of thousands of entries. */
      if (cd_all(srv, si, (const char*)queue.items[head++], NULL,
               BROWSE_PAGE, MT_NONE, req, 0, &kids) == UPNP_E_SUCCESS)
         for (i = 0; i < kids.count; i++)
         {
            dlna_entry *e = (dlna_entry*)kids.items[i];
            if (e->kind == ENT_MEDIA)
            {
               if (found == MT_NONE || e->media < found)
                  found = e->media; /* MT_VIDEO < MT_AUDIO < MT_IMAGE */
            }
            else if (e->kind == ENT_FOLDER && subfolders < 3)
            {
               ptr_array_push(&queue, util_strdup(e->id));
               subfolders++;
            }
         }
      free_ptr_entries(&kids);
   }
   for (i = 0; i < queue.count; i++)
      free(queue.items[i]);
   ptr_array_clear(&queue);
   return found;
}

static int type_rank(media_type t, media_type preferred)
{
   if (t != MT_NONE && t == preferred)
      return 0;
   switch (t)
   {
      case MT_VIDEO: return 1;
      case MT_AUDIO: return 2;
      case MT_IMAGE: return 3;
      default:       break;
   }
   return 4;
}

static void free_candidates(candidate *c, int n)
{
   int i;
   for (i = 0; i < n; i++)
   {
      dlna_entry_free(c[i].folder);
      dlna_entry_free(c[i].media);
   }
}

static void process_resolve(request *req)
{
   char        *copy = NULL;
   char        *p;
   char        *comps[64];
   int          ncomps = 0;
   char        *server_name;
   int          si, i, j, nfold, ncand = 0, best;
   server      *srv;
   char         ext[16];
   media_type   guess;
   candidate    cand[MAX_CANDIDATES];
   ptr_array    firsts;
   size_t       root_count = 0, k;
   dlna_result *r;

   /* Split "dlna://server/a/b/c" into the server name and components. */
   p = req->uri;
   if (util_starts_with_nocase(p, "dlna://"))
      p += 7;
   copy = util_strdup(p);
   if (!copy)
      return;
   server_name = copy;
   p           = strchr(copy, '/');
   if (p)
      *p++ = '\0';
   while (p && *p && ncomps < 64)
   {
      char *slash = strchr(p, '/');
      if (slash)
         *slash = '\0';
      if (*p)
      {
         util_percent_decode(p);
         comps[ncomps++] = p;
      }
      p = slash ? slash + 1 : NULL;
   }
   util_percent_decode(server_name);

   if (!*server_name)
   {
      post_error(req->token, "No server name in %s", req->uri);
      free(copy);
      return;
   }

   si = wait_for_server(server_name, req);
   if (si < 0)
   {
      if (!req->cancelled)
         post_error(req->token, "Media server \"%s\" was not found", server_name);
      free(copy);
      return;
   }
   srv = get_server(si);

   r = (dlna_result*)calloc(1, sizeof(*r));
   if (!r)
   {
      free(copy);
      return;
   }
   r->token  = req->token;
   r->kind   = RES_RESOLVE;
   r->server = si;
   r->tree   = MT_NONE;

   if (ncomps == 0)
   {
      /* Only a server: show the Audio / Picture / Video chooser. */
      r->title = util_strdup(srv->name);
      post_result(r);
      free(copy);
      return;
   }

   /* A known media extension on the last component means "a file". */
   guess = util_media_type_from_extension(
         util_extension(comps[ncomps - 1], ext, sizeof(ext)));
   nfold = guess != MT_NONE ? ncomps - 1 : ncomps;
   memset(cand, 0, sizeof(cand));
   memset(&firsts, 0, sizeof(firsts));

   if (nfold == 0)
   {
      /* Just a file name: find the item anywhere; its folder is the top. */
      find_everywhere(srv, si, comps[0], 1, req, &firsts, &root_count);
      for (k = 0; k < firsts.count && ncand < MAX_CANDIDATES; k++)
      {
         dlna_entry *m = (dlna_entry*)firsts.items[k];
         cand[ncand].media  = dlna_entry_copy(m);
         cand[ncand].folder = dlna_entry_new(ENT_FOLDER);
         if (cand[ncand].folder)
         {
            cand[ncand].folder->id     = util_strdup(m->parent_id ? m->parent_id : "0");
            cand[ncand].folder->title  = util_strdup(srv->name);
            cand[ncand].folder->server = si;
         }
         cand[ncand].root_level = k < root_count;
         ncand++;
      }
   }
   else
   {
      find_everywhere(srv, si, comps[0], 0, req, &firsts, &root_count);
      /* No folder of that name at all, and it may be a file whose title
       * just has no extension (e.g. dlna://server/Some Title). */
      if (!firsts.count && ncomps == 1)
      {
         find_everywhere(srv, si, comps[0], 1, req, &firsts, &root_count);
         for (k = 0; k < firsts.count && ncand < MAX_CANDIDATES; k++)
         {
            dlna_entry *m = (dlna_entry*)firsts.items[k];
            cand[ncand].media  = dlna_entry_copy(m);
            cand[ncand].folder = dlna_entry_new(ENT_FOLDER);
            if (cand[ncand].folder)
            {
               cand[ncand].folder->id     = util_strdup(m->parent_id ? m->parent_id : "0");
               cand[ncand].folder->title  = util_strdup(srv->name);
               cand[ncand].folder->server = si;
            }
            ncand++;
         }
         free_ptr_entries(&firsts);
         memset(&firsts, 0, sizeof(firsts));
      }

      for (k = 0; k < firsts.count && ncand < MAX_CANDIDATES && !req->cancelled; k++)
      {
         dlna_entry *cur   = dlna_entry_copy((dlna_entry*)firsts.items[k]);
         dlna_entry *media = NULL;

         for (i = 1; cur && i < nfold; i++)
         {
            dlna_entry *next = find_child(srv, si, cur->id, comps[i], 0, req);
            if (!next && i == ncomps - 1)
            {
               /* Last component, no extension: maybe an item. */
               media = find_child(srv, si, cur->id, comps[i], 1, req);
               if (!media)
               {
                  dlna_entry_free(cur);
                  cur = NULL;
               }
               break;
            }
            dlna_entry_free(cur);
            cur = next;
         }
         if (cur && !media && nfold < ncomps)
         {
            media = find_child(srv, si, cur->id, comps[ncomps - 1], 1, req);
            if (!media)
            {
               dlna_entry_free(cur);
               cur = NULL;
            }
         }
         if (cur)
         {
            cand[ncand].folder     = cur;
            cand[ncand].media      = media;
            cand[ncand].root_level = k < root_count;
            ncand++;
         }
      }
   }
   free_ptr_entries(&firsts);

   if (!ncand || req->cancelled)
   {
      free_candidates(cand, ncand);
      dlna_result_free(r);
      if (!req->cancelled)
         post_error(req->token, "Could not find %s on the server", req->uri);
      free(copy);
      return;
   }

   /* Rank.  Only look inside folders when there is a choice to make. */
   best = 0;
   if (ncand > 1)
   {
      int best_score = 1000;
      for (j = 0; j < ncand; j++)
      {
         int score;
         cand[j].type = cand[j].media ? cand[j].media->media
            : sniff_folder(srv, si, cand[j].folder->id, req);
         score = type_rank(cand[j].type, guess) * 2 + (cand[j].root_level ? 0 : 1);
         if (score < best_score)
         {
            best_score = score;
            best       = j;
         }
      }
      dlna_log(LOG_INFO, "%s: %d candidates, chose \"%s\" (%s)\n", req->uri,
            ncand, cand[best].folder->title, cand[best].folder->id);
   }

   /* Which division (video, music, pictures) the folder belongs to, for
    * its folder icons.  Already known if we had to choose. */
   if (ncand > 1)
      r->division = cand[best].type;
   else if (cand[best].media)
      r->division = cand[best].media->media;
   else
      r->division = sniff_folder(srv, si, cand[best].folder->id, req);

   r->container_id = util_strdup(cand[best].folder->id);
   r->title        = util_strdup(cand[best].folder->title);
   r->media        = cand[best].media;
   cand[best].media = NULL;
   free_candidates(cand, ncand);
   post_result(r);
   free(copy);
}

/* ------------------------------------------------------------------ */
/* Worker thread */

static void free_request(request *r)
{
   if (!r)
      return;
   free(r->container_id);
   free(r->uri);
   free(r);
}

static void load_manual_servers(void)
{
   char  path[1024];
   char *text, *line, *next;

   util_path_join(path, sizeof(path), g_system_dir, "dlna");
   util_path_join(path, sizeof(path), path, "servers.txt");
   text = util_read_file(path, NULL);
   if (!text)
      return;
   for (line = text; line && *line; line = next)
   {
      next = strchr(line, '\n');
      if (next)
         *next++ = '\0';
      util_trim(line);
      if (*line && *line != '#')
         add_server_from_location(line);
   }
   free(text);
}

static void worker_main(void *arg)
{
   time_t started = time(NULL);
   int    searches = 1; /* dlna_init sent the first one */

   (void)arg;
   load_manual_servers();

   slock_lock(g_lock);
   while (!g_quit)
   {
      request *req = g_queue;
      if (!req)
      {
         /* SSDP is UDP and easily lost: repeat the search a couple of
          * times shortly after start up. */
         time_t now = time(NULL);
         if ((searches == 1 && now - started >= 2)
               || (searches == 2 && now - started >= 8))
         {
            searches++;
            slock_unlock(g_lock);
            dlna_search();
            slock_lock(g_lock);
            continue;
         }
         scond_wait_timeout(g_cond, g_lock, 500000);
         continue;
      }
      g_queue   = req->next;
      g_current = req;
      slock_unlock(g_lock);

      if (!req->cancelled)
      {
         if (req->kind == REQ_BROWSE)
            process_browse(req);
         else
            process_resolve(req);
      }

      slock_lock(g_lock);
      g_current = NULL;
      free_request(req);
   }
   slock_unlock(g_lock);
}

static int submit(request *req)
{
   request **tail;
   int       token;

   slock_lock(g_lock);
   token      = g_next_token++;
   req->token = token;
   for (tail = &g_queue; *tail; tail = &(*tail)->next)
      ;
   *tail = req;
   scond_broadcast(g_cond);
   slock_unlock(g_lock);
   return token;
}

int dlna_browse(int server, const char *container_id, media_type tree)
{
   request *req;
   if (!g_upnp_ok)
      return 0;
   req = (request*)calloc(1, sizeof(*req));
   if (!req)
      return 0;
   req->kind         = REQ_BROWSE;
   req->server       = server;
   req->container_id = util_strdup(container_id);
   req->tree         = tree;
   return submit(req);
}

int dlna_resolve(const char *uri)
{
   request *req;
   if (!g_upnp_ok || !uri)
      return 0;
   req = (request*)calloc(1, sizeof(*req));
   if (!req)
      return 0;
   req->kind = REQ_RESOLVE;
   req->uri  = util_strdup(uri);
   return submit(req);
}

void dlna_cancel(int token)
{
   request *r;
   if (!g_lock || token <= 0)
      return;
   slock_lock(g_lock);
   for (r = g_queue; r; r = r->next)
      if (r->token == token)
         r->cancelled = 1;
   if (g_current && g_current->token == token)
      g_current->cancelled = 1;
   scond_broadcast(g_cond);
   slock_unlock(g_lock);
}

/* ------------------------------------------------------------------ */
/* Init / deinit */

int dlna_init(const char *system_dir)
{
   int rc;

   if (g_upnp_ok)
      return 1;
   strncpy(g_system_dir, system_dir ? system_dir : "", sizeof(g_system_dir) - 1);
   g_system_dir[sizeof(g_system_dir) - 1] = '\0';
   g_quit = 0;

   g_lock = slock_new();
   g_cond = scond_new();
   if (!g_lock || !g_cond)
      return 0;

   /* NULL interface: let pupnp pick the first usable network interface;
    * port 0: any free port. */
   rc = UpnpInit2(NULL, 0);
   if (rc != UPNP_E_SUCCESS)
   {
      dlna_log(LOG_ERROR, "UpnpInit2 failed: %s (%d)\n",
            UpnpGetErrorMessage(rc), rc);
      return 0;
   }
   rc = UpnpRegisterClient(upnp_callback, NULL, &g_client);
   if (rc != UPNP_E_SUCCESS)
   {
      dlna_log(LOG_ERROR, "UpnpRegisterClient failed: %s (%d)\n",
            UpnpGetErrorMessage(rc), rc);
      UpnpFinish();
      return 0;
   }
   /* pupnp refuses SOAP messages larger than 16 kB by default, and that
    * limit also applies to the responses we receive: one page of DIDL-Lite
    * for a folder of a couple of hundred items is far bigger. */
   UpnpSetMaxContentLength(MAX_SOAP_RESPONSE);
   g_upnp_ok = 1;
   dlna_log(LOG_INFO, "UPnP ready on %s:%u\n",
         UpnpGetServerIpAddress() ? UpnpGetServerIpAddress() : "?",
         (unsigned)UpnpGetServerPort());

   dlna_search();
   g_thread = sthread_create(worker_main, NULL);
   if (!g_thread)
   {
      dlna_deinit();
      return 0;
   }
   return 1;
}

void dlna_deinit(void)
{
   int          i;
   size_t       j;
   result_node *rn;

   g_quit = 1;
   if (g_lock)
   {
      slock_lock(g_lock);
      if (g_current)
         g_current->cancelled = 1;
      scond_broadcast(g_cond);
      slock_unlock(g_lock);
   }
   if (g_thread)
   {
      sthread_join(g_thread);
      g_thread = NULL;
   }
   if (g_upnp_ok)
   {
      UpnpUnRegisterClient(g_client);
      UpnpFinish();
      g_upnp_ok = 0;
      g_client  = -1;
   }

   while (g_queue)
   {
      request *next = g_queue->next;
      free_request(g_queue);
      g_queue = next;
   }
   rn = g_results_head;
   while (rn)
   {
      result_node *next = rn->next;
      dlna_result_free(rn->result);
      free(rn);
      rn = next;
   }
   g_results_head = g_results_tail = NULL;

   for (i = 0; i < g_num_servers; i++)
      server_free(g_servers[i]);
   g_num_servers = 0;
   for (j = 0; j < g_seen_locations.count; j++)
      free(g_seen_locations.items[j]);
   ptr_array_clear(&g_seen_locations);

   if (g_cond)
      scond_free(g_cond);
   if (g_lock)
      slock_free(g_lock);
   g_cond = NULL;
   g_lock = NULL;
}
