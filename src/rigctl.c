/* Copyright (C)
*  2016 Steve Wilson <wevets@gmail.com>
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

/* TS-2000 emulation via TCP */
/*
 * PiHPSDR RigCtl by Steve KA6S Oct 16 2016
 * With a kindly assist from Jae, K5JAE who has helped
 * greatly with hamlib integration!
 */
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <libgen.h>
#ifdef __APPLE__
  #include <mach-o/dyld.h>   // Für _NSGetExecutablePath
#endif
#include <limits.h>
#include "receiver.h"
#include "toolbar.h"
#include "band_menu.h"
#include "sliders.h"
#include "rigctl.h"
#include "radio.h"
#include "channel.h"
#include "filter.h"
#include "mode.h"
#include "filter.h"
#include "band.h"
#include "bandstack.h"
#include "filter_menu.h"
#include "vfo.h"
#include "transmitter.h"
#include "agc.h"
#include "store.h"
#include "ext.h"
#include "rigctl_menu.h"
#include "noise_menu.h"
#include "new_protocol.h"
#include "old_protocol.h"
#include "iambic.h"
#include "actions.h"
#include "new_menu.h"
#include "zoompan.h"
#include "message.h"
#include "startup.h"

#include <math.h>

#include <sys/socket.h>
#include <arpa/inet.h> //inet_addr
#include <netinet/tcp.h>

#include <netinet/in.h>
#include <pthread.h>
#include <json-c/json.h>

unsigned int rigctl_tcp_port = 19090;
volatile int rigctl_tcp_enable = 0;
int rigctl_tcp_andromeda = 0;
int rigctl_tcp_autoreporting = 0;
volatile int rigctld_enabled = 0;
volatile int use_rigctld = 0;

#if defined (__LDESK__)
  int serptt_fd;
  int sertune_fd;
  volatile gboolean serptt_cts = FALSE;
  int autogain_is_adjusted = 1;
#endif

// max number of bytes we can get at once
#define MAXDATASIZE 2000

gboolean rigctl_debug = FALSE;

int parse_cmd (void *data);

int cat_control = 0;

static GMutex mutex_numcat;   // only needed to make in/de-crements of "cat_control"  atomic

#define MAX_TCP_CLIENTS 3
#define MAX_ANDROMEDA_LEDS 16

static GThread *rigctl_server_thread_id = NULL;
static GThread *rigctl_cw_thread_id = NULL;
#if defined (__LDESK__)
  static GThread *serptt_thread_id = NULL;
  static GThread *sertune_thread_id = NULL;
  static GMutex sertune_mutex;
#endif
#if defined (__AUTOG__)
  pthread_t autogain_thread;
  pthread_mutex_t autogain_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex für Threadsicherheit
  volatile int autogain_thread_running = 0;
#endif

// Portnummer für die UDP Listener -> move to radio.c
static pthread_t rx200_listener_thread;  // Thread für den RX200 UDP Listener
static pthread_mutex_t rx200_array_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex für Threadsicherheit

static pthread_t lpf_listener_thread;  // Thread für den LPF UDP Listener
static pthread_mutex_t lpf_array_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex für Threadsicherheit

static pthread_t rigctld_thread;
static pthread_mutex_t rigctld_mutex = PTHREAD_MUTEX_INITIALIZER; // Mutex für Threadsicherheit
static pid_t rigctld_pid = 0;
extern char **environ;  // wird von posix_spawnp benötigt

static int tcp_running = 0;
static char rigctld_path[PATH_MAX];

static int server_socket = -1;
static struct sockaddr_in server_address;

typedef struct _client {
  int fd;
  int fifo;                         // serial only: this is a FIFO and not a true serial line
  int busy;                         // serial only (for FIFO handling)
  int done;                         // serial only (for FIFO handling)
  int running;                      // set this to zero to terminate client
  socklen_t address_length;         // TCP only: initialized by accept(), never used
  struct sockaddr_in address;       // TCP only: initialized by accept(), never used
  GThread *thread_id;               // ID of thread that serves the client
  guint andromeda_timer;            // for reporting ANDROMEDA LED states
  guint auto_timer;                 // for auto-reporting FA/FB
  int auto_reporting;               // auto-reporting (AI, ZZAI) 0...3
  int andromeda_type;               // 1:Andromeda, 4:G2Mk1 with CM5 upgrade, 5:G2 ultra
  int last_v;                       // Last push-button state received
  int last_fa, last_fb, last_md;    // last VFO-A/B frequency and VFO-A mode reported
  int last_led[MAX_ANDROMEDA_LEDS]; // last status of ANDROMEDA LEDs
} CLIENT;

//
// A G2V2 VFO encoder has 480 ticks per revolution and reports the number of ticks
// accumulated in 20 msec.
// Thus, slow/fast/veryfast tuning with 1/2/3 revolutions per second generates
// 9/19/29 ticks in one ZZZU/ZZZD command.
// Up to 1 rev/sec, no modification should occur. At 2 rev/sec, the tuning speed should
// be doubled, at 3 rev/sec and beyond it should be quadrupled.
//
// The following table maps input_ticks to output_ticks. If there are more than 30 input
// ticks in one ZZZU/ZZZD message, the number is quadrupled, and this is defined
// through the last entry in the table.
//
static uint8_t andromeda_vfo_speedup[32] = {  0,   1,   2,   3,   4,   5,   6,   7,
                                              8,   9,  11,  12,  14,  17,  19,  22,
                                              25,  29,  33,  38,  43,  48,  54,  61,
                                              69,  77,  85,  95, 105, 116, 128,   4
                                           };
typedef struct _command {
  CLIENT *client;
  char *command;
} COMMAND;

static CLIENT tcp_client[MAX_TCP_CLIENTS]; // TCP clients
static CLIENT serial_client[MAX_SERIAL];   // serial clienta
#if defined (__LDESK__)
  SERIALPORT SerialPorts[MAX_SERIAL + 2];
#else
  SERIALPORT SerialPorts[MAX_SERIAL];
#endif

static gpointer rigctl_client (gpointer data);

//
// This macro handles cases where RX2 is referred to but might not
// exist. These macros lead to an action only  if the RX exists.
// RXCHECK_ERR sets an error flag if RX is non-exisiting.
// RXCHECK     just silently ignores the command
//
#define RXCHECK_ERR(id, what) if (id >= 0 && id < receivers) { what; } else { implemented = FALSE; }
#define RXCHECK(id, what)     if (id >= 0 && id < receivers) { what; }

int rigctl_tcp_running() {
  return (server_socket >= 0);
}

#if defined (__LDESK__)
// Funktion zum Abrufen des CTS-Status der seriellen PTT
static gboolean get_serptt_cts(int fd) {
  int status;

  // Holen des CTS-Status von einem seriellen Gerät
  if (ioctl(fd, TIOCMGET, &status) < 0) {
    // Fehlerbehandlung
    return FALSE;  // Gibt FALSE zurück, wenn ein Fehler auftritt
  }

  // Überprüft, ob der CTS-Pin gesetzt ist
  return (status & TIOCM_CTS) != 0 ? TRUE : FALSE;
}

// Funktion zur Aktualisierung des CTS-Status der seriellen PTT
static gboolean update_serptt_cts(gpointer user_data) {
  gboolean current_state_serptt = GPOINTER_TO_INT(user_data);  // Umwandlung von gpointer zu gboolean

  // Wenn sich der Zustand von `serptt_cts` geändert hat
  if (current_state_serptt != serptt_cts) {
    serptt_cts = current_state_serptt;

    // Wenn PTT (Push To Talk) an ist
    if (serptt_cts) {
      t_print("%s: serial PTT ON\n", __FUNCTION__);
#if defined (__HAVEATU__)

      // Wenn der Sender gestimmt ist, wird eine Funktion mit Idle hinzugefügt
      if (transmitter->is_tuned) {
        g_idle_add(ext_mox_update, GINT_TO_POINTER(1));
      }

#else
      // Unabhängig von der Bedingung in #ifdef wird ext_mox_update mit 1 aufgerufen
      g_idle_add(ext_mox_update, GINT_TO_POINTER(1));
#endif
    } else {
      t_print("%s: serial PTT OFF\n", __FUNCTION__);
      // Wenn PTT aus ist, wird eine Funktion mit Timeout hinzugefügt
      g_timeout_add(50, ext_mox_update, GINT_TO_POINTER(0));
    }
  }

  return G_SOURCE_REMOVE;  // Einmalige Ausführung der Funktion
}

static gpointer monitor_serptt_cts_thread(gpointer user_data) {
  gboolean last_state_serptt = FALSE;  // Umstellung von bool auf gboolean
  int fd = *(int *)user_data;

  if (fd < 0) {
    if (SerialPorts[MAX_SERIAL + 1].enable) {
      SerialPorts[MAX_SERIAL + 1].enable = 0;
    }

    t_print("%s: ERROR open serial port %s failed\n", __FUNCTION__, SerialPorts[MAX_SERIAL + 1].port);
  }

  while (fd >= 0) {  // Solange fd gültig ist
    gboolean current_state_serptt = get_serptt_cts(fd);  // Get the current CTS state

    if (current_state_serptt != last_state_serptt) {
      last_state_serptt = current_state_serptt;
      g_idle_add(update_serptt_cts, GINT_TO_POINTER(current_state_serptt));  // Update if state changes
    }

    g_usleep(50000); // 50 ms warten
  }

  return NULL;
}

static gpointer monitor_sertune_thread(gpointer user_data) {
  int fd = *(int *)user_data;
  int status;

  if (fd < 0) {
    if (SerialPorts[MAX_SERIAL].enable) {
      SerialPorts[MAX_SERIAL].enable = 0;
    }

    t_print("%s: ERROR open serial port %s failed\n", __FUNCTION__, SerialPorts[MAX_SERIAL].port);
  }

  while (!(fd < 0)) {
    ioctl(fd, TIOCMGET, &status);          // Read state

    /*
    if (mox || vox || tune) {
      t_print("%s: MOX: %d VOX: %d TUNE: %d\n", __FUNCTION__, mox, vox, tune); // add debug output
    }
    */

    // if using TUNE we set RTS & DTR active,
    // if we transmit (MOX, VOX, TUNE) set DTR active, which can be used as external PTT output

    if (radio_is_transmitting()) {
      g_mutex_lock(&sertune_mutex);        // Lock thread

      if (tune) {
        status |= TIOCM_RTS;               // Set RTS active
        status |= TIOCM_DTR;               // Set DTR active
        ioctl(fd, TIOCMSET, &status);      // Write new state
      } else {
        if (SerialPorts[MAX_SERIAL].swapRtsDtr) {
          status |= TIOCM_RTS;             // Set RTS active instead DTR if RTS <-> DTR is swapped
        } else {
          status |= TIOCM_DTR;             // Set DTR active (default)
        }

        ioctl(fd, TIOCMSET, &status);      // Write new state
      }

      g_mutex_unlock(&sertune_mutex);      // Unlock thread
    } else {
      g_mutex_lock(&sertune_mutex);        // Lock thread
      status &= ~TIOCM_RTS;                // Clear RTS
      status &= ~TIOCM_DTR;                // Clear DTR
      ioctl(fd, TIOCMSET, &status);        // Set new state
      g_mutex_unlock(&sertune_mutex);      // Unlock thread
    }

    g_usleep(10000); // delay 10 ms
  }

  return NULL;
}

void launch_serptt() {
  if (SerialPorts[MAX_SERIAL + 1].enable) {
    serptt_fd = open(SerialPorts[MAX_SERIAL + 1].port, O_RDWR | O_NOCTTY | O_SYNC | O_NONBLOCK);

    if (serptt_fd < 0) {
      SerialPorts[MAX_SERIAL + 1].enable = 0;
      t_print("%s: ERROR open serial port %s failed\n", __FUNCTION__, SerialPorts[MAX_SERIAL + 1].port);
    } else {
      serptt_thread_id = g_thread_new("serPTT-Monitoring", monitor_serptt_cts_thread, &serptt_fd);
      t_print("---- LAUNCHING serPTT control Thread Id %d ----\n", serptt_thread_id);
    }
  } else {
    if (serptt_thread_id) {
      serptt_thread_id = NULL;
      close(serptt_fd);
      serptt_fd = -1;
      t_print("---- Shutdown SerPTT Thread at %s ----\n", SerialPorts[MAX_SERIAL + 1].port);
    }
  }
}

void launch_sertune() {
  int status_sertune;

  if (SerialPorts[MAX_SERIAL].enable) {
    sertune_fd = open(SerialPorts[MAX_SERIAL].port, O_RDWR | O_NOCTTY | O_SYNC | O_NONBLOCK);

    if (sertune_fd < 0) {
      SerialPorts[MAX_SERIAL].enable = 0;
      t_print("%s: ERROR open serial port %s failed\n", __FUNCTION__, SerialPorts[MAX_SERIAL].port);
    } else {
      ioctl(sertune_fd, TIOCMGET, &status_sertune);  // get state
      status_sertune &= ~TIOCM_RTS;                  // clear RTS
      status_sertune &= ~TIOCM_DTR;                  // clear DTR
      ioctl(sertune_fd, TIOCMSET, &status_sertune);  // set new state
      sertune_thread_id = g_thread_new("serTUNE-Monitoring", monitor_sertune_thread, &sertune_fd);
      t_print("---- LAUNCHING serTUNE control Thread at %s ----\n", SerialPorts[MAX_SERIAL].port);
    }
  } else {
    if (sertune_thread_id) {
      sertune_thread_id = NULL;
      close(sertune_fd);
      sertune_fd = -1;
      t_print("---- Shutdown serTUNE control Thread at %s ----\n", SerialPorts[MAX_SERIAL].port);
    }
  }
}

#endif

#if defined (__AUTOG__)
static void* autogain_thread_function(void* arg) {
  static struct timespec start_time, current_time, last_autogain_increase;
  static time_t elapsed_time;
  static int re_adjustment_time = 30; // in sec
  static int autogain_first_run = 1;  // only if deskHPSDR starts we get sometimes wrong ADC OVL states, we add a delay
  static double gain_step = 3.0;      // gain step size for aotomatic
  static double gain = 0.0;
  static double max_gain = 0.0;
  static double min_gain = 0.0;
  static unsigned int adc_count_limit = 4;  // count more than 1x ADC OVL as hyterese
  static unsigned int adc0_error_count = 0;
  static unsigned int adc1_error_count = 0;
  pthread_mutex_lock(&autogain_mutex);
  min_gain = adc[active_receiver->adc].min_gain;
  max_gain = adc[active_receiver->adc].max_gain;
  pthread_mutex_unlock(&autogain_mutex);
  /*
  In this thread we check, if the ADC0 or ADC1 runs in overflow because the gain is too much.
  The feedback come from the SDR itself, OVF signal will send inside the HPSDR protocol.
  If we receive a ADC0/ADC1 OVF, we reduce stepwise the gain every 0.5s with the defined gain_step.
  If the OVF flag is cleared, we stop with decreasing gain.
  If deskHPSDR starts the first time, we need to add an delay around 10-20s before the gain monitoring starts.
  That's why sometimes not all protocol initializations are completed just in time after app start
  This function needs more fine adjustment, not all is completed yet
  */
  clock_gettime(CLOCK_MONOTONIC, &start_time);              // get start time

  while (1) {
    clock_gettime(CLOCK_MONOTONIC, &current_time);          // get currect time
    elapsed_time = current_time.tv_sec - start_time.tv_sec; // calculate time difference in s

    if (elapsed_time > 10 && autogain_first_run) {          // set a delay of 10s
      autogain_first_run = !autogain_first_run;             // clear state of autogain_first_run
      t_print("%s: Clear state autogain_first_run = %d after %ds delay\n", __FUNCTION__, autogain_first_run,
              (int)elapsed_time);
    }

    if (!radio_is_transmitting() && !radio_ptt) {
      if (adc0_overload) {
        adc0_error_count++;   // if ADC0 OVL increase counter
      } else {
        adc0_error_count = 0; // reset counter
      }

      if (adc1_overload) {
        adc1_error_count++;   // if ADC1 OVL increase counter
      } else {
        adc1_error_count = 0; // reset counter
      }

      pthread_mutex_lock(&autogain_mutex);
      gain = adc[active_receiver->adc].gain; //get current gain from active receiver
      pthread_mutex_unlock(&autogain_mutex);

      if (gain > max_gain) {
        gain = max_gain;  // Sicherstellen, dass GAIN nicht größer als MAX_GAIN
      }

      if (!radio_is_transmitting() && !radio_ptt && adc0_error_count >= adc_count_limit && !autogain_first_run) {
        pthread_mutex_lock(&autogain_mutex);
        autogain_is_adjusted = 0;
        pthread_mutex_unlock(&autogain_mutex);
        g_idle_add(ext_vfo_update, NULL);

        while (!radio_is_transmitting() && !radio_ptt && adc0_overload && gain > min_gain) {
          gain -= gain_step; // decrease gain with gain_step

          if (gain < min_gain) {
            gain = min_gain;  // Sicherstellen, dass GAIN nicht kleiner MIN_GAIN
          }

          pthread_mutex_lock(&autogain_mutex);
          set_rf_gain(active_receiver->id, gain); // set new gain
          pthread_mutex_unlock(&autogain_mutex);
          g_usleep(500000);  // wait 0.5s
        }

        t_print("%s: RxPGA[RX%d] re-adjusted, new RxPGA gain is %+ddb\n", __FUNCTION__, active_receiver->id, (int)gain);
      }

      if (!radio_is_transmitting() && !radio_ptt && adc1_error_count >= adc_count_limit && !autogain_first_run) {
        while (!radio_is_transmitting() && !radio_ptt && adc1_overload && gain > min_gain) {
          gain -= gain_step; // decrease gain with gain_step

          if (gain < min_gain) {
            gain = min_gain;  // Sicherstellen, dass GAIN nicht kleiner MIN_GAIN
          }

          pthread_mutex_lock(&autogain_mutex);
          set_rf_gain(active_receiver->id, gain); // set new gain
          pthread_mutex_unlock(&autogain_mutex);
          g_usleep(500000); // wait 0.5s
        }
      }

      if (!radio_is_transmitting() && !radio_ptt && !adc0_overload && !autogain_is_adjusted && !autogain_first_run) {
        while (!radio_is_transmitting() && !radio_ptt && !adc0_overload && gain >= min_gain && gain < max_gain) {
          gain += 1.0;                                  // increase gain +1db
          pthread_mutex_lock(&autogain_mutex);
          set_rf_gain(active_receiver->id, gain);       // set gain
          pthread_mutex_unlock(&autogain_mutex);
          g_usleep(500000); // wait 0.5s
        }

        pthread_mutex_lock(&autogain_mutex);
        set_rf_gain(active_receiver->id, gain - 3.0); // decrease gain -3db
        autogain_is_adjusted = 1;
        pthread_mutex_unlock(&autogain_mutex);
        last_autogain_increase = current_time; // patch by DH0DM
        g_idle_add(ext_vfo_update, NULL);
        // g_usleep(1000000); // wait 0.5s
      }
    }

    // patch by DH0DM: add time-controlled adjustment
    if (autogain_time_enabled && autogain_is_adjusted) {
      if (current_time.tv_sec - last_autogain_increase.tv_sec > re_adjustment_time) {
        pthread_mutex_lock(&autogain_mutex);
        autogain_is_adjusted = 0;
        pthread_mutex_unlock(&autogain_mutex);
        t_print("%s: recall time-controlled autogain adjustment\n", __FUNCTION__);
      }
    }

    g_usleep(1000000); // statt sleep(1)
  }

  return NULL;
}

/*
void launch_autogain_hl2() {
  if (autogain_enabled) {
    if (pthread_create(&autogain_thread, NULL, autogain_thread_function, NULL) != 0) {
      t_perror("---- ERROR: cannot start autogain_thread ----\n"); // return EXIT_FAILURE;
    }

    t_print("---- LAUNCHING HL2 AutoGain Thread ----\n");
  } else {
    pthread_cancel(autogain_thread);
    t_print("---- Shutdown HL2 AutoGain Thread ----\n");
  }
}
*/
void launch_autogain_hl2() {
  if (autogain_enabled) {
    if (!autogain_thread_running) {
      autogain_thread_running = 1;

      if (pthread_create(&autogain_thread, NULL, autogain_thread_function, NULL) != 0) {
        t_print("%s: ERROR cannot start autogain_thread\n", __FUNCTION__);
      } else {
        t_print("---- LAUNCHING HL2 AutoGain Thread ----\n");
      }
    }
  } else {
    if (autogain_thread_running) {
      autogain_thread_running = 0;           // Stop-Signal setzen
      // pthread_join(autogain_thread, NULL);   // Auf sauberen Thread-Exit warten
      pthread_cancel(autogain_thread);
      t_print("---- Shutdown HL2 AutoGain Thread ----\n");
    }
  }
}

/*
void restart_autogain_hl2() {
  pthread_cancel(autogain_thread);
  t_print("%s\n", __FUNCTION__);
  launch_autogain_hl2();
}
*/
void restart_autogain_hl2() {
  if (autogain_thread_running) {
    autogain_thread_running = 0;           // Stop-Signal setzen
    // pthread_join(autogain_thread, NULL);   // Auf sauberen Thread-Exit warten
    pthread_cancel(autogain_thread);
    t_print("---- Stop HL2 AutoGain Thread ----\n");
  }

  autogain_thread_running = 1;             // Neustart vorbereiten

  if (pthread_create(&autogain_thread, NULL, autogain_thread_function, NULL) != 0) {
    autogain_thread_running = 0;
    t_print("%s: ERROR cannot start autogain_thread\n", __FUNCTION__);
  } else {
    t_print("---- Restart HL2 AutoGain Thread ----\n");
  }
}

#endif

// Callback-Funktion für empfangene RX200 UDP Daten
static void rx200_process_data(const char *rx200_data[7]) {
  // Daten von rx200_data in g_rx200_data thread-safe kopieren
  pthread_mutex_lock(&rx200_array_mutex); // Mutex sperren

  for (int i = 0; i < 4; i++) {
    if (rx200_data[i] != NULL) { // Sicherstellen, dass der Zeiger gültig ist
      g_strlcpy(g_rx200_data[i], rx200_data[i], sizeof(g_rx200_data[i]));
      // Keine manuelle Nullterminierung notwendig
    } else {
      g_rx200_data[i][0] = '\0'; // Leeren String setzen, falls NULL
    }
  }

  pthread_mutex_unlock(&rx200_array_mutex); // Mutex entsperren
  // t_print("RX200: Pfwd: %sW Pref: %sW SWR %s Timestamp: %s\n", g_rx200_data[0], g_rx200_data[1], g_rx200_data[2], g_rx200_data[3]);
}

// Callback-Funktion für empfangene LPF UDP Daten
static void lpf_process_data(const char *lpf_data[6]) {
  // Daten von lpf_data in g_lpf_data thread-safe kopieren
  pthread_mutex_lock(&lpf_array_mutex); // Mutex sperren

  for (int i = 0; i < 6; i++) {
    if (lpf_data[i] != NULL) { // Sicherstellen, dass der Zeiger gültig ist
      g_strlcpy(g_lpf_data[i], lpf_data[i], sizeof(g_lpf_data[i]));
      // Keine manuelle Nullterminierung notwendig
    } else {
      g_lpf_data[i][0] = '\0'; // Leeren String setzen, falls NULL
    }
  }

  pthread_mutex_unlock(&lpf_array_mutex); // Mutex entsperren
  // t_print("LPF: Band: %s PTT: %s\n", g_lpf_data[0], g_lpf_data[5]);
}

// RX200 UDP Listener-Funktion
static void *rx200_udp_listener(void *arg) {
  int port = *((int *)arg);
  int sockfd;
  struct sockaddr_in server_addr;
  char buffer[1024];

  // Socket erstellen
  if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    // perror("Socket creation failed");
    pthread_exit(NULL);
  }

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(port);
  // Socket an Port binden
  int opt = 1;
  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

  if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    t_perror("RX200 UDP Listener: socket bind failed\n");
    close(sockfd);
    pthread_exit(NULL);
  }

  t_print("%s: starting RX200 UDP-Listener at port %d\n", __FUNCTION__, port);

  // while (atomic_load(&toggle_listener)) { // Überprüfen, ob der Listener aktiv bleiben soll
  while (1) { // Überprüfen, ob der Listener aktiv bleiben soll, 1 = immer ohne Abbruchcondition
    int len = recvfrom(sockfd, buffer, sizeof(buffer), 0, NULL, NULL);

    if (len < 0) {
      t_print("%s: ERROR: invalid incoming UDP data\n", __FUNCTION__);
      rx200_udp_valid = 0;
      continue;
    }

    buffer[len] = '\0'; // Null-terminieren
    // JSON-Daten parsen und Callback aufrufen
    struct json_object *parsed_json = json_tokener_parse(buffer);

    if (parsed_json) {
      struct json_object *pwrfwd_rx200, *pwrref_rx200, *swr_rx200, *time_rx200, *rssi_rx200, *rst_rx200, *ssid_rx200;
      const char *data[7];
      json_object_object_get_ex(parsed_json, "pwrfwd", &pwrfwd_rx200);
      json_object_object_get_ex(parsed_json, "pwrref", &pwrref_rx200);
      json_object_object_get_ex(parsed_json, "swr", &swr_rx200);
      json_object_object_get_ex(parsed_json, "time", &time_rx200);
      json_object_object_get_ex(parsed_json, "rssi", &rssi_rx200);
      json_object_object_get_ex(parsed_json, "rst", &rst_rx200);
      json_object_object_get_ex(parsed_json, "ssid", &ssid_rx200);
      data[0] = json_object_get_string(pwrfwd_rx200);
      data[1] = json_object_get_string(pwrref_rx200);
      data[2] = json_object_get_string(swr_rx200);
      data[3] = json_object_get_string(time_rx200);
      data[4] = json_object_get_string(rssi_rx200);
      data[5] = json_object_get_string(rst_rx200);
      data[6] = json_object_get_string(ssid_rx200);
      rx200_process_data(data); // Callback aufrufen
      rx200_udp_valid = 1;
      json_object_put(parsed_json); // Speicher freigeben
    } else {
      t_print("%s: RX200: invalid JSON data received: %s\n", __FUNCTION__, buffer);
    }
  }

  t_print("RX200: UDP-Listener stopped...\n");
  close(sockfd);
  pthread_exit(NULL);
}

// LPF UDP Listener-Funktion
static void *lpf_udp_listener(void *arg) {
  int port = *((int *)arg);
  int sockfd;
  struct sockaddr_in server_addr;
  char buffer[1024];

  // Socket erstellen
  if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
    // perror("Socket creation failed");
    pthread_exit(NULL);
  }

  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(port);
  // Socket an Port binden
  int opt = 1;
  setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));

  if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
    t_perror("LPF UDP Listener: socket bind failed\n");
    close(sockfd);
    pthread_exit(NULL);
  }

  t_print("%s: starting LPF UDP-Listener at port %d\n", __FUNCTION__, port);

  // while (atomic_load(&toggle_listener)) { // Überprüfen, ob der Listener aktiv bleiben soll
  while (1) { // Überprüfen, ob der Listener aktiv bleiben soll, 1 = immer ohne Abbruchcondition
    int len = recvfrom(sockfd, buffer, sizeof(buffer), 0, NULL, NULL);

    if (len < 0) {
      t_print("%s: ERROR: invalid incoming UDP data\n", __FUNCTION__);
      lpf_udp_valid = 0;
      continue;
    }

    buffer[len] = '\0'; // Null-terminieren
    // JSON-Daten parsen und Callback aufrufen
    struct json_object *parsed_json = json_tokener_parse(buffer);

    if (parsed_json) {
      struct json_object *band_lpf, *time_lpf, *rssi_lpf, *rst_lpf, *ssid_lpf, *ptt_lpf;
      const char *data[6];
      json_object_object_get_ex(parsed_json, "band", &band_lpf);
      json_object_object_get_ex(parsed_json, "time", &time_lpf);
      json_object_object_get_ex(parsed_json, "rssi", &rssi_lpf);
      json_object_object_get_ex(parsed_json, "rst", &rst_lpf);
      json_object_object_get_ex(parsed_json, "ssid", &ssid_lpf);
      json_object_object_get_ex(parsed_json, "ptt", &ptt_lpf);
      data[0] = json_object_get_string(band_lpf);
      data[1] = json_object_get_string(time_lpf);
      data[2] = json_object_get_string(rssi_lpf);
      data[3] = json_object_get_string(rst_lpf);
      data[4] = json_object_get_string(ssid_lpf);
      data[5] = json_object_get_string(ptt_lpf);
      lpf_process_data(data); // Callback aufrufen
      lpf_udp_valid = 1;
      json_object_put(parsed_json); // Speicher freigeben
    } else {
      t_print("%s: LPF: invalid JSON data received: %s\n", __FUNCTION__, buffer);
    }
  }

  t_print("LPF: UDP-Listener stopped...\n");
  close(sockfd);
  pthread_exit(NULL);
}

static pid_t get_pid_by_name(const char* process_name) {
  char command[256];
  // -n = neueste (letzte gestartete) Instanz
  // snprintf(command, sizeof(command), "pgrep -n %s", process_name);
  // -f durchsucht die ganze Kommandozeile, nicht nur die ersten 15 Zeichen
  snprintf(command, sizeof(command), "pgrep -n -f '%s'", process_name);
  FILE* fp = popen(command, "r");

  if (!fp) { return 0; }

  pid_t pid = 0;
  fscanf(fp, "%d", &pid);  // Lies die erste Zeile (PID)
  pclose(fp);
  return pid;
}

static char* find_in_path(const char* binary_name) {
  const char* paths[] = {
    "/usr/local/bin",
    "/usr/bin",
    "/opt/bin",
    "/bin",
    workdir,
    NULL
  };

  for (int i = 0; paths[i] != NULL; i++) {
    char fullpath[512];
    snprintf(fullpath, sizeof(fullpath), "%s/%s", paths[i], binary_name);

    if (access(fullpath, X_OK) == 0) {
      return strdup(fullpath);  // Rückgabe → muss später mit free() freigegeben werden
    }
  }

  return NULL;  // nicht gefunden
}

#ifdef __APPLE__
static int start_from_app_bundle() {
  char exe_path[PATH_MAX];
  uint32_t size = sizeof(exe_path);

  if (_NSGetExecutablePath(exe_path, &size) != 0) {
    // Fehler beim Ermitteln des Pfads
    return 0;
  }

  // Prüfen auf typische App-Bundle-Pfadstruktur
  if (strstr(exe_path, ".app/Contents/MacOS") != NULL) {
    return 1;
  } else {
    return 0;
  }
}

// Funktion, um den Pfad zur rigctld_deskhpsdr zu ermitteln
static char* mac_get_rigctld_path() {
  if (start_from_app_bundle()) {
    t_print("%s: macOS: %s start from .app bundle.\n", __FUNCTION__, PGNAME);
    char exec_path[PATH_MAX];
    uint32_t size = sizeof(exec_path);

    if (_NSGetExecutablePath(exec_path, &size) != 0) {
      t_perror("_NSGetExecutablePath fehlgeschlagen\n");
      return NULL;
    }

    char *exec_dir = dirname(exec_path);
    snprintf(rigctld_path, sizeof(rigctld_path), "%s/../Resources/rigctld_deskhpsdr", exec_dir);

    if (access(rigctld_path, X_OK) != 0) {
      t_perror("rigctld_deskhpsdr nicht gefunden oder nicht ausführbar\n");
      return NULL;
    }

    return rigctld_path;
  } else {
    t_print("%s: macOS: %s start from command line.\n", __FUNCTION__, PGNAME);
    return find_in_path("rigctld_deskhpsdr");
  }
}
#endif

static void start_rigctld() {
  char rigctld_target_port[16];
  snprintf(rigctld_target_port, sizeof(rigctld_target_port), ":%u", rigctl_tcp_port);
  t_print("%s: rigctld_target_port is %s\n", __FUNCTION__, rigctld_target_port);
  pid_t running_pid = get_pid_by_name("rigctld_deskhpsdr");

  if (running_pid > 0) {
    t_print("%s: Stop old rigctld (PID %d)...\n", __FUNCTION__, running_pid);
    kill(running_pid, SIGTERM);
    waitpid(running_pid, NULL, 0);
    rigctld_pid = 0;
  }

  if (rigctld_pid != 0) { return; }

#ifdef __APPLE__
  // Get rigctld path using helper function
  char *rigctld_path = mac_get_rigctld_path();
#else
  char *rigctld_path = find_in_path("rigctld_deskhpsdr");
#endif

  if (!rigctld_path || access(rigctld_path, X_OK) != 0) {
    rigctld_enabled = 0;
    t_perror("rigctld_deskhpsdr nicht gefunden oder nicht ausführbar\n");
    return;
  }

  t_print("%s: rigctld_deskhpsdr gefunden: %s\n", __FUNCTION__, rigctld_path);
  char *args[] = {
    rigctld_path,
    "-t", "4533",
    "-m", "2040",
    "-r", rigctld_target_port,
    NULL
  };
  pid_t pid;
  int status = posix_spawn(&pid, rigctld_path, NULL, NULL, args, environ);

  if (status == 0) {
    rigctld_pid = pid;
    t_print("%s: rigctld gestartet mit PID %d\n", __FUNCTION__, pid);
  } else {
    rigctld_enabled = 0;
    t_perror("posix_spawn fehlgeschlagen\n");
  }
}

// Funktion zum Stoppen von rigctld
void stop_rigctld() {
  if (rigctld_pid == 0) { return; }  // Läuft nicht

  t_print("%s:Stoppe rigctld (PID %d)...\n", __FUNCTION__, rigctld_pid);
  kill(rigctld_pid, SIGTERM);  // Oder SIGKILL bei Bedarf
  waitpid(rigctld_pid, NULL, 0);  // Warten bis beendet
  rigctld_pid = 0;
}

static void* rigctld_control_thread(void* arg) {
  while (1) {
    pthread_mutex_lock(&rigctld_mutex);
    int enabled = rigctld_enabled;
    pthread_mutex_unlock(&rigctld_mutex);

    if (enabled && rigctld_pid == 0) {
      start_rigctld();
    } else if (!enabled && rigctld_pid != 0) {
      stop_rigctld();
    }

    sleep(1);  // Sekündlich prüfen
  }

  return NULL;
}

void launch_rx200_monitor() {
  t_print("---- LAUNCHING RX200 UDP Monitor ----\n", __FUNCTION__);

  // RX200 UDP Listener-Thread starten
  if (pthread_create(&rx200_listener_thread, NULL, rx200_udp_listener, &rx200_udp_port) != 0) {
    t_perror("ERROR: cannot start RX200 UDP Listener thread\n");
    // return EXIT_FAILURE;
  }
}

// Funktion zum Starten des Steuer-Threads
void launch_rigctld_monitor() {
  if (use_rigctld) {
    t_print("---- LAUNCHING RIGCTLD SERVER ----\n", __FUNCTION__);

    if (pthread_create(&rigctld_thread, NULL, rigctld_control_thread, NULL) != 0) {
      t_perror("ERROR: cannot start rigctld thread\n");
      // exit(EXIT_FAILURE);
    }

    pthread_detach(rigctld_thread);
  }
}

void launch_lpf_monitor() {
  t_print("---- LAUNCHING LPF UDP Monitor ----\n", __FUNCTION__);

  // LPF UDP Listener-Thread starten
  if (pthread_create(&lpf_listener_thread, NULL, lpf_udp_listener, &lpf_udp_port) != 0) {
    t_perror("ERROR: cannot start LPF UDP Listener thread\n");
    // return EXIT_FAILURE;
  }
}

void shutdown_tcp_rigctl() {
  struct linger linger = { 0 };
  linger.l_onoff = 1;
  linger.l_linger = 0;
  t_print("%s: server_socket=%d\n", __FUNCTION__, server_socket);
  tcp_running = 0;
  rigctld_enabled = 0;

  //
  // Gracefully terminate all active TCP connections
  //
  for (int id = 0; id < MAX_TCP_CLIENTS; id++) {
    if (tcp_client[id].andromeda_timer != 0) {
      g_source_remove(tcp_client[id].andromeda_timer);
      tcp_client[id].andromeda_timer = 0;
    }

    if (tcp_client[id].auto_timer != 0) {
      g_source_remove(tcp_client[id].auto_timer);
      tcp_client[id].auto_timer = 0;
    }

    tcp_client[id].running = 0;

    if (tcp_client[id].fd != -1) {
      // t_print("%s: setting SO_LINGER to 0 for client_socket: %d\n", __FUNCTION__, tcp_client[id].fd);
      if (setsockopt(tcp_client[id].fd, SOL_SOCKET, SO_LINGER, (const char *)&linger, sizeof(linger)) == -1) {
        t_perror("setsockopt(...,SO_LINGER,...) failed for client:");
      }

      t_print("%s: closing client socket: %d\n", __FUNCTION__, tcp_client[id].fd);
      close(tcp_client[id].fd);
      tcp_client[id].fd = -1;
    }

    if (tcp_client[id].thread_id) {
      g_thread_join(tcp_client[id].thread_id);
      tcp_client[id].thread_id = NULL;
    }
  }

  //
  // Close server socket
  //
  if (server_socket >= 0) {
    // t_print("%s: setting SO_LINGER to 0 for server_socket: %d\n", __FUNCTION__, server_socket);
    if (setsockopt(server_socket, SOL_SOCKET, SO_LINGER, (const char *)&linger, sizeof(linger)) == -1) {
      t_perror("setsockopt(...,SO_LINGER,...) failed for server:");
    }

    t_print("%s: closing server_socket: %d\n", __FUNCTION__, server_socket);
    close(server_socket);
    server_socket = -1;
  }

  // TODO: join with the server thread, but this requires to make the accept() there
  //       non-blocking (use select())
}

//
//  CW ring buffer
//

#define CW_BUF_SIZE 80
static char cw_buf[CW_BUF_SIZE];
static int  cw_buf_in = 0, cw_buf_out = 0;

static int dotsamples;
static int dashsamples;

//
// send_dash()         send a "key-down" of a dashlen, followed by a "key-up" of a dotlen
// send_dot()          send a "key-down" of a dotlen,  followed by a "key-up" of a dotlen
// send_space(int len) send a "key_down" of zero,      followed by a "key-up" of len*dotlen
//
// The "trick" to get proper timing is, that we really specify  the number of samples
// for the next element (dash/dot/nothing) and the following pause. 30 wpm is no
// problem, and without too much "busy waiting". We just take a nap until 10 msec
// before we have to act, and then wait several times for 1 msec until we can shoot.
//
static void send_dash() {
  for (;;) {
    int TimeToGo = cw_key_up + cw_key_down;

    // TimeToGo is invalid if local CW keying has set in
    if (cw_key_hit || cw_not_ready) { return; }

    if (TimeToGo == 0) { break; }

    // sleep until 10 msec before ignition
    if (TimeToGo > 500) { usleep((long)(TimeToGo - 500) * 20L); }

    // sleep 1 msec
    usleep(1000L);
  }

  // If local CW keying has set in, do not interfere
  if (cw_key_hit || cw_not_ready) { return; }

  cw_key_down = dashsamples;
  cw_key_up   = dotsamples;
}

static void send_dot() {
  for (;;) {
    int TimeToGo = cw_key_up + cw_key_down;

    // TimeToGo is invalid if local CW keying has set in
    if (cw_key_hit || cw_not_ready) { return; }

    if (TimeToGo == 0) { break; }

    // sleep until 10 msec before ignition
    if (TimeToGo > 500) { usleep((long)(TimeToGo - 500) * 20L); }

    // sleep 1 msec
    usleep(1000L);
  }

  // If local CW keying has set in, do not interfere
  if (cw_key_hit || cw_not_ready) { return; }

  cw_key_down = dotsamples;
  cw_key_up   = dotsamples;
}

static void send_space(int len) {
  for (;;) {
    int TimeToGo = cw_key_up + cw_key_down;

    // TimeToGo is invalid if local CW keying has set in
    if (cw_key_hit || cw_not_ready) { return; }

    if (TimeToGo == 0) { break; }

    // sleep until 10 msec before ignition
    if (TimeToGo > 500) { usleep((long)(TimeToGo - 500) * 20L); }

    // sleep 1 msec
    usleep(1000L);
  }

  // If local CW keying has set in, do not interfere
  if (cw_key_hit || cw_not_ready) { return; }

  cw_key_up = len * dotsamples;
}

//
// This stores the "buffered join character" status
//
static int join_cw_characters = 0;

static void rigctl_send_cw_char(char cw_char) {
  char pattern[9], *ptr;
  ptr = &pattern[0];

  switch (cw_char) {
  case 'a':
  case 'A':
    g_strlcpy(pattern, ".-", 9);
    break;

  case 'b':
  case 'B':
    g_strlcpy(pattern, "-...", 9);
    break;

  case 'c':
  case 'C':
    g_strlcpy(pattern, "-.-.", 9);
    break;

  case 'd':
  case 'D':
    g_strlcpy(pattern, "-..", 9);
    break;

  case 'e':
  case 'E':
    g_strlcpy(pattern, ".", 9);
    break;

  case 'f':
  case 'F':
    g_strlcpy(pattern, "..-.", 9);
    break;

  case 'g':
  case 'G':
    g_strlcpy(pattern, "--.", 9);
    break;

  case 'h':
  case 'H':
    g_strlcpy(pattern, "....", 9);
    break;

  case 'i':
  case 'I':
    g_strlcpy(pattern, "..", 9);
    break;

  case 'j':
  case 'J':
    g_strlcpy(pattern, ".---", 9);
    break;

  case 'k':
  case 'K':
    g_strlcpy(pattern, "-.-", 9);
    break;

  case 'l':
  case 'L':
    g_strlcpy(pattern, ".-..", 9);
    break;

  case 'm':
  case 'M':
    g_strlcpy(pattern, "--", 9);
    break;

  case 'n':
  case 'N':
    g_strlcpy(pattern, "-.", 9);
    break;

  case 'o':
  case 'O':
    g_strlcpy(pattern, "---", 9);
    break;

  case 'p':
  case 'P':
    g_strlcpy(pattern, ".--.", 9);
    break;

  case 'q':
  case 'Q':
    g_strlcpy(pattern, "--.-", 9);
    break;

  case 'r':
  case 'R':
    g_strlcpy(pattern, ".-.", 9);
    break;

  case 's':
  case 'S':
    g_strlcpy(pattern, "...", 9);
    break;

  case 't':
  case 'T':
    g_strlcpy(pattern, "-", 9);
    break;

  case 'u':
  case 'U':
    g_strlcpy(pattern, "..-", 9);
    break;

  case 'v':
  case 'V':
    g_strlcpy(pattern, "...-", 9);
    break;

  case 'w':
  case 'W':
    g_strlcpy(pattern, ".--", 9);
    break;

  case 'x':
  case 'X':
    g_strlcpy(pattern, "-..-", 9);
    break;

  case 'y':
  case 'Y':
    g_strlcpy(pattern, "-.--", 9);
    break;

  case 'z':
  case 'Z':
    g_strlcpy(pattern, "--..", 9);
    break;

  case '0':
    g_strlcpy(pattern, "-----", 9);
    break;

  case '1':
    g_strlcpy(pattern, ".----", 9);
    break;

  case '2':
    g_strlcpy(pattern, "..---", 9);
    break;

  case '3':
    g_strlcpy(pattern, "...--", 9);
    break;

  case '4':
    g_strlcpy(pattern, "....-", 9);
    break;

  case '5':
    g_strlcpy(pattern, ".....", 9);
    break;

  case '6':
    g_strlcpy(pattern, "-....", 9);
    break;

  case '7':
    g_strlcpy(pattern, "--...", 9);
    break;

  case '8':
    g_strlcpy(pattern, "---..", 9);
    break;

  case '9':
    g_strlcpy(pattern, "----.", 9);
    break;

  //
  //     DL1YCF:
  //     added some signs from ITU Recommendation M.1677-1 (2009)
  //     in the order given there.
  //
  case '.':
    g_strlcpy(pattern, ".-.-.-", 9);
    break;

  case ',':
    g_strlcpy(pattern, "--..--", 9);
    break;

  case ':':
    g_strlcpy(pattern, "---..", 9);
    break;

  case '?':
    g_strlcpy(pattern, "..--..", 9);
    break;

  case '\'':
    g_strlcpy(pattern, ".----.", 9);
    break;

  case '-':
    g_strlcpy(pattern, "-....-", 9);
    break;

  case '/':
    g_strlcpy(pattern, "-..-.", 9);
    break;

  case '(':
    g_strlcpy(pattern, "-.--.", 9);
    break;

  case ')':
    g_strlcpy(pattern, "-.--.-", 9);
    break;

  case '"':
    g_strlcpy(pattern, ".-..-.", 9);
    break;

  case '=':
    g_strlcpy(pattern, "-...-", 9);
    break;

  case '+':
    g_strlcpy(pattern, ".-.-.", 9);
    break;

  case '@':
    g_strlcpy(pattern, ".--.-.", 9);
    break;

  //
  //     Often used, but not ITU: Ampersand for "wait"
  //
  case '&':
    g_strlcpy(pattern, ".-...", 9);
    break;

  default:
    g_strlcpy(pattern, "", 9);
  }

  while (*ptr != '\0') {
    if (*ptr == '-') {
      send_dash();
    }

    if (*ptr == '.') {
      send_dot();
    }

    ptr++;
  }

  // The last element (dash or dot) sent already has one dotlen space appended.
  // If the current character is another "printable" sign, we need an additional
  // pause of 2 dotlens to form the inter-character spacing of 3 dotlens.
  // However if the current character is a "space" we must produce an inter-word
  // spacing (7 dotlens) and therefore need 6 additional dotlens
  // We need no longer take care of a sequence of spaces since adjacent spaces
  // are now filtered out while filling the CW character (ring-) buffer.
  if (cw_char == ' ') {
    send_space(6);  // produce inter-word space of 7 dotlens
  } else {
    if (!join_cw_characters) { send_space(2); }  // produce inter-character space of 3 dotlens
  }
}

//
// rigctl_cw_thread is started once and runs forever,
// checking for data in the CW ring buffer and sending it.
//
static gpointer rigctl_cw_thread(gpointer data) {
  int i;
  char cwchar;
  int  buffered_speed = 0;
  int  bracket_command = 0;

  for (;;) {
    // wait for CW data (periodically look every 100 msec)
    if (cw_buf_in == cw_buf_out) {
      cw_key_hit = 0;
      usleep(100000L);
      continue;
    }

    //
    // if TX mode is not CW, drain ring buffer
    //
    int txmode = vfo_get_tx_mode();

    if (txmode != modeCWU && txmode != modeCWL) {
      cw_buf_out = cw_buf_in;
      continue;
    }

    //
    // Take one character from the ring buffer
    //
    cwchar = cw_buf[cw_buf_out];
    i = cw_buf_out + 1;

    if (i >= CW_BUF_SIZE) { i = 0; }

    cw_buf_out = i;

    //
    // Special character sequences or characters:
    //
    //  [+         Increase speed by 25 %
    //  [-         Decrease speed by 25 %
    //  [          Join Characters
    //  ]          End speed change or joining
    //
    if (bracket_command)  {
      switch (cwchar) {
      case '+':
        buffered_speed = (5 * cw_keyer_speed) / 4;
        cwchar = 0;
        break;

      case '-':
        buffered_speed = (3 * cw_keyer_speed) / 4;
        cwchar = 0;
        break;

      case '.':
        join_cw_characters = 1;
        cwchar = 0;
        break;
      }

      bracket_command = 0;
    }

    if (cwchar == '[') {
      bracket_command = 1;
      cwchar = 0;
    }

    if (cwchar == ']') {
      buffered_speed = 0;
      join_cw_characters = 0;
      cwchar = 0;
    }

    // The dot and dash length may have changed, so recompute them here
    // This means that we can change the speed (KS command) while
    // the buffer is being sent
    if (buffered_speed > 0) {
      dotsamples = 57600 / buffered_speed;
      dashsamples = (3456 * cw_keyer_weight) / buffered_speed;
    } else {
      dotsamples = 57600 / cw_keyer_speed;
      dashsamples = (3456 * cw_keyer_weight) / cw_keyer_speed;
    }

    CAT_cw_is_active = 1;
    schedule_transmit_specific();

    if (!mox) {
      // activate PTT
      g_idle_add(ext_mox_update, GINT_TO_POINTER(1));
      // have to wait until it is really there
      // Note that if out-of-band, we would wait
      // forever here, so allow at most 200 msec
      // We also have to wait for cw_not_ready becoming zero
      i = 200;

      while ((!mox || cw_not_ready) && i-- > 0) { usleep(1000L); }

      // still no MOX? --> silently discard CW character and give up
      if (!mox) {
        CAT_cw_is_active = 0;
        schedule_transmit_specific();
        continue;
      }
    }

    // At this point, mox == 1 and CAT_cw_active == 1
    if (cw_key_hit || cw_not_ready) {
      //
      // CW transmission has been aborted, either due to manually
      // removing MOX, changing the mode to non-CW, or because a CW key has been hit.
      // Do not remove PTT in the latter case
      buffered_speed = 0;
      CAT_cw_is_active = 0;
      schedule_transmit_specific();

      // If a CW key has been hit, we continue in TX mode.
      // This also applies if we have an active foot-switch
      // Otherwise, switch PTT off.
      if (!cw_key_hit && mox && !radio_ptt) {
        g_idle_add(ext_mox_update, GINT_TO_POINTER(0));
      }

      //
      // keep draining ring buffer until it stays empty for 0.5 seconds
      // This is necessary: after aborting a very long CW
      // text such as a CQ call by hitting a Morse key,
      // CW characters may flow in for quite a while.
      //
      do {
        cw_buf_out = cw_buf_in;
        usleep(500000L);
      } while (cw_buf_out != cw_buf_in);
    } else {
      if (cwchar) { rigctl_send_cw_char(cwchar); }

      //
      // Character has been sent, so continue.
      // Since the second character possibly comes 250 msec after
      // the first one, we have to wait if the buffer stays
      // empty. Only then, stop CAT CW.
      //
      for (i = 0; i < 5; i++ ) {
        if (cw_buf_in != cw_buf_out) { break; }

        usleep(50000);
      }

      if (cw_buf_in != cw_buf_out) { continue; }

      CAT_cw_is_active = 0;
      schedule_transmit_specific();

      if (!cw_key_hit && !radio_ptt) {
        g_idle_add(ext_mox_update, GINT_TO_POINTER(0));
        // wait up to 500 msec for MOX having gone
        // otherwise there might be a race condition when sending
        // the next character really soon
        i = 10;

        while (mox && (i--) > 0) { usleep(50000L); }

        buffered_speed = 0;
      }
    }
  }

  // NOTREACHED (now this thread is started once-and-for-all)

  // We arrive here if the rigctl server shuts down.
  // This very rarely happens. But we should shut down the
  // local CW system gracefully, in case we were in the mid
  // of a transmission
  if (CAT_cw_is_active) {
    CAT_cw_is_active = 0;
    schedule_transmit_specific();
    g_idle_add(ext_mox_update, GINT_TO_POINTER(0));
  }

  rigctl_cw_thread_id = NULL;
  return NULL;
}

static void send_resp (int fd, char * msg) {
  //
  // send_resp is ONLY called from within the GTK event queue
  // ==> no multi-thread problems can occur.
  //
  if (fd == -1) {
    //
    // This means the client fd has been explicitly closed
    // in the mean time. Silently give up and do not
    // emit an error message.
    //
    return;
  }

  if (rigctl_debug) { t_print("RIGCTL: RESP=%s\n", msg); }

  int length = strlen(msg);
  int count = 0;

  while (length > 0) {
    //
    // Since this is in the GTK event queue, we cannot try
    // for a long time. In case of an error (rc < 0) we give
    // up immediately, for rc == 0 we try at most 10 times.
    //
    int rc = write(fd, msg, length);

    if (rc < 0) { return; }

    if (rc == 0) {
      count++;

      if (count > 10) { return; }
    }

    length -= rc;
    msg += rc;
  }
}

static int wdspmode(int kenwoodmode) {
  int wdspmode;

  switch (kenwoodmode) {
  case 1:                // Kenwood LSB
    wdspmode = modeLSB;
    break;

  case 2:                // Kenwood USB
    wdspmode = modeUSB;
    break;

  case 3:                // Kenwood CW (upper side band)
    wdspmode = modeCWU;
    break;

  case 4:                // Kenwood FM
    wdspmode = modeFMN;
    break;

  case 5:                // Kenwood AM
    wdspmode = modeAM;
    break;

  case 6:                // Kenwood FSK (lower side band)
    wdspmode = modeDIGL;
    break;

  case 7:                // Kenwood CW-R (lower side band)
    wdspmode = modeCWL;
    break;

  case 9:                // Kenwood FSK-R (upper side band)
    wdspmode = modeDIGU;
    break;

  default:
    // NOTREACHED?
    wdspmode = modeLSB;
    break;
  }

  return wdspmode;
}

static int ts2000_mode(int wdspmode) {
  int kenwoodmode;

  switch (wdspmode) {
  case modeLSB:
    kenwoodmode = 1;  // Kenwood LDB
    break;

  case modeUSB:
    kenwoodmode = 2;  // Kenwood USB
    break;

  case modeCWL:
    kenwoodmode = 7;  // Kenwood CW-R
    break;

  case modeCWU:
    kenwoodmode = 3;  // Kenwood CW
    break;

  case modeFMN:
    kenwoodmode = 4;  // Kenwood FM
    break;

  case modeAM:
  case modeSAM:
    kenwoodmode = 5;  // Kenwood AM
    break;

  case modeDIGL:
    kenwoodmode = 6;  // Kenwood FSK
    break;

  case modeDIGU:
    kenwoodmode = 9;  // Kenwood FSK-R
    break;

  default:
    // NOTREACHED?
    kenwoodmode = 1;  // LSB
    break;
  }

  return kenwoodmode;
}

static gboolean autoreport_handler(gpointer data) {
  CLIENT *client = (CLIENT *) data;
  //
  // This function is repeatedly called as long as the CAT
  // connection is active. It reports VFOA and VFOB frequency changes
  // to the client, provided it has auto-reporting enabled and is running.
  //
  // Note this runs in the GTK event queue so it cannot interfere
  // with another CAT command.
  // Auto-reporting to a FIFO is suppressed because all data sent there will
  // be echoed back and then be read again.
  //

  if (client->fifo || !client->running) {
    //
    // return and remove timer
    //
    return FALSE;
  }

  if (client->auto_reporting > 0) {
    long long fa = vfo[VFO_A].ctun ? vfo[VFO_A].ctun_frequency : vfo[VFO_A].frequency;
    long long fb = vfo[VFO_B].ctun ? vfo[VFO_B].ctun_frequency : vfo[VFO_B].frequency;

    if (fa != client->last_fa) {
      char reply[256];
      snprintf(reply, 256, "FA%011lld;", fa);
      send_resp(client->fd, reply);
      client->last_fa = fa;
    }

    if (fb != client->last_fb) {
      char reply[256];
      snprintf(reply, 256, "FB%011lld;", fb);
      send_resp(client->fd, reply);
      client->last_fb = fb;
    }
  }

  if (client->auto_reporting > 1) {
    int md = vfo[VFO_A].mode;

    if (md != client->last_md) {
      char reply[256];
      snprintf(reply, 256, "MD%1d;", ts2000_mode(md));
      send_resp(client->fd, reply);
      client->last_md = md;
    }
  }

  return TRUE;
}

static gboolean andromeda_handler(gpointer data) {
  //
  // This function is repeatedly called as long as the client runs
  //
  //
  CLIENT *client = (CLIENT *)data;
  char reply[256];

  if (!client->running || client->andromeda_type == 4) {
    //
    // If the client is no longer running, remove source.
    // The same applies for ANDROMEDA type-4 clients since there
    // are no LEDs on a G2MkI panel
    //
    return G_SOURCE_REMOVE;
  }

  //
  // Do not proceed until Andromeda version is known
  // Send a ZZZS command and re-trigger the handler
  //
  if (client->andromeda_type < 1) {
    snprintf(reply, 256, "ZZZS;");
    send_resp(client->fd, reply);
    return TRUE;
  }

  for (int led = 0; led < MAX_ANDROMEDA_LEDS; led++) {
    int new = client->last_led[led];

    if (client->andromeda_type == 1) {
      //
      // Original ANDROMEDA console
      //
      switch (led) {
      case 1:
        new = mox;
        break;

      case 2:
        // ATU has TUNE solution
        break;

      case 3:
        new = tune;
        break;

      case 4:

        // According to the ANAN document this is LED #5
        if (can_transmit) {
          new = transmitter->puresignal;
        } else {
          new = 0;
        }

        break;

      case 5:
        // According to the ANAN document this is LED #5
        new = diversity_enabled;
        break;

      case 6:
        // shift LED handled by Rick's code
        break;

      case 7:
        new = vfo[active_receiver->id].ctun;
        break;

      case 8:
        new = vfo[active_receiver->id].rit_enabled;
        break;

      case 9:
        new = vfo[vfo_get_tx_vfo()].xit_enabled;
        break;

      case 10:
        new = (active_receiver->id  == 0);
        break;

      case 11:
        new = locked;
        break;
      }
    }

    if (client->andromeda_type == 5) {
      //
      // G2Mk2 (a.k.a. G2 Ultra) console
      //
      switch (led) {
      case 1:
        new = mox;
        break;

      case 2:
        new = tune;
        break;

      case 3:
        if (can_transmit) {
          new = transmitter->puresignal;
        } else {
          new = 0;
        }

        break;

      case 4:
        new = auto_tune_flag;
        break;

      case 6:
        new = vfo[active_receiver->id].rit_enabled;
        break;

      case 7:
        new = vfo[vfo_get_tx_vfo()].xit_enabled;
        break;

      case 8:
        new = (active_receiver->id  == 0);
        break;

      case 9:
        new = locked;
        break;
      }
    }

    //
    // if LED status changed, send it via ZZZI command
    //
    if (client->last_led[led] != new) {
      snprintf(reply, 256, "ZZZI%02d%d;", led, new);
      send_resp(client->fd, reply);
      client->last_led[led] = new;
    }
  }

  return TRUE;
}

static gboolean andromeda_oneshot_handler(gpointer data) {
  //
  // This is the handler, called once, so it has to return
  // G_SOURCE_REMOVE. It is intended to be exectuted via
  // g_idle_add() at the end of a ZZZP handling when
  // "immediate" LED update is desired.
  //
  (void) andromeda_handler(data);
  return G_SOURCE_REMOVE;
}

static gpointer rigctl_server(gpointer data) {
  int port = GPOINTER_TO_INT(data);
  int on = 1;
  t_print("%s: starting TCP server on port %d\n", __FUNCTION__, port);
  server_socket = socket(AF_INET, SOCK_STREAM, 0);

  if (server_socket < 0) {
    t_perror("rigctl_server: listen socket failed");
    return NULL;
  }

  setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  setsockopt(server_socket, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
  // bind to listening port
  memset(&server_address, 0, sizeof(server_address));
  server_address.sin_family = AF_INET;
  server_address.sin_addr.s_addr = INADDR_ANY;
  server_address.sin_port = htons(port);

  if (bind(server_socket, (struct sockaddr * )&server_address, sizeof(server_address)) < 0) {
    t_perror("rigctl_server: listen socket bind failed");
    close(server_socket);
    return NULL;
  }

  for (int id = 0; id < MAX_TCP_CLIENTS; id++) {
    tcp_client[id].fd = -1;
    tcp_client[id].fifo = 0;
    tcp_client[id].auto_reporting = 0;
  }

  // listen with a max queue of 3
  if (listen(server_socket, 3) < 0) {
    t_perror("rigctl_server: listen failed");
    close(server_socket);
    return NULL;
  }

  // must start the thread here in order NOT to inherit a lock
  cw_buf_in = 0;
  cw_buf_out = 0;

  if (!rigctl_cw_thread_id) { rigctl_cw_thread_id = g_thread_new("RIGCTL cw", rigctl_cw_thread, NULL); }

  while (tcp_running) {
    int spare;
    //
    // find a spare slot
    //
    spare = -1;

    for (int id = 0; id < MAX_TCP_CLIENTS; id++) {
      if (tcp_client[id].fd == -1) {
        spare = id;
        break;
      }
    }

    // if all slots are in use, wait and continue
    if (spare < 0) {
      usleep(100000L);
      continue;
    }

    //
    // A slot is available, try to get connection via accept()
    // (this initializes fd, address, address_length)
    //
    t_print("%s: slot= %d waiting for connection\n", __FUNCTION__, spare);
    tcp_client[spare].fd = accept(server_socket, (struct sockaddr*)&tcp_client[spare].address,
                                  &tcp_client[spare].address_length);

    if (tcp_client[spare].fd < 0) {
      t_perror("rigctl_server: client accept failed");
      tcp_client[spare].fd = -1;
      continue;
    }

    t_print("%s: slot= %d connected with fd=%d\n", __FUNCTION__, spare, tcp_client[spare].fd);
    //
    // Setting TCP_NODELAY may (or may not) improve responsiveness
    // by *disabling* Nagle's algorithm for clustering small packets
    //
#ifdef __APPLE__

    if (setsockopt(tcp_client[spare].fd, IPPROTO_TCP, TCP_NODELAY, (void *)&on, sizeof(on)) < 0) {
#else

    if (setsockopt(tcp_client[spare].fd, SOL_TCP, TCP_NODELAY, (void *)&on, sizeof(on)) < 0) {
#endif
      t_perror("TCP_NODELAY");
    }

    //
    // Initialize client data structure
    //
    tcp_client[spare].fifo            = 0;
    tcp_client[spare].busy            = 0;
    tcp_client[spare].done            = 0;
    tcp_client[spare].running         = 1;
    tcp_client[spare].andromeda_timer = 0;
    tcp_client[spare].auto_reporting  = SET(rigctl_tcp_autoreporting);
    tcp_client[spare].andromeda_type  = 0;
    tcp_client[spare].last_fa         = -1;
    tcp_client[spare].last_fb         = -1;
    tcp_client[spare].last_md         = -1;
    tcp_client[spare].last_v          = 0;

    for (int i = 0; i < MAX_ANDROMEDA_LEDS; i++) {
      tcp_client[spare].last_led[i] = -1;
    }

    //
    // Spawn off thread that "does" the connection
    //
    tcp_client[spare].thread_id       = g_thread_new("rigctl client", rigctl_client, (gpointer)&tcp_client[spare]);
    //
    // Launch auto-reporter task
    //
    tcp_client[spare].auto_timer = g_timeout_add(750, autoreport_handler, &tcp_client[spare]);

    //
    // If ANDROMEDA is enabled for TCP, lauch periodic ANDROMEDA task
    //
    if (rigctl_tcp_andromeda) {
      // Note this will send a ZZZS; command upon first invocation
      tcp_client[spare].andromeda_timer = g_timeout_add(500, andromeda_handler, &tcp_client[spare]);
    }
  }

  close(server_socket);
  return NULL;
}

static gpointer rigctl_client (gpointer data) {
  CLIENT *client = (CLIENT *)data;
  t_print("%s: starting rigctl_client: socket=%d\n", __FUNCTION__, client->fd);
  g_mutex_lock(&mutex_numcat);
  cat_control++;

  if (rigctl_debug) { t_print("RIGCTL: CTLA INC cat_control=%d\n", cat_control); }

  g_mutex_unlock(&mutex_numcat);
  g_idle_add(ext_vfo_update, NULL);
  int i;
  int numbytes;
  char  cmd_input[MAXDATASIZE] ;
  char *command = g_new(char, MAXDATASIZE);
  int command_index = 0;

  while (client->running && (numbytes = recv(client->fd, cmd_input, MAXDATASIZE - 2, 0)) > 0 ) {
    for (i = 0; i < numbytes; i++) {
      //
      // Filter out newlines and other non-printable characters
      // These may occur when doing CAT manually with a terminal program
      //
      if (cmd_input[i] < 32) {
        continue;
      }

      command[command_index] = cmd_input[i];
      command_index++;

      if (cmd_input[i] == ';') {
        command[command_index] = '\0';

        if (rigctl_debug) { t_print("RIGCTL: command=%s\n", command); }

        COMMAND *info = g_new(COMMAND, 1);
        info->client = client;
        info->command = command;
        g_idle_add(parse_cmd, info);
        command = g_new(char, MAXDATASIZE);
        command_index = 0;
      }
    }
  }

  // Release the last "command" buffer (that has not yet been used)
  g_free(command);
  t_print("%s: Leaving rigctl_client thread\n", __FUNCTION__);

  //
  // If rigctl is disabled via the GUI, the connections are closed by shutdown_rigctl_ports()
  // but even the we should decrement cat_control
  //
  if (client->fd != -1) {
    // t_print("%s: setting SO_LINGER to 0 for client_socket: %d\n", __FUNCTION__, client->fd);
    struct linger linger = { 0 };
    linger.l_onoff = 1;
    linger.l_linger = 0;

    if (setsockopt(client->fd, SOL_SOCKET, SO_LINGER, (const char *)&linger, sizeof(linger)) == -1) {
      t_perror("setsockopt(...,SO_LINGER,...) failed for client:");
    }

    if (client->andromeda_timer != 0) {
      g_source_remove(client->andromeda_timer);
      client->andromeda_timer = 0;
    }

    if (client->auto_timer != 0) {
      g_source_remove(client->auto_timer);
      client->auto_timer = 0;
    }

    client->running = 0;
    close(client->fd);
    client->fd = -1;
  }

  // Decrement CAT_CONTROL
  g_mutex_lock(&mutex_numcat);
  cat_control--;
  // if (rigctl_debug) { t_print("RIGCTL: CTLA DEC - cat_control=%d\n", cat_control); }
  g_mutex_unlock(&mutex_numcat);
  g_idle_add(ext_vfo_update, NULL);
  return NULL;
}

gboolean parse_extended_cmd (const char *command, CLIENT *client) {
  gboolean implemented = TRUE;
  char reply[256];
  reply[0] = '\0';

  switch (command[2]) {
  case 'A': //ZZAx
    switch (command[3]) {
    case 'C': //ZZAC

      //CATDEF    ZZAC
      //DESCR     Set/read VFO-A step size
      //SET       ZZACxx;
      //READ      ZZAC;
      //RESP      ZZACxx;
      //NOTE      x 0...16 encodes the step size:
      //NOTE      1 Hz (x=0), 10 Hz (x=1), 25 Hz (x=2), 50 Hz (x=3)
      //CONT      100 Hz (x=4), 250 Hz (x=5), 500 Hz (x=6)
      //CONT      1000 Hz (x=7), 5000 Hz (x=8), 6250 Hz (x=9)
      //CONT      9 kHz (x=10), 10 kHz (x=11), 12.5 kHz (x=12)
      //CONT      100 kHz (x=13), 250 kHz (x=14)
      //CONT      500 kHz (x=15), 1 MHz (x=16).
      //ENDDEF
      if (command[4] == ';') {
        // read the step size
        snprintf(reply, 256, "ZZAC%02d;", vfo_get_stepindex(VFO_A));
        send_resp(client->fd, reply) ;
      } else if (command[6] == ';') {
        // set the step size
        int i = atoi(&command[4]) ;
        vfo_set_step_from_index(VFO_A, i);
        g_idle_add(ext_vfo_update, NULL);
      } else {
      }

      break;

    case 'D': //ZZAD

      //CATDEF    ZZAD
      //DESCR     Move down VFO-A frequency by a selected step
      //SET       ZZACxx;
      //NOTE      x encodes the step size, see ZZAC command.
      //ENDDEF
      if (command[6] == ';') {
        int step_index = atoi(&command[4]);
        long long hz = (long long) vfo_get_step_from_index(step_index);
        vfo_id_move(VFO_A, -hz, FALSE);
      } else {
      }

      break;

    case 'E': //ZZAE

      //CATDEF    ZZAE
      //DESCR     Move down VFO-A frequency by several steps
      //SET       ZZAExx;
      //NOTE      VFO-A frequency moved down by x (0...99) times the current step size
      //ENDDEF
      if (command[6] == ';') {
        int steps = atoi(&command[4]);
        vfo_id_step(VFO_A, -steps);
      }

      break;

    case 'F': //ZZAF

      //CATDEF    ZZAF
      //DESCR     Move up VFO-A frequency by several steps
      //SET       ZZAFxx;
      //NOTE      VFO-A frequency moved up by x (0...99) times the current step size
      //ENDDEF
      if (command[6] == ';') {
        int steps = atoi(&command[4]);
        vfo_id_step(VFO_A, steps);
      }

      break;

    case 'G': //ZZAG

      //CATDEF    ZZAG
      //DESCR     Set/Read RX1 volume (AF slider)
      //SET       ZZAGxxx;
      //READ      ZZAG;
      //RESP      ZZAGxxx;
      //NOTE      x = 0...100, mapped logarithmically to -40 ... 0 dB.
      //ENDDEF
      if (command[4] == ';') {
        // send reply back
        snprintf(reply, 256, "ZZAG%03d;", (int)(100.0 * pow(10.0, 0.05 * receiver[0]->volume)));
        send_resp(client->fd, reply) ;
      } else {
        int gain = atoi(&command[4]);

        if (gain < 2) {
          receiver[0]->volume = -40.0;
        } else {
          receiver[0]->volume = 20.0 * log10(0.01 * (double) gain);
        }

        set_af_gain(0, receiver[0]->volume);
      }

      break;

    case 'I': //ZZAI

      //CATDEF    ZZAI
      //DESCR     Set/Read auto-reporting
      //SET       ZZAIx;
      //READ      ZZAI;
      //RESP      ZZAIx;
      //NOTE      x=0: auto-reporting disabled, x>0: enabled.
      //NOTE      Auto-reporting is affected for the client that sends this command.
      //CONT      For x=1, only frequency changes are sent via FA/FB commands.
      //CONT      For x>1, mode changes are also sent via MD commands.
      //ENDDEF
      if (command[4] == ';') {
        // Query status
        snprintf(reply, 256, "ZZAI%d;", client->auto_reporting);
        send_resp(client->fd, reply) ;
      } else if (command[5] == ';') {
        client->auto_reporting = command[4] - '0';

        if (client->auto_reporting < 0) { client->auto_reporting = 0; }

        if (client->auto_reporting > 3) { client->auto_reporting = 3; }
      } else {
        implemented = FALSE;
      }

      break;

    case 'R': //ZZAR

      //CATDEF    ZZAR
      //DESCR     Set/Read RX1 AGC gain
      //SET       ZZARxxxx;
      //READ      ZZAR;
      //RESP      ZZARxxxx;
      //NOTE      x -20...120, must contain + or - sign.
      //ENDDEF
      if (command[4] == ';') {
        // send reply back
        snprintf(reply, 256, "ZZAR%+04d;", (int)(receiver[0]->agc_gain));
        send_resp(client->fd, reply) ;
      } else {
        int threshold = atoi(&command[4]);
        set_agc_gain(VFO_A, (double)threshold);
      }

      break;

    case 'S': //ZZAS

      //CATDEF    ZZAS
      //DESCR     Set/Read RX2 AGC gain
      //SET       ZZASxxxx;
      //READ      ZZAS;
      //RESP      ZZASxxxx;
      //NOTE      x -20...120, must contain + or - sign.
      //ENDDEF
      if (receivers == 2) {
        if (command[4] == ';') {
          // send reply back
          snprintf(reply, 256, "ZZAS%+04d;", (int)(receiver[1]->agc_gain));
          send_resp(client->fd, reply) ;
        } else {
          int threshold = atoi(&command[4]);
          set_agc_gain(VFO_B, (double)threshold);
        }
      } else {
        implemented = FALSE;
      }

      break;

    case 'U': //ZZAU

      //CATDEF    ZZAU
      //DESCR     Move up VFO-A frequency by selected step
      //SET       ZZAUxx;
      //NOTE      x 0...16 selects the size of the step, see ZZAC command.
      //ENDDEF
      if (command[6] == ';') {
        int step_index = atoi(&command[4]);
        long long hz = (long long) vfo_get_step_from_index(step_index);
        vfo_id_move(VFO_A, hz, FALSE);
      } else {
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'B': //ZZBx
    switch (command[3]) {
    case 'A': //ZZBA

      //CATDEF    ZZBA
      //DESCR     Move VFO-B one band down
      //SET       ZZBA;
      //NOTE      Wraps from lowest to highest band.
      //ENDDEF
      if (command[4] == ';') {
        if (receivers == 2) {
          band_minus(receiver[1]->id);
        } else {
          implemented = FALSE;
        }
      }

      break;

    case 'B': //ZZBB

      //CATDEF    ZZBB
      //DESCR     Move VFO-B one band up
      //SET       ZZBB;
      //NOTE      Wraps from highest to lowest band.
      //ENDDEF
      if (command[4] == ';') {
        if (receivers == 2) {
          band_plus(receiver[1]->id);
        } else {
          implemented = FALSE;
        }
      }

      break;

    case 'D': //ZZBD

      //CATDEF    ZZBD
      //DESCR     Move VFO-A one band down
      //SET       ZZBD;
      //NOTE      Wraps from lowest to highest band.
      //ENDDEF
      if (command[4] == ';') {
        band_minus(receiver[0]->id);
      }

      break;

    case 'E': //ZZBE

      //CATDEF    ZZBE
      //DESCR     Move down VFO-B frequency by multiple steps
      //SET       ZZBExx;
      //NOTE      VFO-B frequency moves down by x (0..99) times the current step size
      //ENDDEF
      if (command[6] == ';') {
        int steps = atoi(&command[4]);
        vfo_id_step(VFO_B, -steps);
      }

      break;

    case 'F': //ZZBF

      //CATDEF    ZZBF
      //DESCR     Move up VFO-B frequency by multiple steps
      //SET       ZZBFxx;
      //NOTE      VFO-B frequency moves up by x (0...99) times the current step size
      //ENDDEF
      if (command[6] == ';') {
        int steps = atoi(&command[4]);
        vfo_id_step(VFO_B, +steps);
      }

      break;

    case 'M': //ZZBM

      //CATDEF    ZZBM
      //DESCR     Move down VFO-B frequency by selected step.
      //SET       ZZBMxx;
      //NOTE      x 0...16 selects the size of the step, see ZZAC command.
      //ENDDEF
      if (command[6] == ';') {
        int step_index = atoi(&command[4]);
        long long hz = (long long) vfo_get_step_from_index(step_index);
        vfo_id_move(VFO_B, -hz, FALSE);
      } else {
      }

      break;

    case 'P': //ZZBP

      //CATDEF    ZZBP
      //DESCR     Move up VFO-B frequency by selected step.
      //SET       ZZBPxx;
      //NOTE      x 0...16 selects the size of the step, see ZZAC command.
      //ENDDEF
      if (command[6] == ';') {
        int step_index = atoi(&command[4]);
        long long hz = (long long) vfo_get_step_from_index(step_index);
        vfo_id_move(VFO_B, hz, FALSE);
      }

      break;

    case 'S':
    case 'T': { //ZZBS and ZZBT
      int v = VFO_A;

      if (command[3] == 'T') { v = VFO_B; }

      //CATDEF    ZZBS
      //DESCR     Set/Read VFO-A band
      //SET       ZZBSxxx;
      //NOTE      x 0...999 encodes the band:
      //NOTE      136 kHz (x=136), 472 kHz (x=472), 160M (x=160)
      //CONT      80M (x=80), 60M (x=60), 40M (x=40), 30M (x=30)
      //CONT      20M (x=20), 17M (x=17), 15M (x=15), 12M (x=12)
      //CONT      10M (x=10), 6M (x=6), Gen (x=888), WWV (x=999).
      //ENDDEF
      //CATDEF    ZZBT
      //DESCR     Set/Read VFO-B band
      //SET       ZZBTxxx;
      //NOTE      x 0...999 encodes the band, see ZZBS command.
      //ENDDEF
      if (command[4] == ';') {
        int b;

        switch (vfo[v].band) {
        case band136:
          b = 136;
          break;

        case band472:
          b = 472;
          break;

        case band160:
          b = 160;
          break;

        case band80:
          b = 80;
          break;

        case band60:
          b = 60;
          break;

        case band40:
          b = 40;
          break;

        case band30:
          b = 30;
          break;

        case band20:
          b = 20;
          break;

        case band17:
          b = 17;
          break;

        case band15:
          b = 15;
          break;

        case band12:
          b = 12;
          break;

        case band10:
          b = 10;
          break;

        case band6:
          b = 6;
          break;

        case bandGen:
          b = 888;
          break;

        case bandWWV:
          b = 999;
          break;

        default:
          b = 20;
          break;
        }

        snprintf(reply, 256, "ZZB%c%03d;", 'S' + v, b);
        send_resp(client->fd, reply) ;
      } else if (command[7] == ';') {
        int band = band20;
        int b = atoi(&command[4]);

        switch (b) {
        case 136:
          band = band136;
          break;

        case 472:
          band = band472;
          break;

        case 160:
          band = band160;
          break;

        case 80:
          band = band80;
          break;

        case 60:
          band = band60;
          break;

        case 40:
          band = band40;
          break;

        case 30:
          band = band30;
          break;

        case 20:
          band = band20;
          break;

        case 17:
          band = band17;
          break;

        case 15:
          band = band15;
          break;

        case 12:
          band = band12;
          break;

        case 10:
          band = band10;
          break;

        case 6:
          band = band6;
          break;

        case 888:
          band = bandGen;
          break;

        case 999:
          band = bandWWV;
          break;
        }

        vfo_band_changed(v, band);
      }
    }
    break;

    case 'U': //ZZBU

      //CATDEF    ZZBU
      //DESCR     Move VFO-A one band up
      //SET       ZZBU;
      //NOTE      Wraps from highest to lowest band.
      //ENDDEF
      if (command[4] == ';') {
        band_plus(receiver[0]->id);
      }

      break;

    case 'Y': //ZZBY
      // closes console (ignored)
      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'C': //ZZCx
    switch (command[3]) {
    case 'N': //ZZCN

      //CATDEF    ZZCN
      //DESCR     Set/Read VFO-A CTUN status
      //SET       ZZCNx;
      //READ      ZZCN;
      //RESP      ZZCNx;
      //NOTE      x=0: CTUN disabled, x=1: enabled
      //ENDDEF
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZCN%d;", vfo[VFO_A].ctun);
        send_resp(client->fd, reply) ;
      } else if (command[5] == ';') {
        int state = atoi(&command[4]);
        vfo_ctun_update(VFO_A, state);
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    case 'O': //ZZCO

      //CATDEF    ZZCO
      //DESCR     Set/Read VFO-B CTUN status
      //SET       ZZCOx;
      //READ      ZZCO;
      //RESP      ZZCOx;
      //NOTE      x=0: CTUN disabled, x=1: enabled
      //ENDDEF
      if (command[4] == ';') {
        // return the CTUN status
        snprintf(reply, 256, "ZZCO%d;", vfo[VFO_B].ctun);
        send_resp(client->fd, reply) ;
      } else if (command[5] == ';') {
        int state = atoi(&command[4]);
        vfo_ctun_update(VFO_B, state);
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    case 'P': //ZZCP

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      // set/read compander
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZCP%d;", 0);
        send_resp(client->fd, reply) ;
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'D': //ZZDx
    switch (command[3]) {
    case 'B': //ZZDB

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      // set/read RX Reference
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZDB%d;", 0); // currently always 0
        send_resp(client->fd, reply) ;
      }

      break;

    case 'C': //ZZDC

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      // set/get diversity gain
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZDC%04d;", (int)div_gain);
        send_resp(client->fd, reply) ;
      }

      break;

    case 'D': //ZZDD

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      // set/get diversity phase
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZDD%04d;", (int)div_phase);
        send_resp(client->fd, reply) ;
      }

      break;

    case 'M': //ZZDM

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      // set/read Display Mode
      if (command[4] == ';') {
        int v = 0;

        if (receiver[0]->display_waterfall) {
          v = 8;
        } else {
          v = 2;
        }

        snprintf(reply, 256, "ZZDM%d;", v);
        send_resp(client->fd, reply) ;
      }

      break;

    case 'N': //ZZDN

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      // set/read waterfall low
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZDN%+4d;", receiver[0]->waterfall_low);
        send_resp(client->fd, reply) ;
      }

      break;

    case 'O': //ZZDO

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      // set/read waterfall high
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZDO%+4d;", receiver[0]->waterfall_high);
        send_resp(client->fd, reply) ;
      }

      break;

    case 'P': //ZZDP

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      // set/read panadapter high
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZDP%+4d;", receiver[0]->panadapter_high);
        send_resp(client->fd, reply) ;
      }

      break;

    case 'Q': //ZZDQ

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      // set/read panadapter low
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZDQ%+4d;", receiver[0]->panadapter_low);
        send_resp(client->fd, reply) ;
      }

      break;

    case 'R': //ZZDR

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      // set/read panadapter step
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZDR%2d;", receiver[0]->panadapter_step);
        send_resp(client->fd, reply) ;
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'E': //ZZEx
    switch (command[3]) {
    case 'R': //ZZER

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      // set/read rx equalizer
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZER%d;", receiver[0]->eq_enable);
        send_resp(client->fd, reply) ;
      } else if (command[5] == ';') {
        receiver[0]->eq_enable = SET(atoi(&command[4]));
      }

      break;

    case 'T': //ZZET

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      // set/read tx equalizer
      if (can_transmit) {
        if (command[4] == ';') {
          snprintf(reply, 256, "ZZET%d;", transmitter->eq_enable);
          send_resp(client->fd, reply) ;
        } else if (command[5] == ';') {
          transmitter->eq_enable = SET(atoi(&command[4]));
        }
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'F': //ZZFx
    switch (command[3]) {
    case 'A': //ZZFA

      //CATDEF    ZZFA
      //DESCR     Set/Read VFO-A frequency
      //SET       ZZFAxxxxxxxxxxx;
      //READ      ZZFA;
      //RESP      ZZFAxxxxxxxxxxx;
      //NOTE      x in Hz, left-padded with zeroes
      //ENDDEF
      if (command[4] == ';') {
        if (vfo[VFO_A].ctun) {
          snprintf(reply, 256, "ZZFA%011lld;", vfo[VFO_A].ctun_frequency);
        } else {
          snprintf(reply, 256, "ZZFA%011lld;", vfo[VFO_A].frequency);
        }

        send_resp(client->fd, reply) ;
      } else if (command[15] == ';') {
        long long f = atoll(&command[4]);
        vfo_set_frequency(VFO_A, f);
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    case 'B': //ZZFB

      //CATDEF    ZZFB
      //DESCR     Set/Read VFO-B frequency
      //SET       ZZFBxxxxxxxxxxx;
      //READ      ZZFB;
      //RESP      ZZFBxxxxxxxxxxx;
      //NOTE      x in Hz, left-padded with zeroes
      //ENDDEF
      if (command[4] == ';') {
        if (vfo[VFO_B].ctun) {
          snprintf(reply, 256, "ZZFB%011lld;", vfo[VFO_B].ctun_frequency);
        } else {
          snprintf(reply, 256, "ZZFB%011lld;", vfo[VFO_B].frequency);
        }

        send_resp(client->fd, reply) ;
      } else if (command[15] == ';') {
        long long f = atoll(&command[4]);
        vfo_set_frequency(VFO_B, f);
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    case 'D': //ZZFD

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZFD%d;", vfo[VFO_A].deviation == 2500 ? 0 : 1);
        send_resp(client->fd, reply) ;
      } else if (command[5] == ';') {
        int d = atoi(&command[4]);
        vfo[VFO_A].deviation = d ? 5000 : 2500;
        rx_set_filter(receiver[0]);

        if (can_transmit) {
          tx_set_filter(transmitter);
        }

        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    case 'H': //ZZFH

      //CATDEF    ZZFH
      //DESCR     Set/Read RX1 filter high water
      //SET       ZZFHxxxxx;
      //READ      ZZFH;
      //RESP      ZZFHxxxxxx;
      //NOTE      x must be in the range -9999 ... 9999 and start with a minus sign if negative.
      //CONT      If setting, this switches to the Var1 filter first.
      //CONT      The convention is such that LSB, the filter high cut is negative and affects the low audio frequencies.
      //ENDDEF
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZFH%05d;", receiver[0]->filter_high);
        send_resp(client->fd, reply) ;
      } else if (command[9] == ';') {
        int fh = atoi(&command[4]);
        fh = fmin(9999, fh);
        fh = fmax(-9999, fh);

        // make sure filter is filterVar1
        if (vfo[VFO_A].filter != filterVar1) {
          vfo_id_filter_changed(VFO_A, filterVar1);
        }

        FILTER *mode_filters = filters[vfo[VFO_A].mode];
        FILTER *filter = &mode_filters[filterVar1];
        filter->high = fh;
        vfo_id_filter_changed(VFO_A, filterVar1);
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    case 'I': //ZZFI

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZFI%02d;", vfo[VFO_A].filter);
        send_resp(client->fd, reply) ;
      } else if (command[6] == ';') {
        int filter = atoi(&command[4]);
        vfo_id_filter_changed(VFO_A, filter);
      }

      break;

    case 'J': //ZZFJ

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZFJ%02d;", vfo[VFO_B].filter);
        send_resp(client->fd, reply) ;
      } else if (command[6] == ';') {
        int filter = atoi(&command[4]);
        vfo_id_filter_changed(VFO_B, filter);
      }

      break;

    case 'L': //ZZFL

      //CATDEF    ZZFL
      //DESCR     Set/Read RX1 filter low water
      //SET       ZZFLxxxxx;
      //READ      ZZFL;
      //RESP      ZZFLxxxxxx;
      //NOTE      x must be in the range -9999 ... 9999 and start with a minus sign if negative.
      //CONT      If setting, this switches to the Var1 filter first.
      //CONT      The convention is such that LSB, the filter low cut is negative and affects the high audio frequencies.
      //ENDDEF
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZFL%05d;", receiver[0]->filter_low);
        send_resp(client->fd, reply) ;
      } else if (command[9] == ';') {
        int fl = atoi(&command[4]);
        fl = fmin(9999, fl);
        fl = fmax(-9999, fl);

        // make sure filter is filterVar1
        if (vfo[VFO_A].filter != filterVar1) {
          vfo_id_filter_changed(VFO_A, filterVar1);
        }

        FILTER *mode_filters = filters[vfo[VFO_A].mode];
        FILTER *filter = &mode_filters[filterVar1];
        filter->low = fl;
        vfo_id_filter_changed(VFO_A, filterVar1);
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'G': //ZZGx
    switch (command[3]) {
    case 'T': //ZZGT

      //CATDEF    ZZGT
      //DESCR     Set/Read RX1 AGC
      //SET       ZZGTx;
      //READ      ZZGT;
      //RESP      ZZGTx;
      //NOTE      x=0: AGC OFF, x=1: LONG, x=2: SLOW, x=3: MEDIUM, x=4: FAST
      //ENDDEF
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZGT%d;", receiver[0]->agc);
        send_resp(client->fd, reply) ;
      } else if (command[5] == ';') {
        int agc = atoi(&command[4]);
        // update RX1 AGC
        receiver[0]->agc = agc;
        rx_set_agc(receiver[0]);
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    case 'U': //ZZGU
      //CATDEF    ZZGU
      //DESCR     Set/Read RX2 AGC
      //SET       ZZGUx;
      //READ      ZZGU;
      //RESP      ZZGUx;
      //NOTE      x=0: AGC OFF, x=1: LONG, x=2: SLOW, x=3: MEDIUM, x=4: FAST
      //ENDDEF
      RXCHECK(1,
      if (command[4] == ';') {
      snprintf(reply, 256, "ZZGU%d;", receiver[1]->agc);
        send_resp(client->fd, reply) ;
      } else if (command[5] == ';') {
      int agc = atoi(&command[4]);
        // update RX2 AGC
        RXCHECK(1,
                receiver[1]->agc = agc;
                rx_set_agc(receiver[1]);
                g_idle_add(ext_vfo_update, NULL);
               )
      }
             )
      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'H': //ZZHx
  case 'I': //ZZIx
  case 'K': //ZZKx
    implemented = FALSE;
    break;

  case 'L': //ZZLx
    switch (command[3]) {
    case 'A': //ZZLA

      //CATDEF    ZZLA
      //DESCR     Set/Read RX1 volume (AF slider)
      //SET       ZZLAxxx;
      //READ      ZZLA;
      //RESP      ZZLAxxx;
      //NOTE      x = 0...100, mapped logarithmically to -40 ... 0 dB.
      //ENDDEF
      if (command[4] == ';') {
        // send reply back
        snprintf(reply, 256, "ZZLA%03d;", (int)(receiver[0]->volume * 100.0));
        send_resp(client->fd, reply) ;
      } else {
        int gain = atoi(&command[4]);

        // gain is 0..100
        if (gain < 2) {
          receiver[0]->volume = -40.0;
        } else {
          receiver[0]->volume = 20.0 * log10(0.01 * (double) gain);
        }

        set_af_gain(0, receiver[0]->volume);
      }

      break;

    case 'C': //ZZLC
      //CATDEF    ZZLC
      //DESCR     Set/Read RX2 volume (AF slider)
      //SET       ZZLCxxx;
      //READ      ZZLC;
      //RESP      ZZLCxxx;
      //NOTE      x = 0...100, mapped logarithmically to -40 ... 0 dB.
      //ENDDEF
      RXCHECK(1,
      if (command[4] == ';') {
      // send reply back
      snprintf(reply, 256, "ZZLC%03d;", (int)(255.0 * pow(10.0, 0.05 * receiver[1]->volume)));
        send_resp(client->fd, reply) ;
      } else {
        int gain = atoi(&command[4]);

        // gain is 0..100
        if (gain < 2) {
          receiver[1]->volume = -40.0;
        } else {
          receiver[1]->volume = 20.0 * log10(0.01 * (double) gain);
        }

        set_af_gain(1, receiver[1]->volume);
      }
             )
      break;

    case 'I': //ZZLI

      //CATDEF    ZZLI
      //DESCR     Set/Read PURESIGNAL status
      //SET       ZZLIx;
      //READ      ZZLI;
      //RESP      ZZLIx;
      //NOTE      x=0: PURESIGNAL disabled, x=1: enabled.
      //ENDDEF
      if (can_transmit) {
        if (command[4] == ';') {
          // send reply back
          snprintf(reply, 256, "ZZLI%d;", transmitter->puresignal);
          send_resp(client->fd, reply) ;
        } else {
          int ps = atoi(&command[4]);
          tx_ps_onoff(transmitter, ps);
        }

        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'M': //ZZMx
    switch (command[3]) {
    case 'A':  //ZZMA

      //CATDEF    ZZMA
      //DESCR     Mute/Unmute RX1
      //SET       ZZMAx;
      //READ      ZZMA;
      //RESP      ZZMAx;
      //NOTE      x=0: RX1 not muted, x=1: muted.
      //CONT      This only affects the audio sent to the radio via the HPSDR protocol.
      //ENDDEF
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZMA%d;", receiver[0]->mute_radio);
        send_resp(client->fd, reply) ;
      } else {
        int mute = atoi(&command[4]);
        receiver[0]->mute_radio = mute;
      }

      break;

    case 'B': //ZZMB
      //CATDEF    ZZMB
      //DESCR     Mute/Unmute RX2
      //SET       ZZMBx;
      //READ      ZZMB;
      //RESP      ZZMBx;
      //NOTE      x=0: RX2 not muted, x=1: muted.
      //CONT      This only affects the audio sent to the radio via the HPSDR protocol.
      //ENDDEF
      RXCHECK(1,
      if (command[4] == ';') {
      snprintf(reply, 256, "ZZMA%d;", receiver[1]->mute_radio);
        send_resp(client->fd, reply) ;
      } else {
        int mute = atoi(&command[4]);
        receiver[1]->mute_radio = mute;
      }
             )
      break;

    case 'D': //ZZMD

      //CATDEF    ZZMD
      //DESCR     Set/Read VFO-A modes
      //SET       ZZMDxx;
      //READ      ZZMD;
      //RESP      ZZMDxx;
      //NOTE      Modes: LSB (x=0), USB (x=1), DSB (x=3), CWL (x=4)
      //CONT      CWU (x=5), FMN (x=6), AM (x=7), DIGU (x=7)
      //CONT      SPEC (x=8), DIGL (x=9), SAM (x=10), DRM (x=11)
      //ENDDEF
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZMD%02d;", vfo[VFO_A].mode);
        send_resp(client->fd, reply);
      } else if (command[6] == ';') {
        vfo_id_mode_changed(VFO_A, atoi(&command[4]));
      }

      break;

    case 'E': //ZZME

      //CATDEF    ZZME
      //DESCR     Set/Read VFO-B modes
      //SET       ZZMEx;
      //READ      ZZME;
      //RESP      ZZMEx;
      //NOTE      x encodes the mode (see ZZMD command)
      //ENDDEF
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZMD%02d;", vfo[VFO_B].mode);
        send_resp(client->fd, reply);
      } else if (command[6] == ';') {
        vfo_id_mode_changed(VFO_A, atoi(&command[4]));
      }

      break;

    case 'G': //ZZMG

      //CATDEF    ZZMG
      //DESCR     Set/Read Mic gain (Mic gain slider)
      //SET       ZZMGxxx;
      //READ      ZZMG;
      //RESP      ZZMGxxx;
      //NOTE      x 0-70 mapped to -12 ... +50 dB
      //ENDDEF
      if (can_transmit) {
        if (command[4] == ';') {
          snprintf(reply, 256, "ZZMG%03d;", (int)((transmitter->mic_gain + 12.0) * 1.129));
          send_resp(client->fd, reply);
        } else if (command[7] == ';') {
          int val = atoi(&command[4]);
#if defined (__LDESK__) && defined (__USELESS__)
          set_mic_gain(((double) val * 0.8857) - 12.0);
#else
          transmitter->mic_gain = ((double) val * 0.8857) - 12.0;
          tx_set_mic_gain(transmitter);
#endif
        }
      } else {
        implemented = FALSE;
      }

      break;

    case 'L': //ZZML

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZML LSB00: USB01: DSB02: CWL03: CWU04: FMN05:  AM06:DIGU07:SPEC08:DIGL09: SAM10: DRM11;");
        send_resp(client->fd, reply);
      }

      break;

    case 'N': //ZZMN

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[6] == ';') {
        int mode = atoi(&command[4]) - 1;
        const FILTER *f = filters[mode];
        snprintf(reply, 256, "ZZMN");
        char temp[32];

        for (int i = 0; i < FILTERS; i++) {
          snprintf(temp, 32, "%5s%5d%5d", f[i].title, f[i].high, f[i].low);
          g_strlcat(reply, temp, 256);
        }

        g_strlcat(reply, ";", 256);
        send_resp(client->fd, reply);
      }

      break;

    case 'O': //ZZMO

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      // set/read MON status
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZMO%d;", 0);
        send_resp(client->fd, reply);
      }

      break;

    case 'R': //ZZMR

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZMR%d;", active_receiver->smetermode + 1);
        send_resp(client->fd, reply);
      } else if (command[5] == ';') {
        int val = atoi(&command[4]) - 1;

        switch (val) {
        case 0:
          active_receiver->smetermode = SMETER_PEAK;
          break;

        case 1:
          active_receiver->smetermode = SMETER_AVERAGE;
          break;
        }
      }

      break;

    case 'T': //ZZMT

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZMT%02d;", 1); // forward power
        send_resp(client->fd, reply);
      } else {
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'N': //ZZNx
    switch (command[3]) {
    case 'A': //ZZNA

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZNA%d;", (receiver[0]->nb == 1));
        send_resp(client->fd, reply);
      } else if (command[5] == ';') {
        if (atoi(&command[4])) { receiver[0]->nb = 1; }

        update_noise();
      }

      break;

    case 'B': //ZZNB

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZNB%d;", (receiver[0]->nb == 2));
        send_resp(client->fd, reply);
      } else if (command[5] == ';') {
        if (atoi(&command[4])) { receiver[0]->nb = 2; }

        update_noise();
      }

      break;

    case 'C': //ZZNC

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (receivers == 2) {
        if (command[4] == ';') {
          snprintf(reply, 256, "ZZNC%d;", (receiver[1]->nb == 1));
          send_resp(client->fd, reply);
        } else if (command[5] == ';') {
          if (atoi(&command[4])) { receiver[1]->nb = 1; }

          update_noise();
        }
      } else {
        implemented = FALSE;
      }

      break;

    case 'D': //ZZND

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (receivers == 2) {
        if (command[4] == ';') {
          snprintf(reply, 256, "ZZND%d;", (receiver[1]->nb == 2));
          send_resp(client->fd, reply);
        } else if (command[5] == ';') {
          if (atoi(&command[4])) { receiver[1]->nb = 2; }

          update_noise();
        }
      } else {
        implemented = FALSE;
      }

      break;

    case 'N': //ZZNN

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZNN%d;", receiver[0]->snb);
        send_resp(client->fd, reply);
      } else if (command[5] == ';') {
        receiver[0]->snb = atoi(&command[4]);
        update_noise();
      }

      break;

    case 'O': //ZZNO

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (receivers == 2) {
        if (command[4] == ';') {
          snprintf(reply, 256, "ZZNO%d;", receiver[1]->snb);
          send_resp(client->fd, reply);
        } else if (command[5] == ';') {
          receiver[1]->snb = atoi(&command[4]);
          update_noise();
        }
      } else {
        implemented = FALSE;
      }

      break;

    case 'R': //ZZNR

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (receivers == 2) {
        if (command[4] == ';') {
          snprintf(reply, 256, "ZZNR%d;", (receiver[0]->nr == 1));
          send_resp(client->fd, reply);
        } else if (command[5] == ';') {
          if (atoi(&command[4])) { receiver[0]->nr = 1; }

          update_noise();
        }
      }

      break;

    case 'S': //ZZNS

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZNS%d;", (receiver[0]->nr == 2));
        send_resp(client->fd, reply);
      } else if (command[5] == ';') {
        if (atoi(&command[4])) { receiver[0]->nr = 2; }

        update_noise();
      }

      break;

    case 'T': //ZZNT

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZNT%d;", receiver[0]->anf);
        send_resp(client->fd, reply);
      } else if (command[5] == ';') {
        if (atoi(&command[4])) { receiver[0]->anf = 1; }

        update_noise();
      }

      break;

    case 'U': //ZZNU

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (receivers == 2) {
        if (command[4] == ';') {
          snprintf(reply, 256, "ZZNU%d;", receiver[1]->anf);
          send_resp(client->fd, reply);
        } else if (command[5] == ';') {
          if (atoi(&command[4])) { receiver[1]->anf = 1; }

          update_noise();
        }
      } else {
        implemented = FALSE;
      }

      break;

    case 'V': //ZZNV

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (receivers == 2) {
        if (command[4] == ';') {
          snprintf(reply, 256, "ZZNV%d;", (receiver[1]->nr == 1));
          send_resp(client->fd, reply);
        } else if (command[5] == ';') {
          if (atoi(&command[4])) { receiver[1]->nr = 1; }

          update_noise();
        }
      } else {
        implemented = FALSE;
      }

      break;

    case 'W': //ZZNW

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (receivers == 2) {
        if (command[4] == ';') {
          snprintf(reply, 256, "ZZNW%d;", (receiver[1]->nr == 2));
          send_resp(client->fd, reply);
        } else if (command[5] == ';') {
          if (atoi(&command[4])) { receiver[1]->nr = 2; }

          update_noise();
        }
      } else {
        implemented = FALSE;
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'O': //ZZOx
    implemented = FALSE;
    break;

  case 'P': //ZZPx
    switch (command[3]) {
    case 'A': //ZZPA

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        int a = adc[receiver[0]->adc].attenuation;

        if (a == 0) {
          a = 1;
        } else if (a <= -30) {
          a = 4;
        } else if (a <= -20) {
          a = 0;
        } else if (a <= -10) {
          a = 2;
        } else {
          a = 3;
        }

        snprintf(reply, 256, "ZZPA%d;", a);
        send_resp(client->fd, reply);
      } else if (command[5] == ';' && have_rx_att) {
        int a = atoi(&command[4]);

        switch (a) {
        case 0:
          adc[receiver[0]->adc].attenuation = -20;
          break;

        case 1:
          adc[receiver[0]->adc].attenuation = 0;
          break;

        case 2:
          adc[receiver[0]->adc].attenuation = -10;
          break;

        case 3:
          adc[receiver[0]->adc].attenuation = -20;
          break;

        case 4:
          adc[receiver[0]->adc].attenuation = -30;
          break;

        default:
          adc[receiver[0]->adc].attenuation = 0;
          break;
        }
      }

      break;

    case 'Y': // ZZPY

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZPY%d;", receiver[0]->zoom);
        send_resp(client->fd, reply);
      } else if (command[7] == ';') {
        int zoom = atoi(&command[4]);
        set_zoom(0, zoom);
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'Q': //ZZQx
    implemented = FALSE;
    break;

  case 'R': //ZZRx
    switch (command[3]) {
    case 'C': //ZZRC

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        schedule_action(RIT_CLEAR, PRESSED, 0);
      }

      break;

    case 'D': //ZZRD

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        vfo_rit_incr(VFO_A, -vfo[VFO_A].rit_step);
      } else if (command[9] == ';') {
        // set RIT frequency
        vfo_rit_value(VFO_A, atoi(&command[4]));
      }

      break;

    case 'F': //ZZRF

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZRF%+5lld;", vfo[VFO_A].rit);
        send_resp(client->fd, reply);
      } else if (command[9] == ';') {
        vfo_rit_value(VFO_A, atoi(&command[4]));
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    case 'M': //ZZRM

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[5] == ';') {
        snprintf(reply, 256, "ZZRM%d%20d;", active_receiver->smetermode, (int)receiver[0]->meter);
        send_resp(client->fd, reply);
      }

      break;

    case 'S': //ZZRS

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZRS%d;", receivers == 2);
        send_resp(client->fd, reply);
      } else if (command[5] == ';') {
        int state = atoi(&command[4]);

        if (state) {
          radio_change_receivers(2);
        } else {
          radio_change_receivers(1);
        }
      }

      break;

    case 'T': //ZZRT

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZRT%d;", vfo[VFO_A].rit_enabled);
        send_resp(client->fd, reply);
      } else if (command[5] == ';') {
        vfo_rit_onoff(VFO_A, SET(atoi(&command[4])));
      }

      break;

    case 'U': //ZZRU

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        vfo_rit_incr(VFO_A, vfo[VFO_A].rit_step);
      } else if (command[9] == ';') {
        vfo_rit_value(VFO_A,  atoi(&command[4]));
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'S': //ZZSx
    switch (command[3]) {
    case 'A': //ZZSA

      //CATDEF    ZZSA
      //DESCR     Move down VFO-A frequency one step
      //SET       ZZSA;
      //NOTE      VFO-A frequency moved down by the current step size
      //ENDDEF
      if (command[4] == ';') {
        vfo_id_step(VFO_A, -1);
      }

      break;

    case 'B': //ZZSB

      //CATDEF    ZZSB
      //DESCR     Move up VFO-A frequency one step
      //SET       ZZSB;
      //NOTE      VFO-A frequency moved up by the current step size
      //ENDDEF
      if (command[4] == ';') {
        vfo_id_step(VFO_A, 1);
      }

      break;

    case 'G': //ZZSG

      //CATDEF    ZZSG
      //DESCR     Move down VFO-B frequency one step
      //SET       ZZSG;
      //NOTE      VFO-B frequency moved down by the current step size
      //ENDDEF
      if (command[4] == ';') {
        vfo_id_step(VFO_B, -1);
      }

      break;

    case 'H': //ZZSH

      //CATDEF    ZZSH
      //DESCR     Move up VFO-B frequency one step
      //SET       ZZSG;
      //NOTE      VFO-B frequency moved up by the current step size
      //ENDDEF
      if (command[4] == ';') {
        vfo_id_step(VFO_B, 1);
      }

      break;

    case 'M': //ZZSM

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[5] == ';') {
        int v = atoi(&command[4]);

        if (v >= 0 && v < receivers) {
          double m = receiver[v]->meter;
          m = fmax(-140.0, m);
          m = fmin(-10.0, m);
          snprintf(reply, 256, "ZZSM%d%03d;", v, (int)((m + 140.0) * 2));
          send_resp(client->fd, reply);
        } else {
          implemented = FALSE;
        }
      }

      break;

    case 'P': //ZZSP

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZSP%d;", split);
        send_resp(client->fd, reply) ;
      } else if (command[5] == ';') {
        int val = atoi(&command[4]);
        radio_set_split(val);
      }

      break;

    case 'W': //ZZSW

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZSW%d;", split);
        send_resp(client->fd, reply) ;
      } else if (command[5] == ';') {
        int val = atoi(&command[4]);
        radio_set_split(val);
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'T': //ZZTx
    switch (command[3]) {
    case 'U': //ZZTU

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZTU%d;", tune);
        send_resp(client->fd, reply) ;
      } else if (command[5] == ';') {
        radio_tune_update(atoi(&command[4]));
      }

      break;

    case 'X': //ZZTX

      //CATDEF    ZZTX
      //DESCR     Get/Set MOX status
      //SET       ZZTXx;
      //READ      ZZTX;
      //RESP      ZZTXx;
      //NOTE      x=1: MOX on, x=0: off.
      //ENDDEF
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZTX%d;", mox);
        send_resp(client->fd, reply) ;
      } else if (command[5] == ';') {
        radio_mox_update(atoi(&command[4]));
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'U': //ZZUx
    switch (command[3]) {
    case 'U': //ZZUT

      //CATDEF    ZZUT
      //DESCR     Get/Set TwoTone status
      //SET       ZZUTx;
      //READ      ZZUT;
      //RESP      ZZTXx;
      //NOTE      x=1: TwoTone on, x=0: TwoTone off.
      //ENDDEF
      if (can_transmit) {
        if (command[4] == ';') {
          snprintf(reply, 256, "ZZUT%d;", transmitter->twotone);
          send_resp(client->fd, reply) ;
        } else if (command[5] == ';') {
          tx_set_twotone(transmitter, atoi(&command[4]));
        }
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'V': //ZZVx
    switch (command[3]) {
    case 'L': //ZZVL
      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      locked = command[4] == '1';
      g_idle_add(ext_vfo_update, NULL);
      break;

    case 'S': { //ZZVS
      //CATDEF    ZZVS
      //DESCR     Swap VFO A and B
      //SET       ZZVS;
      //NOTE      The contents (frequencies, CTUN mode, filters, etc.) of VFO A and B are exchanged.
      //ENDDEF
      int i = atoi(&command[4]);

      if (i == 0) {
        vfo_a_to_b();
      } else if (i == 1) {
        vfo_b_to_a();
      } else {
        vfo_a_swap_b();
      }
    }
    break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'W': //ZZWx
    implemented = FALSE;
    break;

  case 'X': //ZZXx
    switch (command[3]) {
    case 'C': //ZZXC
      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      schedule_action(XIT_CLEAR, PRESSED, 0);
      break;

    case 'F': //ZZXF

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZXT%+05lld;", vfo[vfo_get_tx_vfo()].xit);
        send_resp(client->fd, reply) ;
      } else if (command[9] == ';') {
        vfo_xit_value(atoi(&command[4]));
      }

      break;

    case 'N': //ZZXN

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        int status = ((receiver[0]->agc) & 0x03);
        int a = adc[receiver[0]->adc].attenuation;

        if (a == 0) {
          a = 1;
        } else if (a <= -30) {
          a = 4;
        } else if (a <= -20) {
          a = 0;
        } else if (a <= -10) {
          a = 2;
        } else {
          a = 3;
        }

        status = status | ((a & 0x03) << 3);

        if (receiver[0]->squelch_enable) { status |=  0x0040; }

        if (receiver[0]->nb == 1) { status |=  0x0080; }

        if (receiver[0]->nb == 2) { status |=  0x0100; }

        if (receiver[0]->nr == 1) { status |=  0x0200; }

        if (receiver[0]->nr == 2) { status |=  0x0400; }

        if (receiver[0]->snb) { status |=  0x0800; }

        if (receiver[0]->anf) { status |=  0x1000; }

        snprintf(reply, 256, "ZZXN%04d;", status);
        send_resp(client->fd, reply);
      }

      break;

    case 'O': //ZZXO

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (receivers == 2) {
        if (command[4] == ';') {
          int status = ((receiver[1]->agc) & 0x03);
          int a = adc[receiver[1]->adc].attenuation;

          if (a == 0) {
            a = 1;
          } else if (a <= -30) {
            a = 4;
          } else if (a <= -20) {
            a = 0;
          } else if (a <= -10) {
            a = 2;
          } else {
            a = 3;
          }

          status = status | ((a & 0x03) << 3);

          if (receiver[1]->squelch_enable) { status |=  0x0040; }

          if (receiver[1]->nb == 1) { status |=  0x0080; }

          if (receiver[1]->nb == 2) { status |=  0x0100; }

          if (receiver[1]->nr == 1) { status |=  0x0200; }

          if (receiver[1]->nr == 2) { status |=  0x0400; }

          if (receiver[1]->snb) { status |=  0x0800; }

          if (receiver[1]->anf) { status |=  0x1000; }

          snprintf(reply, 256, "ZZXO%04d;", status);
          send_resp(client->fd, reply);
        }
      } else {
        implemented = FALSE;
      }

      break;

    case 'S': //ZZXS

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZXS%d;", vfo[vfo_get_tx_vfo()].xit_enabled);
        send_resp(client->fd, reply);
      } else if (command[5] == ';') {
        vfo[vfo_get_tx_vfo()].xit_enabled = atoi(&command[4]);
        schedule_high_priority();
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    case 'V': //ZZXV

      //CATDEF    ZZXV
      //DESCR     Get extended status information
      //READ      ZZVS;
      //RESP      ZZVSxxxx;
      //NOTE      Status is reported bit-wise in the status word x=0-1023.
      //NOTE      Bit 0: RIT; Bit 1: Lock, Bit2: Lock, Bit3: Split,
      //NOTE      Bit 4: VFO-A CTUN, Bit 5: VFO-B CTUN, Bit 6: MOX,
      //NOTE      Bit 7: TUNE, Bit 8: XIT, Bit 9: always cleared.
      //ENDDEF
      if (command[4] == ';') {
        int status = 0;

        if (vfo[VFO_A].rit_enabled) {
          // cppcheck-suppress badBitmaskCheck
          status = status | 0x01;
        }

        if (locked) {
          status = status | 0x02;
          status = status | 0x04;
        }

        if (split) {
          status = status | 0x08;
        }

        if (vfo[VFO_A].ctun) {
          status = status | 0x10;
        }

        if (vfo[VFO_B].ctun) {
          status = status | 0x20;
        }

        if (mox) {
          status = status | 0x40;
        }

        if (tune) {
          status = status | 0x80;
        }

        if (vfo[vfo_get_tx_vfo()].xit_enabled) {
          status = status | 0x100;
        }

        snprintf(reply, 256, "ZZXV%03d;", status);
        send_resp(client->fd, reply);
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'Y': //ZZYx
    switch (command[3]) {
    case 'R': //ZZYR

      //CATDEF    ZZYR
      //DESCR     Get/Set active receiver
      //SET       ZZYRx;
      //READ      ZZYR;
      //RESP      ZZYRx;
      //NOTE      The active receiver is either RX1 (x=0) or RX2 (x=1).
      //ENDDEF
      if (command[4] == ';') {
        snprintf(reply, 256, "ZZYR%01d;", active_receiver->id);
        send_resp(client->fd, reply);
      } else if (command[5] == ';') {
        int v = atoi(&command[4]);

        if (v >= 0 && v < receivers) {
          schedule_action(v == 0 ? RX1 : RX2, PRESSED, 0);
        } else {
          implemented = FALSE;
        }

        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'Z': //ZZZx
    switch (command[3]) {
    case 'D': //ZZZD

      //CATDEF    ZZZD
      //DESCR     Move down frequency of active receiver
      //SET       ZZZDxx;
      //NOTE      ANDROMEDA extension. x = number of VFO steps.
      //NOTE      For x>10, the number of VFO steps is multiplied with
      //CONT      a speed-up factor that increases up to 4 at x=30
      //CONT      (corresponds to 3 turns of the VFO dial per second).
      //CONT      This implements an over-proportional tuning speed if
      //CONT      turning the VFO knob faster and faster.
      //ENDDEF
      if (command[6] == ';') {
        int steps = 10 * (command[4] - '0') + (command[5] - '0');

        if (steps <= 30) {
          steps = andromeda_vfo_speedup[steps];
        } else {
          steps *= andromeda_vfo_speedup[31];
        }

        schedule_action(VFO, RELATIVE, -steps);
      } else {
        // unexpected command format
        implemented = FALSE;
      }

      break;

    case 'E': //ZZZE ANDROMEDA command

      //CATDEF    ZZZE
      //DESCR     Handle ANDROMEDA encoders
      //SET       ZZZExxy;
      //NOTE      ANDROMEDA extension.
      //NOTE      x encodes the encoder and the direction.
      //NOTE      x= 1-20 maps to encoder 1-20, clockwise
      //NOTE      x=51-70 maps to encoder 1-20, counter clockwise
      //NOTE      y=0-9 is the number of ticks
      //NOTE
      //ENDDEF
      if (command[7] == ';') {
        int v, p;
        p = 10 * (command[4] - '0') + (command[5] - '0');
        v = command[6] - '0';

        if (p > 50) {
          p -= 50;
          v = -v;
        }

        if (v == 0) {
          // This should not happen, but if, just do nothing
          break;
        }

        //
        // At this place, p is the encoder number (1...20) and
        // v the number of ticks (-9 ... 9)
        //

        if (client->andromeda_type == 1) {
          //
          // ANDROMEDA console with six double-encoders
          // Enc1/2, ..., Enc11/12. The silk print given
          // below comes from the ApacheLabs document
          // 1022_Andromeda-manual-v1.pdf.
          // Note that Enc11 is here implemented as MICgain
          // but the silk print reads MULTI.
          //
          if (!locked) switch (p) {
            // Enc1/2: "RX1 AF/RF"
            case 1:
              schedule_action(AF_GAIN_RX1, RELATIVE, v);
              break;

            case 2:
              schedule_action(AGC_GAIN_RX1, RELATIVE, v);
              break;

            // Enc3/4: "RX2 AF/RF"
            case 3:
              schedule_action(AF_GAIN_RX2, RELATIVE, v);
              break;

            case 4:
              schedule_action(AGC_GAIN_RX2, RELATIVE, v);
              break;

            // Enc5/6: "IF FILTER HIGH/LOW CUT"
            case 5:
              schedule_action(FILTER_CUT_HIGH, RELATIVE, v);
              break;

            case 6:
              schedule_action(FILTER_CUT_LOW, RELATIVE, v);
              break;

            // Enc7/8: "DIVERSITY GAIN/PHASE"
            case 7:
              schedule_action(DIV_GAIN, RELATIVE, v);
              break;

            case 8:
              schedule_action(DIV_PHASE, RELATIVE, v);
              break;

            // Enc9/10: "RIT/XIT"
            case 9: // RIT of the VFO of the active receiver
              schedule_action(RIT, RELATIVE, v);
              break;

            case 10:
              schedule_action(XIT, RELATIVE, v);
              break;

            //Enc11/12: "MULTI/DRIVE", but here implemented as "MIC/DRIVE"
            case 11:
              schedule_action(MIC_GAIN, RELATIVE, v);
              break;

            case 12:
              schedule_action(DRIVE, RELATIVE, v);
              break;
            }
        }

        if (client->andromeda_type == 4) {
          //
          // upgraded G2Mk1 panel encoders
          //
          switch (p) {
          case 1:  // left edge lower encoder inner knob, silk print: "RX AF/AGC", default: AF_GAIN
            schedule_action(AF_GAIN, RELATIVE, v);
            break;

          case 2:  // left edge lower encoder outer knob, silk print: "RX AF/AGC", default: AGC_GAIN
            schedule_action(AGC_GAIN, RELATIVE, v);
            break;

          case 5:  // right edge upper encoder inner knob, silk print: "Multi 1" (?), default: FILTER_CUT_HIGH
            schedule_action(FILTER_CUT_HIGH, RELATIVE, v);
            break;

          case 6:  // right edge upper encoder outer knob, silk print: "Multi 1" (?), default: FILTER_CUT_LOW
            schedule_action(FILTER_CUT_LOW, RELATIVE, v);
            break;

          case 9:  // right edge lower encoder inner knob, silk print: "Multi 2" (?), default: RIT
            schedule_action(RIT, RELATIVE, v);
            break;

          case 10:  // right edge lower encoder outer knob, silk print: "Multi 2" (?), default: XIT
            schedule_action(XIT, RELATIVE, v);
            break;

          case 11:  // left edge upper encoder inner knob, silk print: "MIC/DRIVE", default: MULTI_ENC
            schedule_action(MULTI_ENC, RELATIVE, v);
            break;

          case 12:  // left edge upper encoder outer knob, silk print: "MIC/DRIVE", default: TX_DRIVE
            schedule_action(DRIVE, RELATIVE, v);
            break;
          }
        }

        if (client->andromeda_type == 5) {
          //
          // G2 Ultra panel encoders
          //
          switch (p) {
          case 1:  // left edge lower encoder, inner knob, silk print: "RX2 AF/AGC", default: AF_GAIN_RX2
            schedule_action(AF_GAIN_RX2, RELATIVE, v);
            break;

          case 2:  // left edge lower encoder, outer knob, silk print: "RX2 AF/AGC", default: AGC_GAIN_RX2
            schedule_action(AGC_GAIN_RX2, RELATIVE, v);
            break;

          case 3: // left edge upper encoder (directly below power button), inner knob, silk print: "RX1 AF/AGC", default: AF_GAIN_RX1
            schedule_action(AF_GAIN_RX1, RELATIVE, v);
            break;

          case 4:  // left edge upper encoder (directly below power button), outer knob, silk print: "RX1 AF/AGC", default: AGC_GAIN_RX1
            schedule_action(AGC_GAIN_RX1, RELATIVE, v);
            break;

          case 5:  // encoder between power button and screen, inner knob, silk print: "DRIVE/MULTI", default: MULTI_ENC
            schedule_action(MULTI_ENC, RELATIVE, v);
            break;

          case 6:  // encoder between power button and screen, outer knob, silk print: "DRIVE/MULTI", default: DRIVE
            schedule_action(DRIVE, RELATIVE, v);
            break;

          case 7:  // right edge lower encoder inner knob, silk print: "RIT/ATTN", default: RIXXIT
            schedule_action(RITXIT, RELATIVE, v);
            break;

          case 8:  // right edge lower encoder outer knob, silk print: "RIT/ATTN", default: ATTENUATION
            schedule_action(ATTENUATION, RELATIVE, v);
            break;

          case 9:  // right edge upper encoder inner knob, (shift OFF), silk print:" MULTI 2" (?), default: FILTER_CUT_HIGH
            schedule_action(FILTER_CUT_HIGH, RELATIVE, v);
            break;

          case 10:  // right edge upper encoder outer knob (shift OFF), silk print:" MULTI 2" (?), default: FILTER_CUT_LOW
            schedule_action(FILTER_CUT_LOW, RELATIVE, v);
            break;

          case 11:  // right edge upper encoder inner knob, (shift ON), silk print:" MULTI 2" (?), default: DIVERSITY_GAIN
            schedule_action(DIV_GAIN, RELATIVE, v);
            break;

          case 12:  // right edge upper encoder outer knob, (shift ON), silk print:" MULTI 2" (?), default: DIVERSITY_PHASE
            schedule_action(DIV_PHASE, RELATIVE, v);
            break;
          }
        } // end of G2Mk2 console
      } else {
        // unexpected command format
        implemented = FALSE;
      }

      break;

    case 'I': //ZZZI ANDROMEDA info
      //CATDEF    ZZZI
      //DESCR     ANDROMEDA reports
      //RESP      ZZZIxxy;
      //NOTE      Automatic generated response for ANDROMEDA controller.
      //NOTE      The LED with number x shall be switched on (y=1)
      //NOTE      or off (y=0).
      //ENDDEF
      implemented = FALSE;  // this command should never ARRIVE from the console
      break;

    case 'P': //ZZZP ANDROMEDA command

      //CATDEF    ZZZP
      //DESCR     Handle ANDROMEDA push-buttons
      //SET       ZZZPxxy;
      //NOTE      ANDROMEDA extension.
      //NOTE      x encodes the button and y means released (y=0),
      //NOTE      pressed(y=1) or pressed for a longer time (y=2).
      //ENDDEF
      if (command[7] == ';') {
        int p = 10 * (command[4] - '0') + (command[5] - '0');
        int v = (command[6] - '0');

        //
        // The Andromeda console will send a v=0 --> v=1 --> v=0 sequence for a short press,
        // so we have characteristic transitions tr01 (0-->1, upon pressing), tr12 (1-->2, after waiting),
        // tr10 (1-->0, upon release) and tr20 (2-->0, upon release). For any button, either the sequence
        // {tr01,tr01} is generated (short press) or the sequence {tr01,tr12,tr20} (long press).
        //
        // We have to distinguish "normal" buttons from "long" buttons. "normal" buttons make no difference
        // between a short and a long press but "long" buttons may generate different actions for short
        // and long presses.
        //
        // "Normal" buttons should generate a "PRESSED" upon tr01 and (if required) a "RELEASED" upon (tr10 || tr20).
        // "Long" buttons generate a "PRESSED" for the short-press event upon tr10,
        // and a "PRESSED" for the long-press event upon tr12.
        //
        // ATTENTION: no RELEASE event is ever triggered for a "long" button. Such events are currently required
        //            only for PTT, RIT_PLUS, RIT_MINS, XIT_PLUS, XIT_MINUS, and CW keyer actions, which may be associated
        //            to the "function keys" F1-F8 on the original ANDROMEDA console.
        //            In all other cases, there is no need to bother the system with RELEASE events.
        //
        // NOTE: Rick's code for the original ANDROMEDA console remains unchanged.
        //
        //

        if (client->andromeda_type == 1) {
          //
          // Rick's original ANDROMEDA console, ending with a "break"
          //
          static int numpad_active = 0;
          static int longpress = 0;

          if (!numpad_active) switch (p) {
            case 21: // Function Switches
            case 22:
            case 23:
            case 24:
            case 25:
            case 26:
            case 27:
            case 28:
              schedule_action(toolbar_switches[p - 21].switch_function, (v == 0) ? PRESSED : RELEASED, 0);
              snprintf(reply, 256, "ZZZI11%d;", locked);
              send_resp(client->fd, reply);
              break;

            case 46: // SDR On
              if (v == 0) {
                if (longpress) {
                  longpress = 0;
                } else {
                  static int startstop = 1;
                  startstop ^= 1;
                  startstop ? radio_protocol_run() : radio_protocol_stop();
                }
              } else if (v == 2) {
                new_menu();
                longpress = 1;
              }

              break;
            }

          if (numpad_active && v == 0) switch (p) {
            case 30: // Band Buttons
              schedule_action(NUMPAD_1, PRESSED, 0);
              break;

            case 31:
              schedule_action(NUMPAD_2, PRESSED, 0);
              break;

            case 32:
              schedule_action(NUMPAD_3, PRESSED, 0);
              break;

            case 33:
              schedule_action(NUMPAD_4, PRESSED, 0);
              break;

            case 34:
              schedule_action(NUMPAD_5, PRESSED, 0);
              break;

            case 35:
              schedule_action(NUMPAD_6, PRESSED, 0);
              break;

            case 36:
              schedule_action(NUMPAD_7, PRESSED, 0);
              break;

            case 37:
              schedule_action(NUMPAD_8, PRESSED, 0);
              break;

            case 38:
              schedule_action(NUMPAD_9, PRESSED, 0);
              break;

            case 39:
              schedule_action(NUMPAD_DEC, PRESSED, 0);
              break;

            case 40:
              schedule_action(NUMPAD_0, PRESSED, 0);
              break;

            case 41: {
              schedule_action(NUMPAD_ENTER, PRESSED, 0);
              numpad_active = 0;
              locked = 0;
            }
            break;

            case 45: {
              schedule_action(NUMPAD_MHZ, PRESSED, 0);
              numpad_active = 0;
              locked = 0;
            }
            } else if (!locked) switch (p) {
              static int shift = 0;

            case 1: // Rx1 AF Mute
              if (v == 0) { receiver[0]->mute_radio ^= 1; }

              break;

            case 3: // Rx2 AF Mute
              if (v == 0) { receiver[1]->mute_radio ^= 1; }

              break;

            case 5: // Filter Cut Defaults
              schedule_action(FILTER_CUT_DEFAULT, (v == 0) ? PRESSED : RELEASED, 0);
              break;

            case 7: // Diversity Enable
              if (RECEIVERS == 2 && n_adc > 1) {
                schedule_action(DIV, (v == 0) ? PRESSED : RELEASED, 0);

                if (v == 0) {
                  snprintf(reply, 256, "ZZZI05%d;", diversity_enabled ^ 1);
                  send_resp(client->fd, reply);
                }
              }

              break;

            case 9: // RIT/XIT Clear
              schedule_action(RIT_CLEAR, (v == 0) ? PRESSED : RELEASED, 0);
              schedule_action(XIT_CLEAR, (v == 0) ? PRESSED : RELEASED, 0);
              snprintf(reply, 256, "ZZZI080;");
              send_resp(client->fd, reply);
              snprintf(reply, 256, "ZZZI090;");
              send_resp(client->fd, reply);
              break;

            case 29: // Shift
              if (v == 0) {
                shift ^= 1;
                snprintf(reply, 256, "ZZZI06%d;", shift);
                send_resp(client->fd, reply);
              }

              break;

            case 30: // Band Buttons
            case 31:
            case 32:
            case 33:
            case 34:
            case 35:
            case 36:
            case 37:
            case 38:
            case 39:
            case 40:
            case 41:
              if (shift && v == 0) {
                int band = band20;

                if (p == 30) { band = band160; }
                else if (p == 31) { band = band80; }
                else if (p == 32) { band = band60; }
                else if (p == 33) { band = band40; }
                else if (p == 34) { band = band30; }
                else if (p == 35) { band = band20; }
                else if (p == 36) { band = band17; }
                else if (p == 37) { band = band15; }
                else if (p == 38) { band = band12; }
                else if (p == 39) { band = band10; }
                else if (p == 40) { band = band6; }
                else if (p == 41) { band = bandGen; }

                vfo_band_changed(active_receiver->id ? VFO_B : VFO_A, band);
                shift = 0;
                snprintf(reply, 256, "ZZZI060;");
                send_resp(client->fd, reply);
              } else if (!shift && v == 1) {
                if (p == 30) { start_tx(); }                                  // MODE DATA
                else if (p == 31) { schedule_action(MODE_PLUS, PRESSED, 0); } // MODE+
                else if (p == 32) { schedule_action(FILTER_PLUS, PRESSED, 0); } // FILTER+
                else if (p == 33) { radio_change_receivers(receivers == 1 ? 2 : 1); } // RX2
                else if (p == 34) { schedule_action(MODE_MINUS, PRESSED, 0); } // MODE-
                else if (p == 35) { schedule_action(FILTER_MINUS, PRESSED, 0); } // FILTER-
                else if (p == 36) { schedule_action(A_TO_B, PRESSED, 0); }    // A>B
                else if (p == 37) { schedule_action(B_TO_A, PRESSED, 0); }    // B>A
                else if (p == 38) { schedule_action(SPLIT, PRESSED, 0); }     // SPLIT
                else if (p == 39) { schedule_action(NB, PRESSED, 0); }        // U1 (use NB)
                else if (p == 40) { schedule_action(NR, PRESSED, 0); }        // U2 (use NR)
              } else if (p == 41) {
                if (v == 0 || v == 2) {
                  numpad_active = 1;
                  locked = 1;
                  g_idle_add(ext_vfo_update, NULL);
                  schedule_action(NUMPAD_CL, PRESSED, 0);               // U3 start Freq entry
                }
              }

              break;

            case 42: // RIT/XIT
              if (v == 0) {
                if (!vfo[active_receiver->id].rit_enabled && !vfo[vfo_get_tx_vfo()].xit_enabled) {
                  // neither RIT nor XIT: ==> activate RIT
                  vfo_rit_onoff(active_receiver->id, 1);
                  snprintf(reply, 256, "ZZZI081;");
                  send_resp(client->fd, reply);
                } else if (vfo[active_receiver->id].rit_enabled && !vfo[vfo_get_tx_vfo()].xit_enabled) {
                  // RIT but no XIT: ==> de-activate RIT and activate XIT
                  vfo_rit_onoff(active_receiver->id, 0);
                  vfo_xit_onoff(1);
                  snprintf(reply, 256, "ZZZI080;");
                  send_resp(client->fd, reply);
                  snprintf(reply, 256, "ZZZI091;");
                  send_resp(client->fd, reply);
                } else {
                  // else deactivate both.
                  vfo_rit_onoff(active_receiver->id, 0);
                  vfo_xit_onoff(0);
                  snprintf(reply, 256, "ZZZI080;");
                  send_resp(client->fd, reply);
                  snprintf(reply, 256, "ZZZI090;");
                  send_resp(client->fd, reply);
                }

                g_idle_add(ext_vfo_update, NULL);
              }

              break;

            case 43: // switch receivers
              if (receivers == 2) {
                if (v == 0) {
                  if (active_receiver->id == 0) {
                    schedule_action(RX2, PRESSED, 0);
                    snprintf(reply, 256, "ZZZI07%d;", vfo[VFO_B].ctun);
                    send_resp(client->fd, reply);
                    snprintf(reply, 256, "ZZZI08%d;", vfo[VFO_B].rit_enabled);
                    send_resp(client->fd, reply);
                    snprintf(reply, 256, "ZZZI100;");
                  } else {
                    schedule_action(RX1, PRESSED, 0);
                    snprintf(reply, 256, "ZZZI07%d;", vfo[VFO_A].ctun);
                    send_resp(client->fd, reply);
                    snprintf(reply, 256, "ZZZI08%d;", vfo[VFO_A].rit_enabled);
                    send_resp(client->fd, reply);
                    snprintf(reply, 256, "ZZZI101;");
                  }

                  send_resp(client->fd, reply);
                  g_idle_add(ext_vfo_update, NULL);
                }
              }

              break;

            case 45: // ctune
              if (v == 1) {
                schedule_action(CTUN, PRESSED, 0);
                snprintf(reply, 256, "ZZZI07%d;", vfo[active_receiver->id].ctun ^ 1);
                send_resp(client->fd, reply);
                g_idle_add(ext_vfo_update, NULL);
              }

              break;

            case 47: // MOX
              if (v == 0) {
                snprintf(reply, 256, "ZZZI01%d;", mox);
                send_resp(client->fd, reply);
              } else {
                radio_mox_update(mox ^ 1);
              }

              break;

            case 48: // TUNE
              if (v == 0) {
                snprintf(reply, 256, "ZZZI03%d;", tune);
                send_resp(client->fd, reply);
              } else {
                radio_tune_update(tune ^ 1);
              }

              break;

            case 50: // TWO TONE
              schedule_action(TWO_TONE, (v == 0) ? PRESSED : RELEASED, 0);
              break;

            case 49: // PS ON
              if (v == 0) {
                if (longpress) {
                  longpress = 0;
                } else {
                  if (can_transmit) {
                    tx_ps_onoff(transmitter, NOT(transmitter->puresignal));
                    snprintf(reply, 256, "ZZZI04%d;", transmitter->puresignal);
                    send_resp(client->fd, reply);
                  }
                }
              } else if (v == 2) {
                start_ps();
                longpress = 1;
              }

              break;
            }

          if (p == 44) { // VFO lock
            if (v == 0) {
              if (numpad_active) {
                schedule_action(NUMPAD_KHZ, PRESSED, 0);
                numpad_active = 0;
                locked = 0;
              } else {
                locked ^= 1;
                g_idle_add(ext_vfo_update, NULL);
                snprintf(reply, 256, "ZZZI11%d;", locked);
                send_resp(client->fd, reply);
              }
            }
          }

          break;
        } // end of the "type=1" section

        int tr01 = 0;  // indicates a v=0 --> v=1 transision
        int tr12 = 0;  // indicates a v=1 --> v=2 transision
        int tr10 = 0;  // indicates a v=1 --> v=0 transision
        int tr20 __attribute__((unused)) = 0;  // indicates a v=2 --> v=0 transision

        if (client->last_v == 0 && v == 1) { tr01 = 1; }

        if (client->last_v == 1 && v == 2) { tr12 = 1; }

        if (client->last_v == 1 && v == 0) { tr10 = 1; }

        if (client->last_v == 2 && v == 0) { tr20 = 1; }

        client->last_v = v;

        if (client->andromeda_type == 4) {
          //
          // upgraded G2Mk1 panel push-buttons (including those of the encoders)
          //
          switch (p) {
          case 1:  // left edge lower encoder push-button, silk print: "RX AF/AGC", default: MUTE
            if (tr01) { schedule_action(MUTE, PRESSED, 0); }

            break;

          case 5:  // right edge upper encoder push-button, silk print: "Multi 1", default: FILTER_CUT_DEFAULT
            if (tr01) { schedule_action(FILTER_CUT_DEFAULT, PRESSED, 0); }

            break;

          case 9:  // right edge lower encoder push-button, silk print: "Multi 2", default: RITXIT_CLEAR
            if (tr01) { schedule_action(RITXIT_CLEAR, PRESSED, 0); }

            break;

          case 11:  // left edge upper encoder push-button, silk print: "MIC/DRIVE", default: MULTI_BUTTON
            if (tr01) { schedule_action(MULTI_BUTTON, PRESSED, 0); }

            break;

          case 21:  // 4x3 pad, row 4, column 1, silk print: "FCN", default: FUNCTION
            if (tr01) { schedule_action(FUNCTION, PRESSED, 0); }

            break;

          case 30:  // 4x3 pad, row 1, column 3, silk print: "Band+", default: BAND_PLUS
            if (tr01) { schedule_action(BAND_PLUS, PRESSED, 0); }

            break;

          case 31:  // 4x3 pad, row 1, column 1, silk print: "Mode+", default: MODE_PLUS
            if (tr01) { schedule_action(MODE_PLUS, PRESSED, 0); }

            break;

          case 32:  // 4x3 pad, row 1, column 2, silk print: "Fil+", default: FILTER_PLUS
            if (tr01) { schedule_action(FILTER_PLUS, PRESSED, 0); }

            break;

          case 33:  // 4x3 pad, row 2, column 3, silk print: "Band-", default: BAND_MINUS
            if (tr01) { schedule_action(BAND_MINUS, PRESSED, 0); }

            break;

          case 34:  // 4x3 pad, row 2, column 1, silk print: "Mode-", default: MODE_MINUX
            if (tr01) { schedule_action(MODE_MINUS, PRESSED, 0); }

            break;

          case 35:  // 4x3 pad, row 2, column 3, silk print: "Fil-", default: FILTER_MINUS
            if (tr01) { schedule_action(FILTER_MINUS, PRESSED, 0); }

            break;

          case 36:  // 4x3 pad, row 3, column 1, silk print: "A>B", default: A_TO_B
            if (tr01) { schedule_action(A_TO_B, PRESSED, 0); }

            break;

          case 37:  // 4x3 pad, row 3, column 2, silk print: "B>A", default: B_TO_A
            if (tr01) { schedule_action(B_TO_A, PRESSED, 0); }

            break;

          case 38: // 4x3 pad, row 3, column 3, silk print: "Split", default: SPLIT
            if (tr01) { schedule_action(SPLIT, PRESSED, 0); }

            break;

          case 42:  // 4x3 pad, row 4, column 2, silk print: "RIT", default: RIT_ENABLE
            if (tr01) { schedule_action(RIT_ENABLE, PRESSED, 0); }

            break;

          case 43:  // 4x3 pad, row 4, column 3, silk print: "XIT", default: XIT_ENABLE
            if (tr01) { schedule_action(XIT_ENABLE, PRESSED, 0); }

            break;

          case 44:  // right button below VFO knob, silk print: "LOCK", default: LOCK
            if (tr01) { schedule_action(LOCK, PRESSED, 0); }

            break;

          case 45:  // left button below VFO knob, silk print: "CTUNE", default: CTUN
            if (tr01) { schedule_action(CTUN, PRESSED, 0); }

            break;

          case 47:  // upper button left of the screen, silk print: "MOX", default: MOX
            if (tr01) { schedule_action(MOX, PRESSED, 0); }

            break;

          case 50:  // lower button left of screen, silk print: "2TONE/TUNE", default: TUNE
            if (tr01) { schedule_action(TUNE, PRESSED, 0); }

            break;
          }
        }

        if (client->andromeda_type == 5) {
          //
          // G2 Ultra panel push-buttons, including encoder buttons
          //
          switch (p) {
          case 1:  // left edge lower encoder, push-button, silk print: "RX2 AF/AGC", default: MUTE_RX2
            if (tr01) { schedule_action(MUTE_RX2, PRESSED, 0); }

            break;

          case 2:  // left edge upper encoder (directly below power button), push-button, silk print: "RX1 AF/AGC", default: MUTE_RX1
            if (tr01) { schedule_action(MUTE_RX1, PRESSED, 0); }

            break;

          case 3: // encoder between power button and screen, push-button, silk print: "DRIVE/MULTI", default: MULTI_BUTTON
            if (tr01) { schedule_action(MULTI_BUTTON, PRESSED, 0); }

            break;

          case 4:  // lowest of the four buttons left of the screen, silk print: "ATU", not yet used
            break;

          case 5:  // second-lowest of the four buttons left of the screen, silk print: "2TONE", default: TWO_TONE
            if (tr01) { schedule_action(TWO_TONE, PRESSED, 0); }

            break;

          case 6:  // second-highest of the four buttons left of the screen, silk print: "TUNE", default: TUNE
            if (tr01) { schedule_action(TUNE, PRESSED, 0); }

            break;

          case 7:  // highest of the four buttons left of the screen, silk print: "MOX", default: MOX
            if (tr01) { schedule_action(MOX, PRESSED, 0); }

            break;

          case 8: // lower left of the VFO knob, silk print: "CTUNE", default: CTUN
            if (tr01) { schedule_action(CTUN, PRESSED, 0); }

            break;

          case 9: // lower right of the VFO knob, silk print: "LOCK", default: LOCK
            if (tr01) { schedule_action(LOCK, PRESSED, 0); }

            break;

          case 10: // button with silk print "A/B", default: SWAP_RX
            if (tr01) { schedule_action(SWAP_RX, PRESSED, 0); }

            break;

          case 11: // button with silk print "RIT/XIT", default: RITSELECT
            if (tr01) { schedule_action(RITSELECT, PRESSED, 0); }

            break;

          case 12: // right edge lower encoder, push-button, silk print: "RIT/XIT", default: RITCIT_CLEAR
            if (tr01) { schedule_action(RITXIT_CLEAR, PRESSED, 0); }

            break;

          case 13:  // right edge upper encoder, push-button, SHIFT OFF, silk print: "MULTI 2", default: FILTER_CUT_DEFAULT
            if (tr01) { schedule_action(FILTER_CUT_DEFAULT, PRESSED, 0); }

            break;

          case 14:  // 4x3 pad row 1 col 1, silk print: "160/MODE+", "no Band", default: MODE_PLUS, long: MENU_MODE
            if (tr10) { schedule_action(MODE_PLUS, PRESSED, 0); }

            if (tr12) { schedule_action(MENU_MODE, PRESSED, 0); }

            break;

          case 15:  // 4x3 pad row 1 col 2, silk print: "80/FIL+", "no Band", default: FILTER_PLUS, long: MENU_FILTER
            if (tr10) { schedule_action(FILTER_PLUS, PRESSED, 0); }

            if (tr12) { schedule_action(MENU_FILTER, PRESSED, 0); }

            break;

          case 16:  // 4x3 pad row 1 col 3, silk print: "60/BAND+", "no Band", default: BAND_PLUS, long: MENU_BAND
            if (tr01) { schedule_action(BAND_PLUS, PRESSED, 0); }

            if (tr12) { schedule_action(MENU_BAND, PRESSED, 0); }

            break;

          case 17:  // 4x3 pad row 2 col 1, silk print: "40/MODE-", "no Band", default: MODE_MINUS
            if (tr01) { schedule_action(MODE_MINUS, PRESSED, 0); }

            break;

          case 18:  // 4x3 pad row 2 col 2, silk print: "30/FIL-", "no Band", default: FILTER_MINUS
            if (tr01) { schedule_action(FILTER_MINUS, PRESSED, 0); }

            break;

          case 19:  // 4x3 pad row 2 col 3, silk print: "20/BAND-", "no Band", default: BAND_MINUS
            if (tr01) { schedule_action(BAND_MINUS, PRESSED, 0); }

            break;

          case 20:  // 4x3 pad row 3 col 1, silk print: "17/A>B", "no Band", default: A_TO_B
            if (tr01) { schedule_action(A_TO_B, PRESSED, 0); }

            break;

          case 21:  // 4x3 pad row 3 col 2, silk print: "15/B>A", "no Band", default: B_TO_A
            if (tr01) { schedule_action(B_TO_A, PRESSED, 0); }

            break;

          case 22:  // 4x3 pad row 3 col 3, silk print: "12/SPLIT", "no Band", default: SPLIT
            if (tr01) { schedule_action(SPLIT, PRESSED, 0); }

            break;

          case 23:  // 4x3 pad row 4 col 1, silk print: "10/F1", "no Band", default: SNB, long: MENU_NOISE
            if (tr10) { schedule_action(SNB, PRESSED, 0); }

            if (tr12) { schedule_action(MENU_NOISE, PRESSED, 0); }

            break;

          case 24:  // 4x3 pad row 4 col 2, silk print: "6/F2", "no Band", default: NB, long: MENU_NOISE
            if (tr10) { schedule_action(NB, PRESSED, 0); }

            if (tr12) { schedule_action(MENU_NOISE, PRESSED, 0); }

            break;

          case 25:  // 4x3 pad row 4 col 3, silk print: "LF/HF/F3", "no Band", default: NR, long: MENU_NOISE
            if (tr10) { schedule_action(NR, PRESSED, 0); }

            if (tr12) { schedule_action(MENU_NOISE, PRESSED, 0); }

            break;

          case 26:  // unused
            break;

          case 27:  // 4x3 pad row 1 col 1, silk print: "160/MODE+", "Band", default: BAND_160
            if (tr01) { schedule_action(BAND_160, PRESSED, 0); }

            break;

          case 28:  // 4x3 pad row 1 col 2, silk print: "80/FIL+", "Band", default: BAND_80
            if (tr01) { schedule_action(BAND_80, PRESSED, 0); }

            break;

          case 29:  // 4x3 pad row 1 col 3, silk print: "60/BAND+", "Band", default: BAND_60
            if (tr01) { schedule_action(BAND_60, PRESSED, 0); }

            break;

          case 30:  // 4x3 pad row 2 col 1, silk print: "40/MODE-", "Band", default: BAND_40
            if (tr01) { schedule_action(BAND_40, PRESSED, 0); }

            break;

          case 31:  // 4x3 pad row 2 col 2, silk print: "30/FIL-", "Band", default: BAND_30
            if (tr01) { schedule_action(BAND_30, PRESSED, 0); }

            break;

          case 32:  // 4x3 pad row 2 col 3, silk print: "20/BAND-", "Band", default: BAND_20
            if (tr01) { schedule_action(BAND_20, PRESSED, 0); }

            break;

          case 33:  // 4x3 pad row 3 col 1, silk print: "17/A>B", "Band", default: BAND_17
            if (tr01) { schedule_action(BAND_17, PRESSED, 0); }

            break;

          case 34:  // 4x3 pad row 3 col 2, silk print: "15/B>A", "Band", default: BAND_15
            if (tr01) { schedule_action(BAND_15, PRESSED, 0); }

            break;

          case 35:  // 4x3 pad row 3 col 3, silk print: "12/SPLIT", "Band", default: BAND_12
            if (tr01) { schedule_action(BAND_12, PRESSED, 0); }

            break;

          case 36:  // 4x3 pad row 4 col 1, silk print: "10/F1", "Band", default: BAND_10
            if (tr01) { schedule_action(BAND_10, PRESSED, 0); }

            break;

          case 37:  // 4x3 pad row 4 col 2, silk print: "6/F2", "Band", default: BAND_6
            if (tr01) { schedule_action(BAND_6, PRESSED, 0); }

            break;

          case 38:  // 4x3 pad row 4 col 3, silk print: "LF/HF/F3", "Band", default: BAND_136
            if (tr01) { schedule_action(BAND_136, PRESSED, 0); }

            break;

          case 39:  // Reserved
            break;

          case 40:  // Reserved
            break;

          case 41:  // right edge upper encoder, push-button, SHIFT ON, silk print: "MULTI 2", default: DIV
            if (tr01) { schedule_action(DIV, PRESSED, 0); }

            break;
          }
        }    // end of G2Mk2 ZZZP code

        //
        // Schedule LED update
        //
        g_idle_add(andromeda_oneshot_handler, (gpointer) client);
      } else {
        // all ANDROMEDA types, unexpected command format
        implemented = FALSE;
      }

      break;

    case 'S': //ZZZS ANDROMEDA command

      //CATDEF    ZZZS
      //DESCR     Log ANDROMEDA version
      //SET       ZZZSxxyyzzz;
      //NOTE      ANDROMEDA extension.
      //NOTE      The ANDROMEDA type (x), hardware (y) and
      //CONT      software (z) version is printed in the log file.
      //CONT      The type (x) sent by a client does affect the
      //CONT      processing of ZZZE and ZZZP commands from that client.
      //CONT      Only the cases x=1 (original ANDROMEDA console)
      //CONT      and x=5 (G2 Ultra console) are implemented.
      //ENDDEF
      if (command[11] == ';') {
        //
        // Besides logging, store the ANDROMEDA type in the client data structure
        //
        client->andromeda_type = 10 * (command[4] - '0') + (command[5] - '0');
        t_print("RIGCTL:INFO: Andromeda Client: Type:%c%c h/w:%c%c s/w:%c%c%c\n",
                command[4], command[5],
                command[6], command[7], command[8], command[9], command[10]);
      }

      break;

    case 'U': //ZZZU ANDROMEDA command operating on VFO of active receiver

      //CATDEF    ZZZU
      //DESCR     Move up frequency of active receiver
      //SET       ZZZUxx;
      //NOTE      ANDROMEDA extension. x = number of steps.
      //NOTE      For x>10, the number of VFO steps is multiplied with
      //CONT      a speed-up factor that increases up to 4 at x=30
      //CONT      (corresponds to 3 turns of the VFO dial per second).
      //CONT      This implements an over-proportional tuning speed if
      //CONT      turning the VFO knob faster and faster.
      //ENDDEF
      if (command[6] == ';') {
        int steps = 10 * (command[4] - '0') + (command[5] - '0');

        if (steps <= 30) {
          steps = andromeda_vfo_speedup[steps];
        } else {
          steps *= andromeda_vfo_speedup[31];
        }

        schedule_action(VFO, RELATIVE, steps);
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  default:
    implemented = FALSE;
    break;
  }

  return implemented;
}

// called with g_idle_add so that the processing is running on the main thread
int parse_cmd(void *data) {
  COMMAND *info = (COMMAND *)data;
  CLIENT *client = info->client;
  char *command = info->command;
  char reply[256];
  reply[0] = '\0';
  gboolean implemented = TRUE;

  switch (command[0]) {
  case '#':

    //CATDEF    \#S
    //DESCR     Shutdown Console
    //SET       \#S;
    //ENDDEF
    if (command[1] == 'S' && command[2] == ';') {
      schedule_action(SHUTDOWN, PRESSED, 0);
    } else {
      implemented = FALSE;
    }

    break;

  case 'A':
    switch (command[1]) {
    case 'C': //AC
      // set/read internal atu status
      implemented = FALSE;
      break;

    case 'G': //AG

      //CATDEF    AG
      //DESCR     Sets/Reads audio volume (AF slider)
      //SET       AGxyyy;
      //READ      AGx;
      //RESP      AGxyyy;
      //NOTE      x=0 sets RX1 audio volume, x=1 sets RX2 audio volume.
      //NOTE      y is 0...255 and mapped logarithmically to the volume -40...0 dB
      //ENDDEF
      if (command[3] == ';') {
        int id = SET(command[2] == '1');
        RXCHECK(id,
                snprintf(reply, 256, "AG%1d%03d;", id, (int)(255.0 * pow(10.0, 0.05 * receiver[id]->volume)));
                send_resp(client->fd, reply);
               )
      } else if (command[6] == ';') {
        int id = SET(command[2] == '1');
        int gain = atoi(&command[3]);
        double vol = (gain < 3) ? -40.0 : 20.0 * log10((double) gain / 255.0);
        RXCHECK(id, receiver[id]->volume = vol; set_af_gain(0, receiver[id]->volume));
      }

      break;

    case 'I': //AI

      //CATDEF    AI
      //DESCR     Sets/Reads auto reporting status
      //SET       AIx;
      //READ      AI;
      //RESP      AIx;
      //NOTE      x=0: auto-reporting disabled, x>0: enabled.
      //NOTE      Auto-reporting is affected for the client that sends this command.
      //CONT      For x=1, only frequency changes are sent via FA/FB commands.
      //CONT      For x>1, mode changes are also sent via MD commands.
      //ENDDEF
      if (command[2] == ';') {
        snprintf(reply, 256, "AI%d;", client->auto_reporting);
        send_resp(client->fd, reply) ;
      } else if (command[3] == ';') {
        client->auto_reporting = command[2] - '0';

        if (client->auto_reporting < 0) { client->auto_reporting = 0; }

        if (client->auto_reporting > 3) { client->auto_reporting = 3; }
      }

      break;

    case 'L': // AL
      // set/read Auto Notch level
      implemented = FALSE;
      break;

    case 'M': // AM
      // set/read Auto Mode
      implemented = FALSE;
      break;

    case 'N': // AN
      // set/read Antenna Connection
      implemented = FALSE;
      break;

    case 'S': // AS
      // set/read Auto Mode Function Parameters
      implemented = FALSE;
      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'B':
    switch (command[1]) {
    case 'C': //BC
      // set/read Beat Canceller
      implemented = FALSE;
      break;

    case 'D': //BD
      //CATDEF    BD
      //DESCR     VFO-A Band down
      //SET       BD;
      //NOTE      Wraps from the lowest to the highest band.
      //ENDDEF
      band_minus(receiver[0]->id);
      break;

    case 'P': //BP
      // set/read Manual Beat Canceller frequency
      implemented = FALSE;
      break;

    case 'U': //BU
      //CATDEF    BU
      //DESCR     VFO-A Band up
      //SET       BU;
      //NOTE      Wraps from the highest to the lowest band.
      //ENDDEF
      band_plus(receiver[0]->id);
      break;

    case 'Y': //BY
      // read busy signal
      implemented = FALSE;
      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'C':
    switch (command[1]) {
    case 'A': //CA
      // set/read CW Auto Tune
      implemented = FALSE;
      break;

    case 'G': //CG
      // set/read Carrier Gain
      implemented = FALSE;
      break;

    case 'I': //CI
      // sets the current frequency to the CALL Channel
      implemented = FALSE;
      break;

    case 'M': //CM
      // sets/reads the Packet Cluster Tune function
      implemented = FALSE;
      break;

    case 'N': //CN

      //CATDEF    CN
      //DESCR     Sets/Reads the CTCSS frequency
      //SET       CNxx;
      //READ      CN;
      //RESP      CNxx;
      //NOTE      x =  1...38. CTCSS frequencies in Hz are:
      //CONT      67.0 (x=1),  71.9 (x=2),  74.4 (x=3),  77.0 (x=4),
      //CONT      79.7 (x=5),  82.5 (x=6),  85.4 (x=7),  88.5 (x=8),
      //CONT      91.5 (x=9),  94.8 (x=10), 97.4 (x=11), 100.0 (x=12)
      //CONT      103.5 (x=13), 107.2 (x=14), 110.9 (x=15), 114.8 (x=16)
      //CONT      118.8 (x=17), 123.0 (x=18), 127.3 (x=19), 131.8 (x=20)
      //CONT      136.5 (x=21), 141.3 (x=22), 146.2 (x=23), 151.4 (x=24)
      //CONT      156.7 (x=25), 162.2 (x=26), 167.9 (x=27), 173.8 (x=28)
      //CONT      179.9 (x=29), 186.2 (x=30), 192.8 (x=31), 203.5 (x=32)
      //CONT      210.7 (x=33), 218.1 (x=34), 225.7 (x=35), 233.6 (x=36)
      //CONT      241.8 (x=37), 250.3 (x=38).
      //ENDDEF
      // sets/reads CTCSS function (frequency)
      if (can_transmit) {
        if (command[2] == ';') {
          snprintf(reply, 256, "CN%02d;", transmitter->ctcss + 1);
          send_resp(client->fd, reply) ;
        } else if (command[4] == ';') {
          transmitter->ctcss = atoi(&command[2]) - 1;
          tx_set_ctcss(transmitter);
          g_idle_add(ext_vfo_update, NULL);
        }
      }

      break;

    case 'T': //CT

      //CATDEF    CT
      //DESCR     Enable/Disable CTCSS
      //SET       CTx;
      //READ      CT;
      //RESP      CTx;
      //NOTE      x = 0: CTCSS off, x=1: on
      //ENDDEF
      if (can_transmit) {
        if (command[2] == ';') {
          snprintf(reply, 256, "CT%d;", transmitter->ctcss_enabled);
          send_resp(client->fd, reply) ;
        } else if (command[3] == ';') {
          transmitter->ctcss_enabled = SET(command[2] == '1');
          tx_set_ctcss(transmitter);
          g_idle_add(ext_vfo_update, NULL);
        }
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'D':
    switch (command[1]) {
    case 'C': //DC
      // set/read TX band status
      implemented = FALSE;
      break;

    case 'N': //DN
      //CATDEF    DN
      //DESCR     VFO-A down  one step
      //SET       DN;
      //NOTE      Parameters may be given, but are ignored.
      //ENDDEF
      vfo_id_step(VFO_A, -1);
      break;

    case 'Q': //DQ
      // set/read DCS function status
      implemented = FALSE;
      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'E':
    switch (command[1]) {
    case 'X': //EX
      // set/read the extension menu
      implemented = FALSE;
      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'F':
    switch (command[1]) {
    case 'A': //FA

      //CATDEF    FA
      //DESCR     Set/Read VFO-A frequency
      //SET       FAxxxxxxxxxxx;
      //READ      FA;
      //RESP      FAxxxxxxxxxxx;
      //NOTE      x in Hz, left-padded with zeroes
      //ENDDEF
      if (command[2] == ';') {
        if (vfo[VFO_A].ctun) {
          snprintf(reply, 256, "FA%011lld;", vfo[VFO_A].ctun_frequency);
        } else {
          snprintf(reply, 256, "FA%011lld;", vfo[VFO_A].frequency);
        }

        send_resp(client->fd, reply) ;
      } else if (command[13] == ';') {
        long long f = atoll(&command[2]);
        vfo_set_frequency(VFO_A, f);
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    case 'B': //FB

      //CATDEF    FB
      //DESCR     Set/Read VFO-B frequency
      //SET       FBxxxxxxxxxxx;
      //READ      FB;
      //RESP      FBxxxxxxxxxxx;
      //NOTE      x in Hz, left-padded with zeroes
      //ENDDEF
      if (command[2] == ';') {
        if (vfo[VFO_B].ctun) {
          snprintf(reply, 256, "FB%011lld;", vfo[VFO_B].ctun_frequency);
        } else {
          snprintf(reply, 256, "FB%011lld;", vfo[VFO_B].frequency);
        }

        send_resp(client->fd, reply) ;
      } else if (command[13] == ';') {
        long long f = atoll(&command[2]);
        vfo_set_frequency(VFO_B, f);
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    case 'C': //FC
      // set/read the sub receiver VFO frequency menu
      implemented = FALSE;
      break;

    case 'D': //FD
      // set/read the filter display dot pattern
      implemented = FALSE;
      break;

    case 'R': //FR

      //CATDEF    FR
      //DESCR     Set/Read active receiver
      //SET       FRx;
      //READ      FR;
      //RESP      FRx;
      //NOTE      x = 0 (RX1) or 1 (RX2)
      //ENDDEF
      if (command[2] == ';') {
        snprintf(reply, 256, "FR%d;", active_receiver->id);
        send_resp(client->fd, reply) ;
      } else if (command[3] == ';') {
        int id = SET(command[2] == '1');
        RXCHECK(id, schedule_action(id == 0 ? RX1 : RX2, PRESSED, 0));
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    case 'S': //FS
      // set/read the fine tune function status
      implemented = FALSE;
      break;

    case 'T': //FT

      //CATDEF    FT
      //DESCR     Set/Read Split status
      //SET       FTx;
      //READ      FT;
      //RESP      FTx;
      //NOTE      x=0: TX VFO is the VFO controlling the active receiver, x=1: the other VFO.
      //ENDDEF
      if (command[2] == ';') {
        snprintf(reply, 256, "FT%d;", split);
        send_resp(client->fd, reply) ;
      } else if (command[3] == ';') {
        int id = SET(command[2] == '1');
        radio_set_split(id);
      }

      break;

    case 'W': //FW

      //CATDEF    FW
      //DESCR     Set/Read VFO-A filter width (CW, AM, FM)
      //SET       FWxxxx;
      //READ      FW;
      //RESP      FWxxxx;
      //NOTE      When setting, this switches to the Var1 filter and sets its  width to x.
      //CONT      Only valid for CW, FM, AM. Use SH/SL for LSB, USB, DIGL, DIGU.
      //NOTE      For AM, 8kHz filter width (x=0) or  16 kHz (x$\ne$0)
      //NOTE      For FM, 2.5kHz deviation (x=0) or 5 kHz (x$\ne$0)
      //ENDDEF
      if (command[2] == ';') {
        int val = 0;
        FILTER *mode_filters = filters[vfo[VFO_A].mode];
        const FILTER *filter = &mode_filters[vfo[VFO_A].filter];

        switch (vfo[VFO_A].mode) {
        case modeCWL:
        case modeCWU:
          val = filter->low * 2;
          break;

        case modeAM:
        case modeSAM:
          val = filter->low >= -4000;
          break;

        case modeFMN:
          val = vfo[VFO_A].deviation == 5000;
          break;

        default:
          implemented = FALSE;
          break;
        }

        if (implemented) {
          snprintf(reply, 256, "FW%04d;", val);
          send_resp(client->fd, reply) ;
        }
      } else if (command[6] == ';') {
        // make sure filter is filterVar1
        if (vfo[VFO_A].filter != filterVar1) {
          vfo_id_filter_changed(VFO_A, filterVar1);
        }

        FILTER *mode_filters = filters[vfo[VFO_A].mode];
        FILTER *filter = &mode_filters[filterVar1];
        int fw = atoi(&command[2]);
        filter->low = fw;

        switch (vfo[VFO_A].mode) {
        case modeCWL:
        case modeCWU:
          filter->low = fw / 2;
          filter->high = fw / 2;
          break;

        case modeFMN:
          if (fw == 0) {
            filter->low = -5500;
            filter->high = 5500;
            vfo[VFO_A].deviation = 2500;
          } else {
            filter->low = -8000;
            filter->high = 8000;
            vfo[VFO_A].deviation = 5000;
          }

          rx_set_filter(receiver[0]);

          if (can_transmit) {
            tx_set_filter(transmitter);
          }

          g_idle_add(ext_vfo_update, NULL);
          break;

        case modeAM:
        case modeSAM:
          if (fw == 0) {
            filter->low = -4000;
            filter->high = 4000;
          } else {
            filter->low = -8000;
            filter->high = 8000;
          }

          break;

        default:
          implemented = FALSE;
          break;
        }

        if (implemented) {
          vfo_id_filter_changed(VFO_A, filterVar1);
          g_idle_add(ext_vfo_update, NULL);
        }
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'G':
    switch (command[1]) {
    case 'T': //GT

      //CATDEF    GT
      //DESCR     Set/Read RX1 AGC
      //SET       GTxxx;
      //READ      GT;
      //RESP      GTxxx;
      //NOTE      x=0: AGC OFF, x=5: LONG, x=10: SLOW,
      //CONT      x=15: MEDIUM, x=20: FAST.
      //ENDDEF
      if (command[2] == ';') {
        snprintf(reply, 256, "GT%03d;", receiver[0]->agc * 5);
        send_resp(client->fd, reply) ;
      } else if (command[5] == ';') {
        receiver[0]->agc = atoi(&command[2]) / 5;
        rx_set_agc(receiver[0]);
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'H':
    switch (command[1]) {
    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'I':
    switch (command[1]) {
    case 'D': //ID
      //CATDEF    ID
      //DESCR     Get radio model ID
      //READ      ID;
      //RESP      IDxxx;
      //NOTE      piHPSDR responds ID019; (so does the Kenwood TS-2000)
      //ENDDEF
      g_strlcpy(reply, "ID019;", sizeof(reply));
      send_resp(client->fd, reply);
      break;

    case 'F': { //IF
      //CATDEF    IF
      //DESCR     Get VFO-A Frequency/Mode etc.
      //READ      IF;
      //RESP      IFxxxxxxxxxxxyyyyzzzzzzsbc|ddefghikllm;
      //NOTE      x : VFO-A Frequency (11 digit)
      //NOTE      y : VFO-A step size
      //NOTE      z : VFO-A rit step size
      //NOTE      s : VFO-A rit enabled (0/1)
      //NOTE      b : VFO-A xit enabled (0/1)
      //NOTE      c : always 0
      //NOTE      d : always 0
      //NOTE      e : RX (e=0) or TX (e=1)
      //NOTE      f : mode (TS-2000 encoding, see MD command)
      //NOTE      g : always 0
      //NOTE      h : always 0
      //NOTE      i : Split enabled (i=1) or disabled (i=0)
      //NOTE      k : CTCSS enabled (i=2) or disabled (i=0)
      //NOTE      l : CTCSS frequency (1 - 38), see CN command
      //NOTE      m : always 0
      //ENDDEF
      int mode = ts2000_mode(vfo[VFO_A].mode);
      int tx_xit_en = 0;
      int tx_ctcss_en = 0;
      int tx_ctcss = 0;

      if (can_transmit) {
        tx_xit_en   = vfo[vfo_get_tx_vfo()].xit_enabled;
        tx_ctcss    = transmitter->ctcss + 1;
        tx_ctcss_en = transmitter->ctcss_enabled;
      }

      snprintf(reply, 256, "IF%011lld%04d%+06lld%d%d%d%02d%d%d%d%d%d%d%02d%d;",
               vfo[VFO_A].ctun ? vfo[VFO_A].ctun_frequency : vfo[VFO_A].frequency,
               vfo[VFO_A].step, vfo[VFO_A].rit, vfo[VFO_A].rit_enabled, tx_xit_en,
               0, 0, radio_is_transmitting(), mode, 0, 0, split, tx_ctcss_en ? 2 : 0, tx_ctcss, 0);
      send_resp(client->fd, reply);
    }
    break;

    case 'S': //IS

      //DO NOT DOCUMENT, THIS WILL BE REMOVED
      if (command[2] == ';') {
        g_strlcpy(reply, "IS 0000;", 256);
        send_resp(client->fd, reply);
      } else {
        implemented = FALSE;
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'J':
    switch (command[1]) {
    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'K':
    switch (command[1]) {
    case 'S': //KS

      //CATDEF    KS
      //DESCR     Set CW speed
      //SET       KSxxx;
      //READ      KS;
      //RESP      KSxxx;
      //NOTE      x (1 - 60) is in wpm
      //ENDDEF
      if (command[2] == ';') {
        snprintf(reply, 256, "KS%03d;", cw_keyer_speed);
        send_resp(client->fd, reply);
      } else if (command[5] == ';') {
        int speed = atoi(&command[2]);

        if (speed >= 1 && speed <= 60) {
          cw_keyer_speed = speed;
          keyer_update();
          g_idle_add(ext_vfo_update, NULL);
        }
      }

      break;

    case 'Y': //KY

      //CATDEF    KY
      //DESCR     Send Morse/query Morse buffer
      //SET       KYxyyy...yyy;
      //READ      KY;
      //RESP      KYx;
      //NOTE      When setting (sending), x must be a space.
      //CONT      When reading, x=1 indicates buffer space is available, x=0  buffer full
      //NOTE      y: string of up to 24 characters NOT containing ';'.
      //CONT      Trailing blanks are ignored in y, but if it is completely blank it causes an inter-word space.
      //ENDDEF
      if (command[2] == ';') {
        //
        // reply "buffer full" condition if the buffer contains
        // more than (CW_BUF_SIZE-24) characters.
        //
        int avail = cw_buf_in - cw_buf_out;

        if (avail < 0) { avail += CW_BUF_SIZE; }

        if (avail < CW_BUF_SIZE - 24) {
          snprintf(reply, 256, "KY0;");
        } else {
          snprintf(reply, 256, "KY1;");
        }

        send_resp(client->fd, reply);
      } else {
        //
        // Recent versions of Hamlib send CW messages on character at a time.
        // So all trailing blanks have to be removed, and an entirely blank
        // message is interpreted as a inter-word distance.
        // Note we allow variable lengths of incoming messages here, although
        // the standard says they are exactly 24 characters long.
        //
        int j = 3;

        for (unsigned int i = 3; i < strlen(command); i++) {
          if (command[i] == ';') { break; }

          if (command[i] != ' ') { j = i; }
        }

        // j points to the last non-blank character, or to the first blank
        // in an empty string
        for (int i = 3; i <= j; i++) {
          int new = cw_buf_in + 1;

          if (new >= CW_BUF_SIZE) { new = 0; }

          if (new != cw_buf_out) {
            cw_buf[cw_buf_in] = command[i];
            cw_buf_in = new;
          }
        }
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'L':
    switch (command[1]) {
    case 'K': //LK

      //CATDEF    LK
      //DESCR     Set/Read Lock status
      //SET       LKxx;
      //READ      LK;
      //RESP      LKxx;
      //NOTE      When setting, any nonzero x sets lock status.
      //CONT      When reading, x = 00 (not locked) or x = 11 (locked)
      //ENDDEF
      if (command[2] == ';') {
        snprintf(reply, 256, "LK%d%d;", locked, locked);
        send_resp(client->fd, reply);
      } else if (command[4] == ';') {
        locked = atoi(&command[2]);
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    case 'M': //LM
      // set/read keyer recording status
      implemented = FALSE;
      break;

    case 'T': //LT
      // set/read ALT fucntion status
      implemented = FALSE;
      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'M':
    switch (command[1]) {
    case 'C': //MC
      // set/read Memory Channel
      implemented = FALSE;
      break;

    case 'D': //MD

      //CATDEF    MD
      //DESCR     Set/Read VFO-A modes
      //SET       MDx;
      //READ      MD;
      //RESP      MDx;
      //NOTE      Kenwood-type  mode  list:
      //CONT      LSB (x=1), USB (x=2), CWU (x=3), FMN (x=4),
      //CONT      AM (x=5), DIGL (x=6), CWL (x=7), DIGU (x=9)
      //ENDDEF
      if (command[2] == ';') {
        int mode = ts2000_mode(vfo[VFO_A].mode);
        snprintf(reply, 256, "MD%d;", mode);
        send_resp(client->fd, reply);
      } else if (command[3] == ';') {
        int mode = wdspmode(atoi(&command[2]));
        vfo_id_mode_changed(VFO_A, mode);
      }

      break;

    case 'F': //MF
      // set/read Menu
      implemented = FALSE;
      break;

    case 'G': //MG

      //CATDEF    MG
      //DESCR     Set/Read Mic gain (Mic gain slider)
      //SET       MGxxx;
      //READ      MG;
      //RESP      MGxxx;
      //NOTE      x 0-100 mapped to -12 ... +50 dB
      //ENDDEF
      if (can_transmit) {
        if (command[2] == ';') {
          snprintf(reply, 256, "MG%03d;", (int)(((transmitter->mic_gain + 12.0) / 62.0) * 100.0));
          send_resp(client->fd, reply);
        } else if (command[5] == ';') {
          double gain = (double)atoi(&command[2]);
          gain = ((gain / 100.0) * 62.0) - 12.0;

          if (gain < -12.0) { gain = -12.0; }

          if (gain > 50.0) { gain = 50.0; }

#if defined (__LDESK__) && defined (__USELESS__)
          set_mic_gain(gain);
#else
          transmitter->mic_gain = gain;
          tx_set_mic_gain(transmitter);
#endif
        }
      } else {
        implemented = FALSE;
      }

      break;

    case 'L': //ML
      // set/read Monitor Function Level
      implemented = FALSE;
      break;

    case 'O': //MO
      // set/read Monitor Function On/Off
      implemented = FALSE;
      break;

    case 'R': //MR
      // read Memory Channel
      implemented = FALSE;
      break;

    case 'U': //MU
      // set/read Memory Group
      implemented = FALSE;
      break;

    case 'W': //MW
      // Write Memory Channel
      implemented = FALSE;
      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'N':
    switch (command[1]) {
    case 'B': //NB

      //CATDEF    NB
      //DESCR     Set/Read RX1 noise blanker
      //SET       NBx;
      //READ      NB;
      //RESP      NBx;
      //NOTE      x=0: NB off, x=1: NB1 on, x=2: NB2 on
      //ENDDEF
      if (command[2] == ';') {
        snprintf(reply, 256, "NB%d;", receiver[0]->nb);
        send_resp(client->fd, reply);
      } else if (command[3] == ';') {
        receiver[0]->nb = atoi(&command[2]);
        update_noise();
      }

      break;

    case 'L': //NL
      // set/read noise blanker level
      implemented = FALSE;
      break;

    case 'R': //NR

      //CATDEF    NR
      //DESCR     Set/Read RX1 noise reduction
      //SET       NRx;
      //READ      NR;
      //RESP      NRx;
      //NOTE      x=0: NR off, x=1: NR1 on, x=2: NR2 on
      //ENDDEF
      if (command[2] == ';') {
        snprintf(reply, 256, "NR%d;", receiver[0]->nr);
        send_resp(client->fd, reply);
      } else if (command[3] == ';')  {
        receiver[0]->nr = atoi(&command[2]);
        update_noise();
      }

      break;

    case 'T': //NT

      //CATDEF    NT
      //DESCR     Set/Read RX1 auto notch filter
      //SET       NTx;
      //READ      NT;
      //RESP      NTx;
      //NOTE      x=0: Automatic Notch Filter off, x=1: on
      //ENDDEF
      if (command[2] == ';') {
        snprintf(reply, 256, "NT%d;", receiver[0]->anf);
        send_resp(client->fd, reply);
      } else if (command[3] == ';') {
        receiver[0]->anf = atoi(&command[2]);
        update_noise();
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'O':
    switch (command[1]) {
    case 'F': //OF
      // set/read offset frequency
      implemented = FALSE;
      break;

    case 'I': //OI
      // set/read offset frequency
      implemented = FALSE;
      break;

    case 'S': //OS
      // set/read offset function status
      implemented = FALSE;
      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'P':
    switch (command[1]) {
    case 'A': //PA

      //CATDEF    PA
      //DESCR     Set/Read RX1 preamp status
      //SET       PAx;
      //READ      PA;
      //RESP      PAx;
      //NOTE      Applies to RX1
      //NOTE      x=0: RX1 preamp off, x=1: on
      //NOTE      newer HPSDR radios do not have a switchable preamp
      //ENDDEF
      if (command[2] == ';') {
        snprintf(reply, 256, "PA%d0;", receiver[0]->preamp);
        send_resp(client->fd, reply);
      } else if (command[4] == ';') {
        receiver[0]->preamp = command[2] == '1';
      }

      break;

    case 'B': //PB
      // set/read FRU-3A playback status
      implemented = FALSE;
      break;

    case 'C': //PC

      //CATDEF    PC
      //DESCR     Set/Read TX power (Drive slider)
      //SET       PCxxx;
      //READ      PC;
      //RESP      PCxxx;
      //NOTE      x = 0...100
      //ENDDEF
      if (can_transmit) {
        if (command[2] == ';') {
          snprintf(reply, 256, "PC%03d;", (int)transmitter->drive);
          send_resp(client->fd, reply);
        } else if (command[5] == ';') {
          set_drive((double)atoi(&command[2]));
        }
      }

      break;

    case 'I': //PI
      // store in program memory channel
      implemented = FALSE;
      break;

    case 'K': //PK
      // read packet cluster data
      implemented = FALSE;
      break;

    case 'L': //PL

      //CATDEF    PL
      //DESCR     Set/Read TX compressor level
      //SET       PLxxxyyy;
      //READ      PL;
      //RESP      PLxxxyyy;
      //NOTE      x = 0...100, maps to compression 0...20 dB.
      //NOTE      y ignored when setting, y=0 when reading
      //ENDDEF
      if (can_transmit) {
        if (command[2] == ';') {
          snprintf(reply, 256, "PL%03d000;", (int)(5.0 * transmitter->compressor_level));
          send_resp(client->fd, reply);
        } else if (command[8] == ';') {
          command[5] = '\0';
          double level = (double)atoi(&command[2]);
          level = 0.2 * level;
          transmitter->compressor_level = level;
          tx_set_compressor(transmitter);
          g_idle_add(ext_vfo_update, NULL);
        }
      }

      break;

    case 'M': //PM
      // recall program memory
      implemented = FALSE;
      break;

    case 'R': //PR
      // set/read speech processor function
      implemented = FALSE;
      break;

    case 'S': //PS

      //CATDEF    PS
      //DESCR     Set/Read power status
      //SET       PSx;
      //READ      PS;
      //RESP      PSx;
      //NOTE      x = 0: Power on, x=1: off
      //NOTE      When setting, x=0 is ignored and x=1 leads to shutdown
      //NOTE      Reading always reports x=1
      //ENDDEF
      if (command[2] == ';') {
        snprintf(reply, 256, "PS1;");
        send_resp(client->fd, reply);
      } else if (command[3] == ';') {
        int pwrc = atoi(&command[2]);

        if ( pwrc == 0 ) {
          schedule_action(SHUTDOWN, PRESSED, 0);
        } else {
          // power-on command. Should there be a reply?
          // snprintf(reply, 256, "PS1;");
        }
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'Q':
    switch (command[1]) {
    case 'C': //QC
      // set/read DCS code
      implemented = FALSE;
      break;

    case 'I': //QI
      // store in quick memory
      implemented = FALSE;
      break;

    case 'R': //QR
      // set/read quick memory channel data
      implemented = FALSE;
      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'R':
    switch (command[1]) {
    case 'A': //RA

      //CATDEF    RA
      //DESCR     Set/Read RX1 attenuator or RX1 gain
      //SET       RAxx;
      //READ      RA;
      //RESP      RAxxyy;
      //NOTE      x = 0 ... 99 is mapped to the attenuation range available.
      //NOTE      HPSDR radios: attenuator range 0...31 dB,
      //CONT      HermesLite-II etc.: gain range -12...48 dB.
      //CONT      y is always zero.
      //ENDDEF
      // set/read Attenuator function
      if (command[2] == ';') {
        int att = 0;

        if (have_rx_gain) {
          // map gain value -12...48 to 0...99
          att = (int)(adc[receiver[0]->adc].attenuation + 12);
          att = (int)(((double)att / 60.0) * 99.0);
        }

        if (have_rx_att) {
          // map att value -31 ... 0 to 0...99
          att = (int)(adc[receiver[0]->adc].attenuation);
          att = (int)(((double)att / 31.0) * 99.0);
        }

        snprintf(reply, 256, "RA%02d00;", att);
        send_resp(client->fd, reply);
      } else if (command[4] == ';') {
        int att = atoi(&command[2]);

        if (have_rx_gain) {
          // map 0...99 scale to -12...48
          att = (int)((((double)att / 99.0) * 60.0) - 12.0);
          set_rf_gain(VFO_A, (double)att);
        }

        if (have_rx_att) {
          // mapp 0...99 scale to 0...31
          att = (int)(((double)att / 99.0) * 31.0);
          set_attenuation_value((double)att);
        }
      }

      break;

    case 'C': //RC

      //CATDEF    RC
      //DESCR     Clear VFO-A RIT value
      //SET       RC;
      //ENDDEF
      if (command[2] == ';') {
        vfo[VFO_A].rit = 0;
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    case 'D': //RD

      //CATDEF    RD
      //DESCR     Set or Decrement VFO-A RIT value
      //SET       RDxxxxx;
      //NOTE      When x is not given (RD;)  decrement by 10 Hz (CW modes) or 50 Hz (other modes).
      //CONT      When x is given, set VFO-A rit value to the negative of x.
      //ENDDEF
      if (command[2] == ';') {
        if (vfo[VFO_A].mode == modeCWL || vfo[VFO_A].mode == modeCWU) {
          vfo[VFO_A].rit -= 10;
        } else {
          vfo[VFO_A].rit -= 50;
        }

        g_idle_add(ext_vfo_update, NULL);
      } else if (command[7] == ';') {
        vfo[VFO_A].rit = -atoi(&command[2]);
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    case 'G': //RG
      // set/read RF gain status
      implemented = FALSE;
      break;

    case 'L': //RL
      // set/read noise reduction level
      implemented = FALSE;
      break;

    case 'M': //RM
      // set/read meter function
      implemented = FALSE;
      break;

    case 'T': //RT

      //CATDEF    RT
      //DESCR     Read/Set VFO-A RIT status
      //SET       RTx;
      //READ      RT;
      //RESP      RTx;
      //NOTE      x=0: VFO-A RIT off, x=1: on
      //ENDDEF
      if (command[2] == ';') {
        snprintf(reply, 256, "RT%d;", vfo[VFO_A].rit_enabled);
        send_resp(client->fd, reply);
      } else if (command[3] == ';') {
        vfo[VFO_A].rit_enabled = atoi(&command[2]);
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    case 'U': //RU

      //CATDEF    RU
      //DESCR     Set or Increment VFO-A RIT value
      //SET       RUxxxxx;
      //NOTE      When x is not given (RU;)  increment by 10 Hz (CW modes) or 50 Hz (other modes).
      //CONT      When x is given, set VFO-A rit value to x
      //ENDDEF
      if (command[2] == ';') {
        if (vfo[VFO_A].mode == modeCWL || vfo[VFO_A].mode == modeCWU) {
          vfo[VFO_A].rit += 10;
        } else {
          vfo[VFO_A].rit += 50;
        }

        g_idle_add(ext_vfo_update, NULL);
      } else if (command[7] == ';') {
        vfo[VFO_A].rit = atoi(&command[2]);
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    case 'X': //RX

      //CATDEF    RX
      //DESCR     Enter RX mode
      //SET       RX;
      //ENDDEF
      if (command[2] == ';') {
        radio_mox_update(0);
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'S':
    switch (command[1]) {
    case 'A': //SA

      //CATDEF    SA
      //DESCR     Set/Read SAT mode
      //SET       SAxyzsbcdeeeeeeee;
      //READ      SA;
      //RESP      SAxyzsbcdeeeeeeee;
      //NOTE      x=0: neither SAT nor RSAT, x=1: SAT or RSAT
      //NOTE      y,z,s always zero
      //NOTE      c = 1 indicates SAT mode (TRACE)
      //NOTE      d = 1 indicates RSAT mode (TRACE REV)
      //NOTE      e = eight-character label, here "SAT     "
      //NOTE      when setting, c=1 and/or d=1 is illegal, and s is ignored.
      //ENDDEF
      if (command[2] == ';') {
        snprintf(reply, 256, "SA%d%d%d%d%d%d%dSAT     ;", (sat_mode == SAT_MODE) || (sat_mode == RSAT_MODE), 0, 0, 0,
                 sat_mode == SAT_MODE, sat_mode == RSAT_MODE, 0);
        send_resp(client->fd, reply);
      } else if (command[9] == ';') {
        if (command[2] == '0') {
          radio_set_satmode(SAT_NONE);
        } else if (command[2] == '1') {
          if (command[6] == '0' && command[7] == '0') {
            radio_set_satmode(SAT_NONE);
          } else if (command[6] == '1' && command[7] == '0') {
            radio_set_satmode(SAT_MODE);
          } else if (command[6] == '0' && command[7] == '1') {
            radio_set_satmode(RSAT_MODE);
          } else {
            implemented = FALSE;
          }
        }
      } else {
        implemented = FALSE;
      }

      break;

    case 'B': //SB
      // set/read SUB,TF-W status
      implemented = FALSE;
      break;

    case 'C': //SC
      // set/read SCAN function status
      implemented = FALSE;
      break;

    case 'D': //SD

      //CATDEF    SD
      //DESCR     Set/Read CW break-in hang time
      //SET       SDxxxx;
      //READ      SD;
      //RESP      SDxxxx;
      //NOTE      x = 0...1000 (in milli seconds).
      //CONT      When setting, x = 0  disables break-in
      //ENDDEF
      if (command[2] == ';') {
        snprintf(reply, 256, "SD%04d;", (int)fmin(cw_keyer_hang_time, 1000));
        send_resp(client->fd, reply);
      } else if (command[6] == ';') {
        int b = fmin(atoi(&command[2]), 1000);
        cw_breakin = (b == 0);
        cw_keyer_hang_time = b;
      } else {
        implemented = FALSE;
      }

      break;

    case 'H': //SH

      //CATDEF    SH
      //DESCR     Set/Read VFO-A filter high-water (LSB, USB, DIGL, DIGU only)
      //SET       SHxx;
      //READ      SH;
      //RESP      SHxx;
      //NOTE      When setting, the Var1 filter is activated
      //NOTE      x = 0...11 encodes filter high water mark in Hz:
      //NOTE      1400 (x=0), 1600 (x=1), 1800 (x=2), 2000 (x=3)
      //CONT      2200 (x=4), 2400 (x=5), 2600 (x=6), 2800 (x=7)
      //CONT      3000 (x=8), 3400 (x=9), 4000 (x=10), 5000 (x=11).
      //ENDDEF
      if (command[2] == ';') {
        FILTER *mode_filters = filters[vfo[VFO_A].mode];
        const FILTER *filter = &mode_filters[vfo[VFO_A].filter];
        int fh, high = 0;

        switch (vfo[VFO_A].mode) {
        case modeLSB:
        case modeDIGL:
          high = abs(filter->low);
          break;

        case modeUSB:
        case modeDIGU:
          high = filter->high;
          break;

        default:
          implemented = FALSE;
          break;
        }

        if (high <= 1400) {
          fh = 0;
        } else if (high <= 1600) {
          fh = 1;
        } else if (high <= 1800) {
          fh = 2;
        } else if (high <= 2000) {
          fh = 3;
        } else if (high <= 2200) {
          fh = 4;
        } else if (high <= 2400) {
          fh = 5;
        } else if (high <= 2600) {
          fh = 6;
        } else if (high <= 2800) {
          fh = 7;
        } else if (high <= 3000) {
          fh = 8;
        } else if (high <= 3400) {
          fh = 9;
        } else if (high <= 4000) {
          fh = 10;
        } else {
          fh = 11;
        }

        if (implemented) {
          snprintf(reply, 256, "SH%02d;", fh);
          send_resp(client->fd, reply) ;
        }
      } else if (command[4] == ';') {
        // make sure filter is filterVar1
        if (vfo[VFO_A].filter != filterVar1) {
          vfo_id_filter_changed(VFO_A, filterVar1);
        }

        FILTER *mode_filters = filters[vfo[VFO_A].mode];
        FILTER *filter = &mode_filters[filterVar1];
        int i = atoi(&command[2]);
        int fh;

        switch (i) {
        case 0:
          fh = 1400;
          break;

        case 1:
          fh = 1600;
          break;

        case 2:
          fh = 1800;
          break;

        case 3:
          fh = 2000;
          break;

        case 4:
          fh = 2200;
          break;

        case 5:
          fh = 2400;
          break;

        case 6:
          fh = 2600;
          break;

        case 7:
          fh = 2800;
          break;

        case 8:
          fh = 3000;
          break;

        case 9:
          fh = 3400;
          break;

        case 10:
          fh = 4000;
          break;

        case 11:
          fh = 5000;
          break;

        default:
          fh = 100;
          break;
        }

        switch (vfo[VFO_A].mode) {
        case modeUSB:
        case modeDIGU:
          filter->high = fh;
          break;

        case modeLSB:
        case modeDIGL:
          filter->low = -fh;
          break;

        default:
          implemented = FALSE;
        }

        vfo_id_filter_changed(VFO_A, filterVar1);
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    case 'I': //SI
      // enter satellite memory name
      implemented = FALSE;
      break;

    case 'L': //SL

      //CATDEF    SL
      //DESCR     Set/Read VFO-A filter low-water (LSB, USB, DIGL, DIGU only)
      //SET       SLxx;
      //READ      SL;
      //RESP      SLxx;
      //NOTE      When setting, the Var1 filter is activated
      //NOTE      x = 0...11 encodes filter low water mark in Hz:
      //NOTE      10 (x=0), 50 (x=1), 100 (x=2), 200 (x=3)
      //CONT      300 (x=4), 400 (x=5), 500 (x=6), 600 (x=7)
      //CONT      700 (x=8), 800 (x=9), 900 (x=10), 1000 (x=11).
      //ENDDEF
      if (command[2] == ';') {
        FILTER *mode_filters = filters[vfo[VFO_A].mode];
        const FILTER *filter = &mode_filters[vfo[VFO_A].filter];
        int fl = 2;
        int low = filter->low;

        if (vfo[VFO_A].mode == modeLSB || vfo[VFO_A].mode == modeDIGL) {
          low = abs(filter->high);
        }

        if (low <= 10) {
          fl = 0;
        } else if (low <= 50) {
          fl = 1;
        } else if (low <= 100) {
          fl = 2;
        } else if (low <= 200) {
          fl = 3;
        } else if (low <= 300) {
          fl = 4;
        } else if (low <= 400) {
          fl = 5;
        } else if (low <= 500) {
          fl = 6;
        } else if (low <= 600) {
          fl = 7;
        } else if (low <= 700) {
          fl = 8;
        } else if (low <= 800) {
          fl = 9;
        } else if (low <= 900) {
          fl = 10;
        } else {
          fl = 11;
        }

        snprintf(reply, 256, "SL%02d;", fl);
        send_resp(client->fd, reply) ;
      } else if (command[4] == ';') {
        // make sure filter is filterVar1
        if (vfo[VFO_A].filter != filterVar1) {
          vfo_id_filter_changed(VFO_A, filterVar1);
        }

        FILTER *mode_filters = filters[vfo[VFO_A].mode];
        FILTER *filter = &mode_filters[filterVar1];
        int i = atoi(&command[2]);
        int fl = 100;

        switch (i) {
        case 0:
          fl = 10;
          break;

        case 1:
          fl = 50;
          break;

        case 2:
          fl = 100;
          break;

        case 3:
          fl = 200;
          break;

        case 4:
          fl = 300;
          break;

        case 5:
          fl = 400;
          break;

        case 6:
          fl = 500;
          break;

        case 7:
          fl = 600;
          break;

        case 8:
          fl = 700;
          break;

        case 9:
          fl = 800;
          break;

        case 10:
          fl = 900;
          break;

        case 11:
          fl = 1000;
          break;

        default:
          fl = 100;
          break;
        }

        switch (vfo[VFO_A].mode) {
        case modeLSB:
        case modeDIGL:
          filter->high = -fl;
          break;

        case modeUSB:
        case modeDIGU:
          filter->low = fl;
          break;
        }

        vfo_id_filter_changed(VFO_A, filterVar1);
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    case 'M': //SM

      //CATDEF    SM
      //DESCR     Read S-meter
      //READ      SMx;
      //RESP      SMxyyyy;
      //NOTE      x=0: read RX1, x=1: read RX2
      //NOTE      y : 0 ... 30 mapped to -127...-19 dBm
      //ENDDEF
      if (command[3] == ';') {
        int id = atoi(&command[2]);
        RXCHECK (id,
                 int val = (int)((receiver[id]->meter + 127.0) * 0.277778);

        if (val > 30) { val = 30; }
      if (val < 0 ) { val = 0; }
      snprintf(reply, 256, "SM%d%04d;", id, val);
      send_resp(client->fd, reply);
              )
      }

      break;

    case 'Q': //SQ

      //CATDEF    SQ
      //DESCR     Set/Read squelch level (Squelch slider)
      //SET       SQxyyy;
      //READ      SQx;
      //RESP      SQxyyy
      //NOTE      x=0: read/set RX1 squelch, x=1: RX2.
      //NOTE      y : 0-255 mapped to 0-100
      //ENDDEF
      if (command[3] == ';') {
        int id = atoi(&command[2]);
        RXCHECK(id,
                snprintf(reply, 256, "SQ%d%03d;", id, (int)((double)receiver[id]->squelch / 100.0 * 255.0 + 0.5));
                send_resp(client->fd, reply);
               )
      } else if (command[6] == ';') {
        int id = atoi(&command[2]);
        int p2 = atoi(&command[3]);
        RXCHECK(id,
                receiver[id]->squelch = (int)((double)p2 / 255.0 * 100.0 + 0.5);
                set_squelch(receiver[id]);
               )
      }

      break;

    case 'R': //SR
      // reset transceiver
      implemented = FALSE;
      break;

    case 'S': //SS
      // set/read program scan pause frequency
      implemented = FALSE;
      break;

    case 'T': //ST
      // set/read MULTI/CH channel frequency steps
      implemented = FALSE;
      break;

    case 'U': //SU
      // set/read program scan pause frequency
      implemented = FALSE;
      break;

    case 'V': //SV
      // execute memory transfer function
      implemented = FALSE;
      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'T':
    switch (command[1]) {
    case 'C': //TC
      // set/read internal TNC mode
      implemented = FALSE;
      break;

    case 'D': //TD
      // send DTMF memory channel data
      implemented = FALSE;
      break;

    case 'I': //TI
      // read TNC LED status
      implemented = FALSE;
      break;

    case 'N': //TN
      // set/read sub-tone frequency
      implemented = FALSE;
      break;

    case 'O': //TO
      // set/read TONE function
      implemented = FALSE;
      break;

    case 'S': //TS
      // set/read TF-SET function
      implemented = FALSE;
      break;

    case 'X': //TX

      //CATDEF    TX
      //DESCR     Enter TX mode
      //SET       TX;
      //ENDDEF
      // set transceiver to TX mode
      if (command[2] == ';') {
        radio_mox_update(1);
      }

      break;

    case 'Y': //TY

      //CATDEF    TY
      //DESCR     Read firmware version
      //READ      TY;
      //RESP      TYxxx;
      //NOTE      x is always zero
      //ENDDEF
      if (command[2] == ';') {
        send_resp(client->fd, "TY000;");
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'U':
    switch (command[1]) {
    case 'L': //UL
      // detects the PLL unlock status
      implemented = FALSE;
      break;

    case 'P': //UP

      //CATDEF    UP
      //DESCR     Move VFO-A one step up
      //SET       UP;
      //NOTE      use current VFO-A step size
      //ENDDEF
      if (command[2] == ';') {
        vfo_id_step(VFO_A, 1);
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'V':
    switch (command[1]) {
    case 'D': //VD
      // set/read VOX delay time
      implemented = FALSE;
      break;

    case 'G': //VG

      //CATDEF    VG
      //DESCR     Set/Read VOX threshold
      //SET       VGxxx;
      //READ      VG;
      //RESP      VGxxx;
      //NOTE      x is in the range 0-9, mapped to an amplitude
      //CONT      threshold 0.0-1.0
      //ENDDEF
      if (command[2] == ';') {
        snprintf(reply, 256, "VG%03d;", (int)((vox_threshold * 100.0) * 0.9));
        send_resp(client->fd, reply);
      } else if (command[5] == ';') {
        vox_threshold = atof(&command[2]) / 9.0;
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    case 'R': //VR
      // emulate VOICE1 or VOICE2 key
      implemented = FALSE;
      break;

    case 'X': //VX

      //CATDEF    VX
      //DESCR     Set/Read VOX status
      //SET       VXx;
      //READ      VX;
      //RESP      VXx;
      //NOTE      x=0: VOX disabled, x=1: enabled
      //ENDDEF
      if (command[2] == ';') {
        snprintf(reply, 256, "VX%d;", vox_enabled);
        send_resp(client->fd, reply);
      } else if (command[3] == ';') {
        vox_enabled = atoi(&command[2]);
        g_idle_add(ext_vfo_update, NULL);
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'W':
    switch (command[1]) {
    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'X':
    switch (command[1]) {
    case 'T': //XT

      //CATDEF    XT
      //DESCR     Set/Read XIT status
      //SET       XTx;
      //READ      XT;
      //RESP      XTx;
      //NOTE      x=0: XIT disabled, x=1: enabled
      //ENDDEF
      if (can_transmit) {
        if (command[2] == ';') {
          snprintf(reply, 256, "XT%d;", vfo[vfo_get_tx_vfo()].xit_enabled);
          send_resp(client->fd, reply);
        } else if (command[3] == ';') {
          vfo_xit_onoff(SET(atoi(&command[2])));
        }
      }

      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'Y':
    switch (command[1]) {
    default:
      implemented = FALSE;
      break;
    }

    break;

  case 'Z':
    switch (command[1]) {
    case 'Z':
      implemented = parse_extended_cmd (command, client);
      break;

    default:
      implemented = FALSE;
      break;
    }

    break;

  default:
    implemented = FALSE;
    break;
  }

  if (!implemented) {
    if (rigctl_debug) { t_print("RIGCTL: UNIMPLEMENTED COMMAND: %s\n", info->command); }

    send_resp(client->fd, "?;");
  }

  client->done = 1; // possibly inform server that command is finished
  g_free(info->command);
  g_free(info);
  return 0;
}

// Serial Port Launch
int set_interface_attribs (int fd, int speed, int parity) {
  struct termios tty;
  memset (&tty, 0, sizeof tty);

  if (tcgetattr (fd, &tty) != 0) {
    t_perror ("RIGCTL (tcgetattr):");
    return -1;
  }

  cfsetospeed (&tty, speed);
  cfsetispeed (&tty, speed);
  tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8;     // 8-bit chars
  // disable IGNBRK for mismatched speed tests; otherwise receive break
  // as \000 chars
  tty.c_iflag &= ~IGNBRK;         // disable break processing
  tty.c_lflag = 0;                // no signaling chars, no echo,
  // no canonical processing
  tty.c_oflag = 0;                // no remapping, no delays
  tty.c_cc[VMIN]  = 0;            // read doesn't block
  tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout
  //tty.c_iflag &= ~(IXON | IXOFF | IXANY); // shut off xon/xoff ctrl
  tty.c_iflag |= (IXON | IXOFF | IXANY); // shut off xon/xoff ctrl
  tty.c_cflag |= (CLOCAL | CREAD);// ignore modem controls,
  // enable reading
  tty.c_cflag &= ~(PARENB | PARODD);      // shut off parity
  tty.c_cflag |= parity;
  tty.c_cflag &= ~CSTOPB;
  tty.c_cflag &= ~CRTSCTS;

  if (tcsetattr (fd, TCSANOW, &tty) != 0) {
    t_perror( "RIGCTL (tcsetattr):");
    return -1;
  }

  return 0;
}

void set_blocking (int fd, int should_block) {
  struct termios tty;
  memset (&tty, 0, sizeof tty);
  int flags = fcntl(fd, F_GETFL, 0);

  if (should_block) {
    flags &= ~O_NONBLOCK;
  } else {
    flags |= O_NONBLOCK;
  }

  fcntl(fd, F_SETFL, flags);

  if (tcgetattr (fd, &tty) != 0) {
    t_perror ("RIGCTL (tggetattr):");
    return;
  }

  tty.c_cc[VMIN]  = SET(should_block);
  tty.c_cc[VTIME] = 5;            // 0.5 seconds read timeout

  if (tcsetattr (fd, TCSANOW, &tty) != 0) {
    t_perror("RIGCTL (tcsetattr):");
  }
}

static gpointer serial_server(gpointer data) {
  // We're going to Read the Serial port and
  // when we get data we'll send it to parse_cmd
  CLIENT *client = (CLIENT *)data;
  char cmd_input[MAXDATASIZE];
  char *command = g_new(char, MAXDATASIZE);
  int command_index = 0;
  int i;
  fd_set fds;
  struct timeval tv;
  t_print("%s: Entering Thread\n", __FUNCTION__);
  g_mutex_lock(&mutex_numcat);
  cat_control++;
  // if (rigctl_debug) { t_print("RIGCTL: SER INC cat_control=%d\n", cat_control); }
  g_mutex_unlock(&mutex_numcat);
  g_idle_add(ext_vfo_update, NULL);
  client->running = TRUE;

  while (client->running) {
    //
    // If the "serial line" is a FIFO, we must not drain it
    // by reading our own responses (they must go to the other
    // side). Therefore, wait until 50msec after the last
    // CAT command of this client has been processed.
    // If for some reason this does not happen, resume after
    // waiting for about 500 msec.
    // Check client->running after the "pause" and after returning
    // from "read".
    //
    while (client->fifo && client->busy > 0) {
      if (client->done) {
        // command done, possibly response sent:
        // wait 50 msec then resume listening
        usleep(50000L);
        break;
      }

      usleep(50000L);
      client->busy--;
    }

    client->busy = 0;
    client->done = 0;

    if (!client->running) { break; }

    //
    // Blocking I/O with a time-out
    //
    FD_ZERO(&fds);
    FD_SET(client->fd, &fds);
    tv.tv_usec = 250000; // 250 msec
    tv.tv_sec = 0;

    if (select(client->fd + 1, &fds, NULL, NULL, &tv) <= 0) {
      continue;
    }

    int numbytes = read (client->fd, cmd_input, sizeof cmd_input);

    //
    // On my MacOS using a FIFO, I have seen that numbytes can be -1
    // (with errno = EAGAIN) although the select() inidcated that data
    // is available. Therefore the serial thread is not shut down if
    // the read() failed -- it will try again and again until it is
    // shut down by the rigctl menu.
    if (!client->running) { break; }

    if (numbytes > 0) {
      for (i = 0; i < numbytes; i++) {
        //
        // Filter out newlines and other non-printable characters
        // These may occur when doing CAT manually with a terminal program
        //
        if (cmd_input[i] < 32) {
          continue;
        }

        command[command_index] = cmd_input[i];
        command_index++;

        if (cmd_input[i] == ';') {
          command[command_index] = '\0';

          if (rigctl_debug) { t_print("RIGCTL: serial command=%s\n", command); }

          COMMAND *info = g_new(COMMAND, 1);
          info->client = client;
          info->command = command;
          client->busy = 10;
          g_idle_add(parse_cmd, info);
          command = g_new(char, MAXDATASIZE);
          command_index = 0;
        }
      }
    }
  }

  // Release the last "command" buffer (that has not yet been used)
  g_free(command);
  g_mutex_lock(&mutex_numcat);
  cat_control--;
  // if (rigctl_debug) { t_print("RIGCTL: SER DEC - cat_control=%d\n", cat_control); }
  g_mutex_unlock(&mutex_numcat);
  g_idle_add(ext_vfo_update, NULL);
  t_print("%s: Exiting Thread, running=%d\n", __FUNCTION__, client->running);
  return NULL;
}

int launch_serial_rigctl (int id) {
  int fd;
  int baud;
  t_print("%s:  Port %s\n", __FUNCTION__, SerialPorts[id].port);
  //
  // Use O_NONBLOCK to prevent "hanging" upon open(), set blocking mode
  // later.
  //
  fd = open (SerialPorts[id].port, O_RDWR | O_NOCTTY | O_SYNC | O_NONBLOCK);

  if (fd < 0) {
    t_perror("RIGCTL (open serial):");
    return 0 ;
  }

  t_print("%s: serial port fd=%d\n", __FUNCTION__, fd);
  serial_client[id].fd = fd;
  // hard-wired parity = NONE
  baud = SerialPorts[id].baud;

  //
  // ANDROMEDA uses a hard-wired baud rate 9600
  //
  if (SerialPorts[id].andromeda && !SerialPorts[id].g2) {
    baud = B9600;
  }

  t_print("%s: Baud rate=%d\n", __FUNCTION__, baud);
  serial_client[id].fifo = 0;

  if (set_interface_attribs (fd, baud, 0) == 0) {
    set_blocking (fd, 1);                   // set blocking
  } else {
    //
    // This tells the server that fd is something else
    // than a serial line (most likely a FIFO), but it
    // can still be used.
    //
    t_print("%s: serial port is probably a FIFO\n", __FUNCTION__);
    serial_client[id].fifo = 1;
  }

  //
  // Initialize the rest of the CLIENT data structure
  //
  serial_client[id].busy = 0;
  serial_client[id].done = 0;
  serial_client[id].running = 1;
  serial_client[id].andromeda_timer = 0;
  serial_client[id].auto_reporting = SET(SerialPorts[id].autoreporting);
  serial_client[id].andromeda_type = 0;
  serial_client[id].last_fa = 0;
  serial_client[id].last_fb = 0;

  for (int i = 0; i < MAX_ANDROMEDA_LEDS; i++) {
    serial_client[id].last_led[i] = -1;
  }

  //
  // Spawn off server thread
  //
  serial_client[id].thread_id = g_thread_new( "Serial server", serial_server, (gpointer)&serial_client[id]);
  //
  // Launch auto-reporter task
  //
  serial_client[id].auto_timer = g_timeout_add(750, autoreport_handler, &serial_client[id]);

  //
  // If this is a serial line to an ANDROMEDA controller, initialize it and start a periodic GTK task
  //
  if (SerialPorts[id].andromeda) {
    //
    // For Arduino UNO and the like, opening the serial line executes a hardware
    // reset and then the device stays in bootloader mode for half a second or so.
    //
    usleep(700000L);
    // Note this will send a ZZZS; command upon first invocation
    serial_client[id].andromeda_timer = g_timeout_add(500, andromeda_handler, &serial_client[id]);
  }

  return 1;
}

// Serial Port close
void disable_serial_rigctl (int id) {
  t_print("%s: Close Serial Port %s\n", __FUNCTION__, SerialPorts[id].port);

  if (serial_client[id].andromeda_timer != 0) {
    g_source_remove(serial_client[id].andromeda_timer);
    serial_client[id].andromeda_timer = 0;
  }

  if (serial_client[id].auto_timer != 0) {
    g_source_remove(serial_client[id].auto_timer);
    serial_client[id].auto_timer = 0;
  }

  serial_client[id].running = FALSE;

  if (serial_client[id].fifo) {
    //
    // If the "serial port" is a fifo then the serial thread
    // may hang in a blocking read().
    // Fortunately, we can set the thread free
    // by sending something to the FIFO
    //
    write (serial_client[id].fd, "ID;", 3);
  }

  // wait for the serial server actually terminating
  if (serial_client[id].thread_id) {
    g_thread_join(serial_client[id].thread_id);
    serial_client[id].thread_id = NULL;
  }

  if (serial_client[id].fd >= 0) {
    close(serial_client[id].fd);
    serial_client[id].fd = -1;
  }
}

void launch_tcp_rigctl () {
  t_print( "---- LAUNCHING RIGCTL SERVER ----\n");
  tcp_running = 1;

  //
  // Start CW thread and auto reporter, if not yet done
  //
  if (!rigctl_cw_thread_id) {
    rigctl_cw_thread_id = g_thread_new("RIGCTL cw", rigctl_cw_thread, NULL);
  }

  //
  // Start TCP thread
  //
  rigctl_server_thread_id = g_thread_new( "rigctl server", rigctl_server, GINT_TO_POINTER(rigctl_tcp_port));
}
