/* Copyright (C)
* 2024,2025 - Heiko Amft, DL1BZ (Project deskHPSDR)
*
*   This program is free software: you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation, either version 3 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program.  If not, see <https://www.gnu.org/licenses/>.
*
*/

#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <time.h>
#include <string.h>
#include <stdio.h>

#include "appearance.h"
#include "clock.h"

static GtkWidget *clock_widget = NULL;
static cairo_surface_t *clock_surface = NULL;
static int clock_width = 100;
static int clock_height = 60;

static gboolean
clock_configure_event_cb(GtkWidget *widget,
                         GdkEventConfigure *event,
                         gpointer data) {
  if (clock_surface) {
    cairo_surface_destroy(clock_surface);
  }
  clock_surface = gdk_window_create_similar_surface(gtk_widget_get_window(widget),
    CAIRO_CONTENT_COLOR, clock_width, clock_height);

  cairo_t *cr;
  cr = cairo_create(clock_surface);
  cairo_set_source_rgba(cr, COLOUR_VFO_BACKGND);
  cairo_paint(cr);
  cairo_destroy(cr);
  return TRUE;
}

static gboolean
clock_draw_cb(GtkWidget *widget, cairo_t *cr, gpointer data) {
  cairo_set_source_surface(cr, clock_surface, 0.0, 0.0);
  cairo_paint(cr);
  return FALSE;
}

static void clock_draw_display(void) {
  if (!clock_surface || !clock_widget) {
    return;
  }

  time_t now;
  struct tm *local_time;
  struct tm *utc_time;
  char utc_str[32];
  char local_str[32];
  char date_str[32];

  time(&now);
  local_time = localtime(&now);
  utc_time = gmtime(&now);

  strftime(utc_str, sizeof(utc_str), "%H:%M:%S", utc_time);
  strftime(local_str, sizeof(local_str), "%H:%M:%S", local_time);
  strftime(date_str, sizeof(date_str), "%Y-%m-%d", local_time);

  cairo_t *cr = cairo_create(clock_surface);

  cairo_set_source_rgba(cr, COLOUR_VFO_BACKGND);
  cairo_paint(cr);

  cairo_set_source_rgba(cr, COLOUR_BLACK);
  cairo_select_font_face(cr, DISPLAY_FONT_METER, CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);

  double x_margin = 8.0;
  double y_start = 6.0;
  double line_height = 14.0;

  cairo_set_font_size(cr, 10.0);
  cairo_set_source_rgba(cr, 0.4, 0.4, 0.4, 1.0);
  cairo_move_to(cr, x_margin, y_start);
  cairo_show_text(cr, "UTC");

  cairo_set_font_size(cr, 11.0);
  cairo_set_source_rgba(cr, COLOUR_BLACK);
  cairo_move_to(cr, x_margin + 25.0, y_start);
  cairo_show_text(cr, utc_str);

  cairo_set_font_size(cr, 10.0);
  cairo_set_source_rgba(cr, 0.4, 0.4, 0.4, 1.0);
  cairo_move_to(cr, x_margin, y_start + line_height);
  cairo_show_text(cr, "LOC");

  cairo_set_font_size(cr, 11.0);
  cairo_set_source_rgba(cr, COLOUR_BLACK);
  cairo_move_to(cr, x_margin + 25.0, y_start + line_height);
  cairo_show_text(cr, local_str);

  cairo_set_font_size(cr, 9.0);
  cairo_set_source_rgba(cr, 0.5, 0.5, 0.5, 1.0);
  cairo_move_to(cr, x_margin, y_start + 2.0 * line_height);
  cairo_show_text(cr, date_str);

  cairo_destroy(cr);

  gtk_widget_queue_draw(clock_widget);
}

GtkWidget *clock_init(int width, int height) {
  clock_width = width;
  clock_height = height;

  clock_widget = gtk_drawing_area_new();
  gtk_widget_set_size_request(clock_widget, width, height);

  g_signal_connect(clock_widget, "draw",
                   G_CALLBACK(clock_draw_cb), NULL);
  g_signal_connect(clock_widget, "configure-event",
                   G_CALLBACK(clock_configure_event_cb), NULL);

  gtk_widget_set_events(clock_widget, gtk_widget_get_events(clock_widget)
                        | GDK_EXPOSURE_MASK);

  clock_draw_display();

  return clock_widget;
}

void clock_update(void) {
  clock_draw_display();
}
