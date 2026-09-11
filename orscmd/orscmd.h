/*
		TWRP is free software: you can redistribute it and/or modify
		it under the terms of the GNU General Public License as published by
		the Free Software Foundation, either version 3 of the License, or
		(at your option) any later version.

		TWRP is distributed in the hope that it will be useful,
		but WITHOUT ANY WARRANTY; without even the implied warranty of
		MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
		GNU General Public License for more details.

		You should have received a copy of the GNU General Public License
		along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef __ORSCMD_H
#define __ORSCMD_H

#define ORS_INPUT_FILE "/system/bin/orsin"
#define ORS_OUTPUT_FILE "/system/bin/orsout"

// Modern "fox" RPC channel. Parallel to the legacy ORS FIFOs so the old "twrp"
// tool keeps working unchanged. The request is one UTF-8 JSON object:
// {"v":1,"id":"optional","op":"status","args":{}}. The response is
// newline-delimited JSON events and always ends with a final "result" event.
#define AERA_INPUT_FILE "/system/bin/foxin"
#define AERA_OUTPUT_FILE "/system/bin/foxout"

// Out-of-band cancel channel for cancellable operations (e.g. backup). The GUI
// event loop watches this FIFO at all times -- even while a fox command runs --
// so a client can request cancellation mid-operation by writing any byte to it.
#define AERA_CANCEL_FILE "/system/bin/foxcancel"

// Continuous screen-frame stream. Started/stopped over foxin, but frame data is
// written here so normal foxin/foxout commands (notably input) remain usable.
#define AERA_SCREEN_STREAM_FILE "/system/bin/foxscreenout"

#endif //__ORSCMD_H
