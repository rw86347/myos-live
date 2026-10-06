/*
 * heif-pixbuf.h — decode HEIC/HEIF/AVIF through libheif into GdkPixbufs.
 * Shared by Preview and My Files so we do not depend on gdk-pixbuf's loader.
 */
#ifndef MYOS_HEIF_PIXBUF_H
#define MYOS_HEIF_PIXBUF_H

#include <glib.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

gboolean myos_heif_sniff(const guchar *head, gsize n);
gboolean myos_heif_path(const char *path);

GdkPixbuf *myos_heif_pixbuf_from_file(const char *path, GError **err);
GdkPixbuf *myos_heif_pixbuf_from_file_at_scale(const char *path, int width, int height,
                                               gboolean preserve_aspect_ratio, GError **err);

#endif
