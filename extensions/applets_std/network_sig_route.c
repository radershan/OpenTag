/*  Copyright 2017, JP Norair
  *
  * Licensed under the OpenTag License, Version 1.0 (the "License");
  * you may not use this file except in compliance with the License.
  * You may obtain a copy of the License at
  *
  * http://www.indigresso.com/wiki/doku.php?id=opentag:license_1_0
  *
  * Unless required by applicable law or agreed to in writing, software
  * distributed under the License is distributed on an "AS IS" BASIS,
  * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  * See the License for the specific language governing permissions and
  * limitations under the License.
  */
/**
  * @file       /otlibext/applets_std/network_sig_route.c
  * @author     JP Norair
  * @version    R102
  * @date       9 May 2017
  * @brief      Standard Network Routing Callback Routine
  *
  * Network layer uses a callback when a packet has been successfully received
  * and it is routed.  This routine will log the type of packet/frame that has
  * been received & routed, and its contents.
  */

#include <otstd.h>
#include <m2api.h>
#include <otlib/logger.h>
#include <otlib/buffers.h>

#ifdef EXTF_network_sig_route
void network_sig_route(void* route, void* session) {
    static const char* label_dialog = "M2_Dialog";
    static const char* label_nack   = "M2_Nack";
    static const char* label_stream = "M2_Stream";
    static const char* label_snack  = "M2_SNack";
    static const ot_u8 label_len[]  = { 10, 8, 10, 9 };
    
    const char* label[] = { label_dialog, label_nack, label_stream, label_snack };
    
	ot_u8 protocol;

	protocol = ((m2session*)session)->extra & 3;
	logger_msg(	MSG_raw,
					label_len[protocol],
					q_length(&rxq),
					(ot_u8*)label[protocol],
					rxq.front	);
}
#endif
