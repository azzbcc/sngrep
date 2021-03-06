/**************************************************************************
 **
 ** sngrep - SIP Messages flow viewer
 **
 ** Copyright (C) 2013-2019 Ivan Alonso (Kaian)
 ** Copyright (C) 2013-2019 Irontec SL. All rights reserved.
 **
 ** This program is free software: you can redistribute it and/or modify
 ** it under the terms of the GNU General Public License as published by
 ** the Free Software Foundation, either version 3 of the License, or
 ** (at your option) any later version.
 **
 ** This program is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 ** GNU General Public License for more details.
 **
 ** You should have received a copy of the GNU General Public License
 ** along with this program.  If not, see <http://www.gnu.org/licenses/>.
 **
 ****************************************************************************/
/**
 * @file capture_input.c
 * @author Ivan Alonso [aka Kaian] <kaian@irontec.com>
 *
 * @brief Source of functions defined in capture_input.h
 *
 */
#include "config.h"
#include <glib.h>
#include <packet/dissector.h>
#include "capture_input.h"

typedef struct
{
    //! Manager owner of this capture input
    CaptureManager *manager;
    //! Capture Input type
    CaptureTech tech;
    //! Are captured packets life
    CaptureMode mode;
    //! Source string
    gchar *source_str;
    //! Source of events for this input
    GSource *source;
    //! Input size for offline mode in bytes
    guint64 size;
    //! Input loaded bytes so far
    guint64 loaded;
    //! Initial dissector for this input packets
    PacketDissector *initial;
} CaptureInputPrivate;

// CaptureInput class definition
G_DEFINE_ABSTRACT_TYPE_WITH_PRIVATE(CaptureInput, capture_input, G_TYPE_OBJECT)

void
capture_input_unref(CaptureInput *self)
{
    g_object_unref(self);
}

gpointer
capture_input_start(CaptureInput *self)
{
    g_return_val_if_fail (CAPTURE_IS_INPUT(self), NULL);

    CaptureInputClass *klass = CAPTURE_INPUT_GET_CLASS(self);
    if (klass->start(self)) {
        return klass->start(self);
    } else {
        return NULL;
    }
}

void
capture_input_stop(CaptureInput *self)
{
    g_return_if_fail (CAPTURE_IS_INPUT(self));

    CaptureInputClass *klass = CAPTURE_INPUT_GET_CLASS(self);
    if (klass->stop != NULL) {
        klass->stop(self);
    }
}

gint
capture_input_filter(CaptureInput *self, const gchar *filter, GError **error)
{
    g_return_val_if_fail (CAPTURE_IS_INPUT(self), -1);
    g_return_val_if_fail (error == NULL || *error == NULL, -1);

    CaptureInputClass *klass = CAPTURE_INPUT_GET_CLASS(self);
    if (klass->filter != NULL) {
        return klass->filter(self, filter, error);
    } else {
        return 0;
    }
}

void
capture_input_set_manager(CaptureInput *self, CaptureManager *manager)
{
    CaptureInputPrivate *priv = capture_input_get_instance_private(self);
    g_return_if_fail(priv != NULL);
    priv->manager = manager;
}

CaptureManager *
capture_input_manager(CaptureInput *self)
{
    CaptureInputPrivate *priv = capture_input_get_instance_private(self);
    g_return_val_if_fail(priv != NULL, NULL);
    return priv->manager;
}

void
capture_input_set_source(CaptureInput *self, GSource *source)
{
    CaptureInputPrivate *priv = capture_input_get_instance_private(self);
    g_return_if_fail(priv != NULL);
    priv->source = source;
}

GSource *
capture_input_source(CaptureInput *self)
{
    CaptureInputPrivate *priv = capture_input_get_instance_private(self);
    g_return_val_if_fail(priv != NULL, NULL);
    return priv->source;
}

void
capture_input_set_mode(CaptureInput *self, CaptureMode mode)
{
    CaptureInputPrivate *priv = capture_input_get_instance_private(self);
    g_return_if_fail(priv != NULL);
    priv->mode = mode;
}

CaptureMode
capture_input_mode(CaptureInput *self)
{
    CaptureInputPrivate *priv = capture_input_get_instance_private(self);
    g_return_val_if_fail(priv != NULL, CAPTURE_MODE_INVALID);
    return priv->mode;
}

void
capture_input_set_tech(CaptureInput *self, CaptureTech tech)
{
    CaptureInputPrivate *priv = capture_input_get_instance_private(self);
    g_return_if_fail(priv != NULL);
    priv->tech = tech;
}

CaptureTech
capture_input_tech(CaptureInput *self)
{
    CaptureInputPrivate *priv = capture_input_get_instance_private(self);
    g_return_val_if_fail(priv != NULL, CAPTURE_TECH_INVALID);
    return priv->tech;
}

void
capture_input_set_source_str(CaptureInput *self, const gchar *source_str)
{
    CaptureInputPrivate *priv = capture_input_get_instance_private(self);
    g_return_if_fail(priv != NULL);
    priv->source_str = g_strdup(source_str);
}

const gchar *
capture_input_source_str(CaptureInput *self)
{
    CaptureInputPrivate *priv = capture_input_get_instance_private(self);
    g_return_val_if_fail(priv != NULL, NULL);
    return priv->source_str;
}

void
capture_input_set_total_size(CaptureInput *self, guint64 size)
{
    CaptureInputPrivate *priv = capture_input_get_instance_private(self);
    g_return_if_fail(priv != NULL);
    priv->size = size;
}

guint64
capture_input_total_size(CaptureInput *self)
{
    CaptureInputPrivate *priv = capture_input_get_instance_private(self);
    g_return_val_if_fail(priv != NULL, 0);
    return priv->size;
}

void
capture_input_set_loaded_size(CaptureInput *self, guint64 loaded)
{
    CaptureInputPrivate *priv = capture_input_get_instance_private(self);
    g_return_if_fail(priv != NULL);
    priv->loaded = loaded;
}

guint64
capture_input_loaded_size(CaptureInput *self)
{
    CaptureInputPrivate *priv = capture_input_get_instance_private(self);
    g_return_val_if_fail(priv != NULL, 0);
    return priv->loaded;
}


void
capture_input_set_initial_dissector(CaptureInput *self, PacketDissector *dissector)
{
    CaptureInputPrivate *priv = capture_input_get_instance_private(self);
    g_return_if_fail(priv != NULL);
    priv->initial = dissector;
}

PacketDissector *
capture_input_initial_dissector(CaptureInput *self)
{
    CaptureInputPrivate *priv = capture_input_get_instance_private(self);
    g_return_val_if_fail(priv != NULL, NULL);
    return priv->initial;
}

static void
capture_input_dispose(GObject *object)
{
    CaptureInputPrivate *priv = capture_input_get_instance_private(CAPTURE_INPUT(object));
    g_source_unref(priv->source);
    G_OBJECT_CLASS(capture_input_parent_class)->dispose(object);
}

static void
capture_input_finalize(GObject *object)
{
    CaptureInputPrivate *priv = capture_input_get_instance_private(CAPTURE_INPUT(object));
    g_free(priv->source_str);
    G_OBJECT_CLASS (capture_input_parent_class)->finalize(object);
}

static void
capture_input_class_init(CaptureInputClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    object_class->dispose = capture_input_dispose;
    object_class->finalize = capture_input_finalize;
}

static void
capture_input_init(G_GNUC_UNUSED CaptureInput *self)
{
    CaptureInputPrivate *priv = capture_input_get_instance_private(CAPTURE_INPUT(self));
    priv->size = 0;
    priv->loaded = 0;
}
