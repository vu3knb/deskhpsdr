/* Copyright (C)
* 2019 - Christoph van Wüllen, DL1YCF
* 2024,2025 - Heiko Amft, DL1BZ (Project deskHPSDR)
*
*   This source code has been forked and was adapted from piHPSDR by DL1YCF to deskHPSDR in October 2024
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

/*
 * Layer-3 of MIDI support
 *
 * In most cases, a certain action only makes sense for a specific
 * type. For example, changing the VFO frequency will only be implemeted
 * for MIDI_WHEEL, and TUNE off/on only with MIDI_KNOB.
 *
 * However, changing the volume makes sense both with MIDI_KNOB and MIDI_WHEEL.
 */
#include <gtk/gtk.h>
#include <glib.h>

#include "actions.h"
#include "message.h"
#include "rigctl.h"
#include "midi.h"

VFO_TIMER vfo_timer  = { VFO,  0, 0, {0} };
VFO_TIMER vfoa_timer = { VFOA, 0, 0, {0} };
VFO_TIMER vfob_timer = { VFOB, 0, 0, {0} };

static int vfo_timeout_cb(gpointer data) {
  VFO_TIMER *timer = (VFO_TIMER *)data;
  g_mutex_lock(&timer->lock);
  t_print("%s: action=%d val=%d\n", __FUNCTION__, timer->action, timer->val);
  schedule_action(timer->action, RELATIVE, timer->val);
  timer->timeout = 0;
  timer->val = 0;
  g_mutex_unlock(&timer->lock);
  return FALSE;
}

void DoTheMidi(int action, enum ACTIONtype type, int val) {
  switch (type) {
  case MIDI_KEY:
    //t_print("%s: action=%d val=%d\n", __FUNCTION__, action, val);
    schedule_action(action, val ? PRESSED : RELEASED, 0);
    break;

  case MIDI_KNOB:
    //t_print("%s: action=%d val=%d\n", __FUNCTION__, action, val);
    schedule_action(action, ABSOLUTE, val);
    break;

  case MIDI_WHEEL:

    //
    // There are "big wheels" at various MIDI consoles that can produce MIDI events
    // with rather high frequency, and these are usually used for VFO, VFOA, VFOB
    // Therefore, these events are filtererd out here in a way that several such events
    // lead to a single scheduled action with a "consolidated" value. The idea is, that
    // turning the VFO knob fast will only generate one VFO update every 100 msec.
    //
    switch (action) {
    case VFOA:
      g_mutex_lock(&vfoa_timer.lock);
      vfoa_timer.val += val;

      if (vfoa_timer.timeout == 0) {
        vfoa_timer.timeout = g_timeout_add(100, vfo_timeout_cb, &vfoa_timer);
      }

      g_mutex_unlock(&vfoa_timer.lock);
      break;

    case VFOB:
      g_mutex_lock(&vfob_timer.lock);
      vfob_timer.val += val;

      if (vfob_timer.timeout == 0) {
        vfob_timer.timeout = g_timeout_add(100, vfo_timeout_cb, &vfob_timer);
      }

      g_mutex_unlock(&vfob_timer.lock);
      break;

    case VFO:
      g_mutex_lock(&vfo_timer.lock);
      vfo_timer.val += val;

      if (vfo_timer.timeout == 0) {
        vfo_timer.timeout = g_timeout_add(100, vfo_timeout_cb, &vfo_timer);
      }

      g_mutex_unlock(&vfo_timer.lock);
      break;

    default:
      if (rigctl_debug) { t_print("%s: action=%d val=%d\n", __FUNCTION__, action, val); }

      schedule_action(action, RELATIVE, val);
    }

    break;

  default:
    // other types cannot happen for MIDI
    break;
  }
}
