/* GStreamer
 * Copyright (C) 2013 Stefan Sauer <ensonic@users.sf.net>
 *
 * gststats.c: tracing module that logs events
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
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */
/**
 * SECTION:gststats
 * @short_description: log event stats
 *
 * A tracing module that builds usage statistic for elements and pads. 
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "gststats.h"

#include <stdio.h>

GST_DEBUG_CATEGORY_STATIC (gst_stats_debug);
#define GST_CAT_DEFAULT gst_stats_debug

static GQuark data_quark;
G_LOCK_DEFINE (_elem_stats);
G_LOCK_DEFINE (_pad_stats);

#define _do_init \
    GST_DEBUG_CATEGORY_INIT (gst_stats_debug, "stats", 0, "stats tracer"); \
    data_quark = g_quark_from_static_string ("gststats:data");
#define gst_stats_tracer_parent_class parent_class
G_DEFINE_TYPE_WITH_CODE (GstStatsTracer, gst_stats_tracer, GST_TYPE_TRACER,
    _do_init);

typedef struct
{
  /* we can't rely on the address to be unique over time */
  guint index;
  /* for pre + post */
  GstClockTime last_ts;
  /* hierarchy */
  guint parent_ix;
} GstPadStats;

typedef struct
{
  /* we can't rely on the address to be unique over time */
  guint index;
  /* for pre + post */
  GstClockTime last_ts;
  /* time spend in this element */
  GstClockTime treal;
  /* hierarchy */
  guint parent_ix;
} GstElementStats;

/* data helper */

static GstElementStats no_elem_stats = { 0, };

static GstElementStats *
fill_element_stats (GstStatsTracer * self, GstElement * element)
{
  GstElementStats *stats = g_slice_new0 (GstElementStats);

  stats->index = self->num_elements++;
  stats->parent_ix = G_MAXUINT;
  return stats;
}

static void
log_new_element_stats (GstElementStats * stats, GstElement * element,
    GstClockTime elapsed)
{
  gst_tracer_log_trace (gst_structure_new ("new-element",
          "thread-id", G_TYPE_UINT, GPOINTER_TO_UINT (g_thread_self ()),
          "ts", G_TYPE_UINT64, elapsed,
          "ix", G_TYPE_UINT, stats->index,
          "parent-ix", G_TYPE_UINT, stats->parent_ix,
          "name", G_TYPE_STRING, GST_OBJECT_NAME (element),
          "type", G_TYPE_STRING, G_OBJECT_TYPE_NAME (element),
          "is-bin", G_TYPE_BOOLEAN, GST_IS_BIN (element), NULL));
}

static void
free_element_stats (gpointer data)
{
  g_slice_free (GstElementStats, data);
}

static GstElementStats *
create_element_stats (GstStatsTracer * self, GstElement * element)
{
  GstElementStats *stats;

  stats = fill_element_stats (self, element);
  g_object_set_qdata_full ((GObject *) element, data_quark, stats,
      free_element_stats);

  return stats;
}

static inline GstElementStats *
get_element_stats (GstStatsTracer * self, GstElement * element)
{
  GstElementStats *stats;
  gboolean is_new = FALSE;

  if (!element) {
    no_elem_stats.index = G_MAXUINT;
    return &no_elem_stats;
  }

  G_LOCK (_elem_stats);
  if (!(stats = g_object_get_qdata ((GObject *) element, data_quark))) {
    stats = create_element_stats (self, element);
    is_new = TRUE;
  }
  G_UNLOCK (_elem_stats);
  if (G_UNLIKELY (stats->parent_ix == G_MAXUINT)) {
    GstElement *parent = GST_ELEMENT_PARENT (element);
    if (parent) {
      GstElementStats *parent_stats = get_element_stats (self, parent);
      stats->parent_ix = parent_stats->index;
    }
  }
  if (G_UNLIKELY (is_new)) {
    log_new_element_stats (stats, element, GST_CLOCK_TIME_NONE);
  }
  return stats;
}

/*
 * Get the element/bin owning the pad. 
 *
 * in: a normal pad
 * out: the element
 *
 * in: a proxy pad
 * out: the element that contains the peer of the proxy
 *
 * in: a ghost pad
 * out: the bin owning the ghostpad
 */
/* TODO(ensonic): gst_pad_get_parent_element() would not work here, should we
 * add this as new api, e.g. gst_pad_find_parent_element();
 */
static GstElement *
get_real_pad_parent (GstPad * pad)
{
  GstObject *parent;

  if (!pad)
    return NULL;

  parent = GST_OBJECT_PARENT (pad);

  /* if parent of pad is a ghost-pad, then pad is a proxy_pad */
  if (parent && GST_IS_GHOST_PAD (parent)) {
    pad = GST_PAD_CAST (parent);
    parent = GST_OBJECT_PARENT (pad);
  }
  return GST_ELEMENT_CAST (parent);
}

static GstPadStats no_pad_stats = { 0, };

static GstPadStats *
fill_pad_stats (GstStatsTracer * self, GstPad * pad)
{
  GstPadStats *stats = g_slice_new0 (GstPadStats);

  stats->index = self->num_pads++;
  stats->parent_ix = G_MAXUINT;

  return stats;
}

static void
log_new_pad_stats (GstPadStats * stats, GstPad * pad)
{
  gst_tracer_log_trace (gst_structure_new ("new-pad",
          "ix", G_TYPE_UINT, stats->index,
          "parent-ix", G_TYPE_UINT, stats->parent_ix,
          "name", G_TYPE_STRING, GST_OBJECT_NAME (pad),
          "type", G_TYPE_STRING, G_OBJECT_TYPE_NAME (pad),
          "is-ghostpad", G_TYPE_BOOLEAN, GST_IS_GHOST_PAD (pad),
          "pad-direction", GST_TYPE_PAD_DIRECTION, GST_PAD_DIRECTION (pad),
          "thread-id", G_TYPE_UINT, GPOINTER_TO_UINT (g_thread_self ()), NULL));
}

static void
free_pad_stats (gpointer data)
{
  g_slice_free (GstPadStats, data);
}

static GstPadStats *
get_pad_stats (GstStatsTracer * self, GstPad * pad)
{
  GstPadStats *stats;
  gboolean is_new = FALSE;

  if (!pad) {
    no_pad_stats.index = G_MAXUINT;
    return &no_pad_stats;
  }

  G_LOCK (_pad_stats);
  if (!(stats = g_object_get_qdata ((GObject *) pad, data_quark))) {
    stats = fill_pad_stats (self, pad);
    g_object_set_qdata_full ((GObject *) pad, data_quark, stats,
        free_pad_stats);
    is_new = TRUE;
  }
  G_UNLOCK (_pad_stats);
  if (G_UNLIKELY (stats->parent_ix == G_MAXUINT)) {
    GstElement *elem = get_real_pad_parent (pad);
    if (elem) {
      GstElementStats *elem_stats = get_element_stats (self, elem);

      stats->parent_ix = elem_stats->index;
    }
  }
  if (G_UNLIKELY (is_new)) {
    log_new_pad_stats (stats, pad);
  }
  return stats;
}

static void
do_buffer_stats (GstStatsTracer * self, GstPad * this_pad,
    GstPadStats * this_pad_stats, GstPad * that_pad,
    GstPadStats * that_pad_stats, GstBuffer * buf, GstClockTime elapsed)
{
  GstElement *this_elem = get_real_pad_parent (this_pad);
  GstElementStats *this_elem_stats = get_element_stats (self, this_elem);
  GstElement *that_elem = get_real_pad_parent (that_pad);
  GstElementStats *that_elem_stats = get_element_stats (self, that_elem);

  /* TODO(ensonic): need a quark-table (shared with the tracer-front-ends?) */
  gst_tracer_log_trace (gst_structure_new ("buffer",
          "thread-id", G_TYPE_UINT, GPOINTER_TO_UINT (g_thread_self ()),
          "ts", G_TYPE_UINT64, elapsed,
          "pad-ix", G_TYPE_UINT, this_pad_stats->index,
          "elem-ix", G_TYPE_UINT, this_elem_stats->index,
          "peer-pad-ix", G_TYPE_UINT, that_pad_stats->index,
          "peer-elem-ix", G_TYPE_UINT, that_elem_stats->index,
          "buffer-size", G_TYPE_UINT, gst_buffer_get_size (buf),
          "buffer-pts", G_TYPE_UINT64, GST_BUFFER_PTS (buf),
          "buffer-dts", G_TYPE_UINT64, GST_BUFFER_DTS (buf),
          "buffer-duration", G_TYPE_UINT64, GST_BUFFER_DURATION (buf),
          "buffer-flags", GST_TYPE_BUFFER_FLAGS, GST_BUFFER_FLAGS (buf),
          /*
             scheduling-jitter: for this we need the last_ts on the pad
           */
          NULL));
}

static void
do_query_stats (GstStatsTracer * self, GstPad * this_pad,
    GstPadStats * this_pad_stats, GstPad * that_pad,
    GstPadStats * that_pad_stats, GstQuery * qry, GstClockTime elapsed,
    gboolean res, gboolean pre)
{
  GstElement *this_elem = get_real_pad_parent (this_pad);
  GstElementStats *this_elem_stats = get_element_stats (self, this_elem);
  GstElement *that_elem = get_real_pad_parent (that_pad);
  GstElementStats *that_elem_stats = get_element_stats (self, that_elem);
  GstStructure *s;

  s = gst_structure_new ("query",
      "thread-id", G_TYPE_UINT, GPOINTER_TO_UINT (g_thread_self ()),
      "ts", G_TYPE_UINT64, elapsed,
      "pad-ix", G_TYPE_UINT, this_pad_stats->index,
      "elem-ix", G_TYPE_UINT, this_elem_stats->index,
      "peer-pad-ix", G_TYPE_UINT, that_pad_stats->index,
      "peer-elem-ix", G_TYPE_UINT, that_elem_stats->index,
      "name", G_TYPE_STRING, GST_QUERY_TYPE_NAME (qry),
      "structure", GST_TYPE_STRUCTURE, gst_query_get_structure (qry), NULL);
  if (!pre) {
    gst_structure_set (s, "res", G_TYPE_BOOLEAN, res, NULL);
  }
  gst_tracer_log_trace (s);
}

static void
do_element_stats (GstStatsTracer * self, GstPad * pad, GstClockTime elapsed1,
    GstClockTime elapsed2)
{
  GstClockTimeDiff elapsed = GST_CLOCK_DIFF (elapsed1, elapsed2);
  GstObject *parent = GST_OBJECT_PARENT (pad);
  GstElement *this =
      GST_ELEMENT_CAST (GST_IS_PAD (parent) ? GST_OBJECT_PARENT (parent) :
      parent);
  GstElementStats *this_stats = get_element_stats (self, this);
  GstPad *peer_pad = GST_PAD_PEER (pad);
  GstElementStats *peer_stats;

  if (!peer_pad)
    return;

  /* walk the ghost pad chain downstream to get the real pad */
  /* if parent of peer_pad is a ghost-pad, then peer_pad is a proxy_pad */
  parent = GST_OBJECT_PARENT (peer_pad);
  if (parent && GST_IS_GHOST_PAD (parent)) {
    peer_pad = GST_PAD_CAST (parent);
    /* if this is now the ghost pad, get the peer of this */
    get_pad_stats (self, peer_pad);
    if ((parent = GST_OBJECT_PARENT (peer_pad))) {
      get_element_stats (self, GST_ELEMENT_CAST (parent));
    }
    peer_pad = GST_PAD_PEER (GST_GHOST_PAD_CAST (peer_pad));
    parent = peer_pad ? GST_OBJECT_PARENT (peer_pad) : NULL;
  }
  /* walk the ghost pad chain upstream to get the real pad */
  /* if peer_pad is a ghost-pad, then parent is a bin and it is the parent of
   * a proxy_pad */
  while (peer_pad && GST_IS_GHOST_PAD (peer_pad)) {
    get_pad_stats (self, peer_pad);
    get_element_stats (self, GST_ELEMENT_CAST (parent));
    peer_pad = gst_ghost_pad_get_target (GST_GHOST_PAD_CAST (peer_pad));
    parent = peer_pad ? GST_OBJECT_PARENT (peer_pad) : NULL;
  }

  if (!parent) {
    printf ("%" GST_TIME_FORMAT
        " transmission on unparented target pad %s_%s -> %s_%s\n",
        GST_TIME_ARGS (elapsed), GST_DEBUG_PAD_NAME (pad),
        GST_DEBUG_PAD_NAME (peer_pad));
    return;
  }
  peer_stats = get_element_stats (self, GST_ELEMENT_CAST (parent));

  /* we'd like to gather time spend in each element, but this does not make too
   * much sense yet
   * pure push/pull-based:
   *   - the time spend in the push/pull_range is accounted for the peer and
   *     removed from the current element
   *   - this works for chains
   *   - drawback is sink elements that block to sync have a high time usage
   *     - we could rerun the ests with sync=false
   * both:
   *   - a.g. demuxers both push and pull. thus we subtract time for the pull
   *     and the push operations, but never add anything.
   *   - can we start a counter after push/pull in such elements and add then
   *     time to the element upon next pad activity?
   */
#if 1
  /* this does not make sense for demuxers */
  this_stats->treal -= elapsed;
  peer_stats->treal += elapsed;
#else
  /* this creates several >100% figures */
  this_stats->treal += GST_CLOCK_DIFF (this_stats->last_ts, elapsed2) - elapsed;
  peer_stats->treal += elapsed;
  this_stats->last_ts = elapsed2;
  peer_stats->last_ts = elapsed2;
#endif
}

/* hooks */

static void
do_push_buffer_pre (GstStatsTracer * self, guint64 ts, GstPad * this_pad,
    GstBuffer * buffer)
{
  GstPadStats *this_pad_stats = get_pad_stats (self, this_pad);
  GstPad *that_pad = GST_PAD_PEER (this_pad);
  GstPadStats *that_pad_stats = get_pad_stats (self, that_pad);

  do_buffer_stats (self, this_pad, this_pad_stats, that_pad, that_pad_stats,
      buffer, ts);
}

static void
do_push_buffer_post (GstStatsTracer * self, guint64 ts, GstPad * pad)
{
  GstPadStats *stats = get_pad_stats (self, pad);

  do_element_stats (self, pad, stats->last_ts, ts);
}

typedef struct
{
  GstStatsTracer *self;
  GstPad *this_pad;
  GstPadStats *this_pad_stats;
  GstPad *that_pad;
  GstPadStats *that_pad_stats;
  guint64 ts;
} DoPushBufferListArgs;

static gboolean
do_push_buffer_list_item (GstBuffer ** buffer, guint idx, gpointer user_data)
{
  DoPushBufferListArgs *args = (DoPushBufferListArgs *) user_data;

  do_buffer_stats (args->self, args->this_pad, args->this_pad_stats,
      args->that_pad, args->that_pad_stats, *buffer, args->ts);
  return TRUE;
}

static void
do_push_buffer_list_pre (GstStatsTracer * self, guint64 ts, GstPad * this_pad,
    GstBufferList * list)
{
  GstPadStats *this_pad_stats = get_pad_stats (self, this_pad);
  GstPad *that_pad = GST_PAD_PEER (this_pad);
  GstPadStats *that_pad_stats = get_pad_stats (self, that_pad);
  DoPushBufferListArgs args = { self, this_pad, this_pad_stats, that_pad,
    that_pad_stats, ts
  };

  gst_buffer_list_foreach (list, do_push_buffer_list_item, &args);
}

static void
do_push_buffer_list_post (GstStatsTracer * self, guint64 ts, GstPad * pad)
{
  GstPadStats *stats = get_pad_stats (self, pad);

  do_element_stats (self, pad, stats->last_ts, ts);
}

static void
do_pull_range_pre (GstStatsTracer * self, guint64 ts, GstPad * pad)
{
  GstPadStats *stats = get_pad_stats (self, pad);
  stats->last_ts = ts;
}

static void
do_pull_range_post (GstStatsTracer * self, guint64 ts, GstPad * this_pad,
    GstBuffer * buffer)
{
  GstPadStats *this_pad_stats = get_pad_stats (self, this_pad);
  guint64 last_ts = this_pad_stats->last_ts;
  GstPad *that_pad = GST_PAD_PEER (this_pad);
  GstPadStats *that_pad_stats = get_pad_stats (self, that_pad);

  if (buffer != NULL) {
    do_buffer_stats (self, this_pad, this_pad_stats, that_pad, that_pad_stats,
        buffer, ts);
  }
  do_element_stats (self, this_pad, last_ts, ts);
}

static void
do_push_event_pre (GstStatsTracer * self, guint64 ts, GstPad * pad,
    GstEvent * ev)
{
  GstElement *elem = get_real_pad_parent (pad);
  GstElementStats *elem_stats = get_element_stats (self, elem);
  GstPadStats *pad_stats = get_pad_stats (self, pad);

  elem_stats->last_ts = ts;
  gst_tracer_log_trace (gst_structure_new ("event",
          "thread-id", G_TYPE_UINT, GPOINTER_TO_UINT (g_thread_self ()),
          "ts", G_TYPE_UINT64, ts,
          "pad-ix", G_TYPE_UINT, pad_stats->index,
          "elem-ix", G_TYPE_UINT, elem_stats->index,
          "name", G_TYPE_STRING, GST_EVENT_TYPE_NAME (ev), NULL));
}

static void
do_post_message_pre (GstStatsTracer * self, guint64 ts, GstElement * elem,
    GstMessage * msg)
{
  GstElementStats *stats = get_element_stats (self, elem);
  const GstStructure *msg_s;
  GstStructure *structure = gst_structure_new ("message",
      "thread-id", G_TYPE_UINT, GPOINTER_TO_UINT (g_thread_self ()),
      "ts", G_TYPE_UINT64, ts,
      "elem-ix", G_TYPE_UINT, stats->index,
      "name", G_TYPE_STRING, GST_MESSAGE_TYPE_NAME (msg),
      NULL);

  stats->last_ts = ts;

  if ((msg_s = gst_message_get_structure (msg))) {
    gst_structure_set (structure, "structure", GST_TYPE_STRUCTURE, msg_s, NULL);
  }

  gst_tracer_log_trace (structure);
}

static void
do_element_new (GstStatsTracer * self, guint64 ts, GstElement * elem)
{
  GstElementStats *stats;

  stats = create_element_stats (self, elem);
  log_new_element_stats (stats, elem, ts);
}

static void
do_element_query_pre (GstStatsTracer * self, guint64 ts, GstElement * elem,
    GstQuery * qry)
{
  GstElementStats *stats = get_element_stats (self, elem);

  stats->last_ts = ts;
  gst_tracer_log_trace (gst_structure_new ("element-query",
          "thread-id", G_TYPE_UINT, GPOINTER_TO_UINT (g_thread_self ()),
          "ts", G_TYPE_UINT64, ts,
          "elem-ix", G_TYPE_UINT, stats->index,
          "name", G_TYPE_STRING, GST_QUERY_TYPE_NAME (qry), NULL));
}

static void
do_query_pre (GstStatsTracer * self, guint64 ts, GstPad * this_pad,
    GstQuery * qry)
{
  GstPadStats *this_pad_stats = get_pad_stats (self, this_pad);
  GstPad *that_pad = GST_PAD_PEER (this_pad);
  GstPadStats *that_pad_stats = get_pad_stats (self, that_pad);

  do_query_stats (self, this_pad, this_pad_stats, that_pad, that_pad_stats,
      qry, ts, FALSE, TRUE);
}

static void
do_query_post (GstStatsTracer * self, guint64 ts, GstPad * this_pad,
    gboolean res, GstQuery * qry)
{
  GstPadStats *this_pad_stats = get_pad_stats (self, this_pad);
  GstPad *that_pad = GST_PAD_PEER (this_pad);
  GstPadStats *that_pad_stats = get_pad_stats (self, that_pad);

  do_query_stats (self, this_pad, this_pad_stats, that_pad, that_pad_stats,
      qry, ts, res, FALSE);
}

/* tracer class */

static void
gst_stats_tracer_class_init (GstStatsTracerClass * klass)
{
  /* announce trace formats */
  /* *INDENT-OFF* */
  gst_tracer_log_trace (gst_structure_new ("buffer.class",
      "thread-id", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "related-to", G_TYPE_STRING, "thread", /* TODO use genum */
          NULL),
      "pad-ix", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "related-to", G_TYPE_STRING, "pad",  /* TODO: use genum */
          NULL),
      "element-ix", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "related-to", G_TYPE_STRING, "element",  /* TODO: use genum */
          NULL),
      "peer-pad-ix", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "related-to", G_TYPE_STRING, "pad",  /* TODO: use genum */
          NULL),
      "peer-element-ix", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "related-to", G_TYPE_STRING, "element",  /* TODO: use genum */
          NULL),
      "buffer-size", GST_TYPE_STRUCTURE, gst_structure_new ("value",
          "type", G_TYPE_GTYPE, G_TYPE_UINT,
          "description", G_TYPE_STRING, "size of buffer in bytes",
          "flags", G_TYPE_STRING, "",  /* TODO: use gflags */ 
          "min", G_TYPE_UINT, 0, 
          "max", G_TYPE_UINT, G_MAXUINT,
          NULL),
      "buffer-ts", GST_TYPE_STRUCTURE, gst_structure_new ("value",
          "type", G_TYPE_GTYPE, G_TYPE_UINT64,
          "description", G_TYPE_STRING, "timestamp of the buffer in ns",
          "flags", G_TYPE_STRING, "",  /* TODO: use gflags */ 
          "min", G_TYPE_UINT64, G_GUINT64_CONSTANT (0),
          "max", G_TYPE_UINT64, G_MAXUINT64,
          NULL),
      "buffer-duration", GST_TYPE_STRUCTURE, gst_structure_new ("value",
          "type", G_TYPE_GTYPE, G_TYPE_UINT64,
          "description", G_TYPE_STRING, "duration of the buffer in ns",
          "flags", G_TYPE_STRING, "",  /* TODO: use gflags */ 
          "min", G_TYPE_UINT64, G_GUINT64_CONSTANT (0),
          "max", G_TYPE_UINT64, G_MAXUINT64,
          NULL),
      /* TODO(ensonic): "buffer-flags" */
      NULL));
  gst_tracer_log_trace (gst_structure_new ("event.class",
      "thread-id", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "related-to", G_TYPE_STRING, "thread", /* TODO use genum */
          NULL),
      "pad-ix", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "related-to", G_TYPE_STRING, "pad",  /* TODO: use genum */
          NULL),
      "element-ix", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "related-to", G_TYPE_STRING, "element",  /* TODO: use genum */
          NULL),
      "name", GST_TYPE_STRUCTURE, gst_structure_new ("value",
          "type", G_TYPE_GTYPE, G_TYPE_STRING,
          "description", G_TYPE_STRING, "name of the event",
          "flags", G_TYPE_STRING, "",  /* TODO: use gflags */ 
          NULL),
      NULL));
  gst_tracer_log_trace (gst_structure_new ("message.class",
      "thread-id", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "related-to", G_TYPE_STRING, "thread", /* TODO use genum */
          NULL),
      "element-ix", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "related-to", G_TYPE_STRING, "element",  /* TODO: use genum */
          NULL),
      "name", GST_TYPE_STRUCTURE, gst_structure_new ("value",
          "type", G_TYPE_GTYPE, G_TYPE_STRING,
          "description", G_TYPE_STRING, "name of the message",
          "flags", G_TYPE_STRING, "",  /* TODO: use gflags */ 
          NULL),
      "structure", GST_TYPE_STRUCTURE, gst_structure_new ("structure",
          "type", G_TYPE_GTYPE, GST_TYPE_STRUCTURE,
          "description", G_TYPE_STRING, "message structure",
          NULL),
      NULL));
  gst_tracer_log_trace (gst_structure_new ("elementquery.class",
      "thread-id", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "related-to", G_TYPE_STRING, "thread", /* TODO use genum */
          NULL),
      "element-ix", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "related-to", G_TYPE_STRING, "element",  /* TODO: use genum */
          NULL),
      "name", GST_TYPE_STRUCTURE, gst_structure_new ("value",
          "type", G_TYPE_GTYPE, G_TYPE_STRING,
          "description", G_TYPE_STRING, "name of the query",
          "flags", G_TYPE_STRING, "",  /* TODO: use gflags */ 
          NULL),
      NULL));
  gst_tracer_log_trace (gst_structure_new ("query.class",
      "thread-id", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "related-to", G_TYPE_STRING, "thread", /* TODO use genum */
          NULL),
      "pad-ix", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "related-to", G_TYPE_STRING, "pad",  /* TODO: use genum */
          NULL),
      "element-ix", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "related-to", G_TYPE_STRING, "element",  /* TODO: use genum */
          NULL),
      "peer-pad-ix", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "related-to", G_TYPE_STRING, "pad",  /* TODO: use genum */
          NULL),
      "peer-element-ix", GST_TYPE_STRUCTURE, gst_structure_new ("scope",
          "related-to", G_TYPE_STRING, "element",  /* TODO: use genum */
          NULL),
      "name", GST_TYPE_STRUCTURE, gst_structure_new ("value",
          "type", G_TYPE_GTYPE, G_TYPE_STRING,
          "description", G_TYPE_STRING, "name of the query",
          "flags", G_TYPE_STRING, "",  /* TODO: use gflags */ 
          NULL),
      "structure", GST_TYPE_STRUCTURE, gst_structure_new ("value",
          "type", G_TYPE_GTYPE, GST_TYPE_STRUCTURE,
          "description", G_TYPE_STRING, "query structure",
          "flags", G_TYPE_STRING, "",  /* TODO: use gflags */ 
          NULL),
      /* TODO(ensonic): "buffer-flags" */
      NULL));
  /* *INDENT-ON* */
}

static void
gst_stats_tracer_init (GstStatsTracer * self)
{
  GstTracer *tracer = GST_TRACER (self);

  gst_tracing_register_hook (tracer, "pad-push-pre",
      G_CALLBACK (do_push_buffer_pre));
  gst_tracing_register_hook (tracer, "pad-push-post",
      G_CALLBACK (do_push_buffer_post));
  gst_tracing_register_hook (tracer, "pad-push-list-pre",
      G_CALLBACK (do_push_buffer_list_pre));
  gst_tracing_register_hook (tracer, "pad-push-list-post",
      G_CALLBACK (do_push_buffer_list_post));
  gst_tracing_register_hook (tracer, "pad-pull-range-pre",
      G_CALLBACK (do_pull_range_pre));
  gst_tracing_register_hook (tracer, "pad-pull-range-post",
      G_CALLBACK (do_pull_range_post));
  gst_tracing_register_hook (tracer, "pad-push-event-pre",
      G_CALLBACK (do_push_event_pre));
  gst_tracing_register_hook (tracer, "element-new",
      G_CALLBACK (do_element_new));
  gst_tracing_register_hook (tracer, "element-post-message-pre",
      G_CALLBACK (do_post_message_pre));
  gst_tracing_register_hook (tracer, "element-query-pre",
      G_CALLBACK (do_element_query_pre));
  gst_tracing_register_hook (tracer, "pad-query-pre",
      G_CALLBACK (do_query_pre));
  gst_tracing_register_hook (tracer, "pad-query-post",
      G_CALLBACK (do_query_post));
}
