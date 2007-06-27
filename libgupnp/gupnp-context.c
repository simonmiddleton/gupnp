/* 
 * Copyright (C) 2006, 2007 OpenedHand Ltd.
 *
 * Author: Jorn Baayen <jorn@openedhand.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

/**
 * SECTION:gupnp-context
 * @short_description: Context object wrapping shared networking bits.
 *
 * #GUPnPContext wraps the networking bits that are used by the various
 * GUPnP classes. It automatically starts a web server on demand.
 */

#include <config.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/utsname.h>
#include <sys/ioctl.h> 
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libsoup/soup-address.h>

#include "xdgmime/xdgmime.h"

#include "gupnp-context.h"
#include "gupnp-context-private.h"
#include "gupnp-marshal.h"
#include "gena-protocol.h"
#include "accept-language.h"

G_DEFINE_TYPE (GUPnPContext,
               gupnp_context,
               GSSDP_TYPE_CLIENT);

struct _GUPnPContextPrivate {
        char        *host_ip;
        guint        port;

        guint        subscription_timeout;

        SoupSession *session;

        SoupServer  *server; /* Started on demand */
        char        *server_url;
};

enum {
        PROP_0,
        PROP_HOST_IP,
        PROP_PORT,
        PROP_SUBSCRIPTION_TIMEOUT
};

#define LOOPBACK_IP "127.0.0.1"

typedef char ip_address[15+1];

#define inaddrr(x) (*(struct in_addr *) &ifr->x[sizeof sa.sin_port])
#define IFRSIZE   ((int)(size * sizeof (struct ifreq)))

/**
 * Generates the default server ID.
 **/
static char *
make_server_id (void)
{
        struct utsname sysinfo;

        uname (&sysinfo);
        
        return g_strdup_printf ("%s/%s UPnP/1.0 GUPnP/%s",
                                sysinfo.sysname,
                                sysinfo.version,
                                VERSION);
}

/**
 * Looks up the IP address for interface @iface.
 *
 * http://www.linuxquestions.org/questions/showthread.php?t=425637
 **/
gboolean
get_ip (const char *iface, ip_address ip)
{
        struct ifreq *ifr;
        struct ifreq ifrr;
        struct sockaddr_in sa;
        struct sockaddr ifaddr;
        int sockfd;

        if ((sockfd = socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
                return FALSE;

        ifr = &ifrr;

        ifrr.ifr_addr.sa_family = AF_INET;

        strncpy (ifrr.ifr_name, iface, sizeof (ifrr.ifr_name));

        if (ioctl (sockfd, SIOCGIFADDR, ifr) < 0)
                return FALSE;

        ifaddr = ifrr.ifr_addr;
        strncpy (ip, inet_ntoa (inaddrr (ifr_addr.sa_data)),
                 sizeof (ip_address));

        return TRUE;
}

/**
 * Looks up the IP address of the first non-loopback network interface.
 **/
static char *
get_default_host_ip (void)
{
        struct if_nameindex *ifs;
        char *ret;
        int i;
        
        /* List network interfaces */
        ifs = if_nameindex ();
        if (ifs == NULL) {
                g_error ("Failed to retrieve list of network interfaces:\n%s\n",
                         strerror (errno));

                return NULL;
        }

        ret = NULL;

        for (i = 0; ifs[i].if_index != 0 && ifs[i].if_name != NULL; i++) {
                ip_address ip;

                /* Retrieve IP address */
                if (!get_ip (ifs[i].if_name, ip))
                        continue;

                if (strcmp (ip, LOOPBACK_IP) != 0) {
                        /* This is not a loopback interface - just what we
                         * are looking for. */
                        ret = g_strdup (ip);

                        break;
                }
        }

        if_freenameindex (ifs);

        if (!ret) {
                /* Didn't find anything. Let's take the loopback IP. */
                ret = g_strdup (LOOPBACK_IP);
        }

        return ret;
}

static void
gupnp_context_init (GUPnPContext *context)
{
        char *server_id;

        context->priv =
                G_TYPE_INSTANCE_GET_PRIVATE (context,
                                             GUPNP_TYPE_CONTEXT,
                                             GUPnPContextPrivate);

        context->priv->session = soup_session_async_new ();

        server_id = make_server_id ();
        gssdp_client_set_server_id (GSSDP_CLIENT (context), server_id);
        g_free (server_id);
}

static void
gupnp_context_set_property (GObject      *object,
                            guint         property_id,
                            const GValue *value,
                            GParamSpec   *pspec)
{
        GUPnPContext *context;

        context = GUPNP_CONTEXT (object);

        switch (property_id) {
        case PROP_HOST_IP:
                context->priv->host_ip = g_value_dup_string (value);
                break;
        case PROP_PORT:
                context->priv->port = g_value_get_uint (value);
                break;
        case PROP_SUBSCRIPTION_TIMEOUT:
                context->priv->subscription_timeout = g_value_get_uint (value);
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
                break;
        }
}

static void
gupnp_context_get_property (GObject    *object,
                            guint       property_id,
                            GValue     *value,
                            GParamSpec *pspec)
{
        GUPnPContext *context;

        context = GUPNP_CONTEXT (object);

        switch (property_id) {
        case PROP_HOST_IP:
                g_value_set_string (value,
                                    gupnp_context_get_host_ip (context));
                break;
        case PROP_PORT:
                g_value_set_uint (value,
                                  gupnp_context_get_port (context));
                break;
        case PROP_SUBSCRIPTION_TIMEOUT:
                g_value_set_uint (value,
                                  gupnp_context_get_subscription_timeout
                                                                   (context));
                break;
        default:
                G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
                break;
        }
}

static void
gupnp_context_dispose (GObject *object)
{
        GUPnPContext *context;
        GObjectClass *object_class;

        context = GUPNP_CONTEXT (object);

        if (context->priv->session) {
                g_object_unref (context->priv->session);
                context->priv->session = NULL;
        }

        if (context->priv->server) {
                g_object_unref (context->priv->server);
                context->priv->server = NULL;
        }

        /* Call super */
        object_class = G_OBJECT_CLASS (gupnp_context_parent_class);
        object_class->dispose (object);
}

static void
gupnp_context_finalize (GObject *object)
{
        GUPnPContext *context;
        GObjectClass *object_class;

        context = GUPNP_CONTEXT (object);

        g_free (context->priv->host_ip);

        g_free (context->priv->server_url);

        /* Call super */
        object_class = G_OBJECT_CLASS (gupnp_context_parent_class);
        object_class->finalize (object);
}

static void
gupnp_context_class_init (GUPnPContextClass *klass)
{
        GObjectClass *object_class;

        object_class = G_OBJECT_CLASS (klass);

        object_class->set_property = gupnp_context_set_property;
        object_class->get_property = gupnp_context_get_property;
        object_class->dispose      = gupnp_context_dispose;
        object_class->finalize     = gupnp_context_finalize;

        g_type_class_add_private (klass, sizeof (GUPnPContextPrivate));

        /**
         * GUPnPContext:host-ip
         *
         * The local host's IP address. Set to NULL to autodetect.
         **/
        g_object_class_install_property
                (object_class,
                 PROP_HOST_IP,
                 g_param_spec_string ("host-ip",
                                      "Host IP",
                                      "The local host's IP address",
                                      NULL,
                                      G_PARAM_READWRITE |
                                      G_PARAM_CONSTRUCT_ONLY |
                                      G_PARAM_STATIC_NAME |
                                      G_PARAM_STATIC_NICK |
                                      G_PARAM_STATIC_BLURB));

        /**
         * GUPnPContext:port
         *
         * The port to run on. Set to 0 if you don't care what port to run on.
         **/
        g_object_class_install_property
                (object_class,
                 PROP_PORT,
                 g_param_spec_uint ("port",
                                    "Port",
                                    "Port to run on",
                                    0, G_MAXUINT, SOUP_ADDRESS_ANY_PORT,
                                    G_PARAM_READWRITE |
                                    G_PARAM_CONSTRUCT_ONLY |
                                    G_PARAM_STATIC_NAME |
                                    G_PARAM_STATIC_NICK |
                                    G_PARAM_STATIC_BLURB));

        /**
         * GUPnPContext:subscription-timeout
         *
         * The preferred subscription timeout: the number of seconds after
         * which subscriptions are renewed.
         **/
        g_object_class_install_property
                (object_class,
                 PROP_SUBSCRIPTION_TIMEOUT,
                 g_param_spec_uint ("subscription-timeout",
                                    "Subscription timeout",
                                    "Subscription timeout",
                                    GENA_MIN_TIMEOUT,
                                    G_MAXUINT,
                                    GENA_DEFAULT_TIMEOUT,
                                    G_PARAM_READWRITE |
                                    G_PARAM_CONSTRUCT_ONLY |
                                    G_PARAM_STATIC_NAME |
                                    G_PARAM_STATIC_NICK |
                                    G_PARAM_STATIC_BLURB));
}

SoupSession *
_gupnp_context_get_session (GUPnPContext *context)
{
        return context->priv->session;
}

/**
 * Default server handler: Return 404 not found.
 **/
static void
default_server_handler (SoupServerContext *server_context,
                        SoupMessage       *msg,
                        gpointer           user_data)
{
        soup_message_set_status (msg, SOUP_STATUS_NOT_FOUND);
}

SoupServer *
_gupnp_context_get_server (GUPnPContext *context)
{
        if (context->priv->server == NULL) {
                context->priv->server = soup_server_new (SOUP_SERVER_PORT,
                                                         context->priv->port,
                                                         NULL);

                soup_server_add_handler (context->priv->server, NULL, NULL,
                                         default_server_handler, NULL, context);

                soup_server_run_async (context->priv->server);
        }
        
        return context->priv->server;
}

/**
 * Makes an URL that refers to our server.
 **/
static char *
make_server_url (GUPnPContext *context)
{
        SoupServer *server;
        guint port;

        if (context->priv->host_ip == NULL)
                context->priv->host_ip = get_default_host_ip ();

        /* What port are we running on? */
        server = _gupnp_context_get_server (context);
        port = soup_server_get_port (server);

        /* Put it all together */
        return g_strdup_printf ("http://%s:%u", context->priv->host_ip, port);
}

const char *
_gupnp_context_get_server_url (GUPnPContext *context)
{
        if (context->priv->server_url == NULL)
                context->priv->server_url = make_server_url (context);

        return (const char *) context->priv->server_url;
}

/**
 * gupnp_context_new
 * @main_context: A #GMainContext, or NULL to use the default one
 * @host_ip: The local host's IP address, or NULL to use the IP address
 * of the first non-loopback network interface.
 * @port: Port to run on, or 0 if you don't care what port is used.
 * @error: A location to store a #GError, or NULL
 *
 * Return value: A new #GUPnPContext object.
 **/
GUPnPContext *
gupnp_context_new (GMainContext *main_context,
                   const char   *host_ip,
                   guint         port,
                   GError      **error)
{
        return g_object_new (GUPNP_TYPE_CONTEXT,
                             "main-context", main_context,
                             "host-ip", host_ip,
                             "port", port,
                             "error", error,
                             NULL);
}

/**
 * gupnp_context_get_host_ip
 * @context: A #GUPnPContext
 *
 * Return value: The IP address we advertise ourselves as using. Do not free.
 **/
const char *
gupnp_context_get_host_ip (GUPnPContext *context)
{
        g_return_val_if_fail (GUPNP_IS_CONTEXT (context), NULL);

        if (context->priv->host_ip == NULL)
                context->priv->host_ip = get_default_host_ip ();

        return context->priv->host_ip;
}

/**
 * gupnp_context_get_port
 * @context: A #GUPnPContext
 *
 * Return value: The port the SOAP server is running on.
 **/
guint
gupnp_context_get_port (GUPnPContext *context)
{
        SoupServer *server;

        g_return_val_if_fail (GUPNP_IS_CONTEXT (context), 0);

        server = _gupnp_context_get_server (context);
        return soup_server_get_port (server);
}

/**
 * gupnp_context_set_subscription_timeout
 * @context: A #GUPnPContext
 * @timeout: Event subscription timeout in seconds
 *
 * Sets the event subscription timeout to @timeout. Use 0 if you don't
 * want subscriptions to time out. Note that any client side subscriptions
 * will automatically be renewed.
 **/
void
gupnp_context_set_subscription_timeout (GUPnPContext *context,
                                        guint         timeout)
{
        g_return_if_fail (GUPNP_IS_CONTEXT (context));

        context->priv->subscription_timeout = timeout;

        g_object_notify (G_OBJECT (context), "subscription-timeout");
}

/**
 * gupnp_context_get_subscription_timeout
 * @context: A #GUPnPContext
 *
 * Return value: The event subscription timeout in seconds, or 0, meaning
 * no timeout.
 **/
guint
gupnp_context_get_subscription_timeout (GUPnPContext *context)
{
        g_return_val_if_fail (GUPNP_IS_CONTEXT (context), 0);

        return context->priv->subscription_timeout;
}

/* Modified from libsoup simple-httpd.c */
static void
hosting_server_handler (SoupServerContext *server_context,
                        SoupMessage       *msg,
                        gpointer           user_data)
{
        const char *local_path, *lang, *mime;
        char *path, *path_to_open, *path_locale, *slash;
        SoupMethodId method;
        struct stat st;
        int fd, path_offset;
        GList *locales;

        local_path = (const char *) user_data;

        path = soup_uri_to_string (soup_message_get_uri (msg), TRUE);

        method = soup_method_get_id (msg->method);
        if (method != SOUP_METHOD_ID_GET && method != SOUP_METHOD_ID_HEAD) {
                soup_message_set_status (msg, SOUP_STATUS_NOT_IMPLEMENTED);
                locales = NULL;
                goto DONE;
        }

        /* Get preferred locales */
        locales = accept_language_get_locales (msg);

        if (path) {
                if (*path != '/') {
                        soup_message_set_status (msg, SOUP_STATUS_BAD_REQUEST);
                        goto DONE;
                }

                /* Skip the server path */
                path_offset = strlen (server_context->handler->path);
        } else {
                path = g_strdup ("");

                path_offset = 0;
        }

        path_locale = NULL;

 NEXT_LOCALE:
        g_free (path_locale);

        if (locales) {
                path_locale = g_strdup_printf ("%s.%s",
                                               path + path_offset,
                                               (char *) locales->data);
        } else
                path_locale = g_strdup (path + path_offset);

        path_to_open = g_build_filename (local_path, path_locale, NULL);

 AGAIN:
        if (stat (path_to_open, &st) == -1) {
                g_free (path_to_open);
                if (errno == EPERM)
                        soup_message_set_status (msg, SOUP_STATUS_FORBIDDEN);
                else if (errno == ENOENT) {
                        if (locales) {
                                locales = locales->next;
                                goto NEXT_LOCALE;
                        } else
                                soup_message_set_status (msg,
                                                         SOUP_STATUS_NOT_FOUND);
                } else
                        soup_message_set_status
                                (msg, SOUP_STATUS_INTERNAL_SERVER_ERROR);
                goto DONE;
        }

        /* Handle directories */
        if (S_ISDIR (st.st_mode)) {
                slash = strrchr (path_to_open, '/');
                if (!slash || slash[1]) {
                        char *uri, *redir_uri;

                        uri = soup_uri_to_string (soup_message_get_uri (msg),
                                                  FALSE);
                        redir_uri = g_strdup_printf ("%s/", uri);
                        soup_message_add_header (msg->response_headers,
                                                 "Location", redir_uri);
                        soup_message_set_status (msg,
                                                 SOUP_STATUS_MOVED_PERMANENTLY);
                        g_free (redir_uri);
                        g_free (uri);
                        g_free (path_to_open);
                        goto DONE;
                }

                g_free (path_to_open);
                path_to_open = g_build_filename (local_path,
                                                 path_locale,
                                                 "index.html", NULL);
                goto AGAIN;
        }

        g_free (path_locale);

        /* Read file */
        fd = open (path_to_open, O_RDONLY);
        g_free (path_to_open);
        if (fd == -1) {
                soup_message_set_status (msg,
                                         SOUP_STATUS_INTERNAL_SERVER_ERROR);
                goto DONE;
        }

        msg->response.owner = SOUP_BUFFER_SYSTEM_OWNED;
        msg->response.length = st.st_size;

        msg->response.body = g_malloc (msg->response.length);
        read (fd, msg->response.body, msg->response.length);

        close (fd);

        /* Set Content-Type */
        mime = xdg_mime_get_mime_type_for_data (msg->response.body,
                                                msg->response.length);

        soup_message_add_header (msg->response_headers,
                                 "Content-Type",
                                 mime);

        /* Set Content-Language */
        if (locales) {
                http_language_from_locale (locales->data);

                lang = locales->data;
        } else
                lang = "en";

        soup_message_add_header (msg->response_headers,
                                 "Content-Language",
                                 lang);

        /* OK */
        soup_message_set_status (msg, SOUP_STATUS_OK);

 DONE:
        /* Cleanup */
        g_free (path);

        while (locales) {
                g_free (locales->data);
                locales = g_list_delete_link (locales, locales);
        }
}

static void
unregister_hosting_server_handler (SoupServer        *server,
                                   SoupServerHandler *handler,
                                   gpointer           user_data)
{
        g_free (user_data);
}

/**
 * gupnp_context_host_path
 * @context: A #GUPnPContext
 * @local_path: Path to the local file or folder to be hosted
 * @server_path: Web server path where @local_path should be hosted
 *
 * Start hosting @local_path at @server_path. Files with the path
 * @local_path.LOCALE (if they exist) will be served up when LOCALE is
 * specified in the request's Accept-Language header. 
 **/
void
gupnp_context_host_path (GUPnPContext *context,
                         const char   *local_path,
                         const char   *server_path)
{
        SoupServer *server;

        g_return_if_fail (GUPNP_IS_CONTEXT (context));
        g_return_if_fail (local_path != NULL);
        g_return_if_fail (server_path != NULL);

        server = _gupnp_context_get_server (context);

        soup_server_add_handler (server, server_path, NULL,
                                 hosting_server_handler,
                                 unregister_hosting_server_handler,
                                 g_strdup (local_path));
}

/**
 * gupnp_context_unhost_path
 * @context: A #GUPnPContext
 * @server_path: Web server path where the file or folder is hosted
 *
 * Stop hosting the file or folder at @server_path.
 **/
void
gupnp_context_unhost_path (GUPnPContext *context,
                           const char   *server_path)
{
        SoupServer *server;

        g_return_if_fail (GUPNP_IS_CONTEXT (context));
        g_return_if_fail (server_path != NULL);

        server = _gupnp_context_get_server (context);

        soup_server_remove_handler (server, server_path);
}
