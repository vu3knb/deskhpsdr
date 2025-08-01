/* Copyright (C)
* 2015 - John Melton, G0ORX/N6LYT
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

#ifndef _OLD_PROTOCOL_H
#define _OLD_PROTOCOL_H

extern void old_protocol_stop(void);
extern void old_protocol_run(void);

extern void old_protocol_init(int rate);
extern void old_protocol_set_mic_sample_rate(int rate);

extern void old_protocol_audio_samples(short left_audio_sample, short right_audio_sample);
extern void old_protocol_iq_samples(int isample, int qsample, int side);
#ifdef __APPLE__
  extern void old_protocol_update_timing(void);
#endif
#endif
