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
 * @file call.c
 * @author Ivan Alonso [aka Kaian] <kaian@irontec.com>
 *
 * @brief Functions to manage SIP call data
 *
 * This file contains the functions and structure to manage SIP call data
 *
 */
#include "config.h"
#include <glib.h>
#include "glib-extra/glib.h"
#include "call.h"
#include "packet/packet_sip.h"
#include "setting.h"
#include "message.h"

Call *
call_create(const gchar *callid, const gchar *xcallid)
{
    // Initialize a new call structure
    Call *call = g_malloc0(sizeof(Call));

    // Create a vector to store call messages
    call->msgs = g_ptr_array_new_with_free_func((GDestroyNotify) msg_free);

    // Create an empty vector to store stream data
    call->streams = g_ptr_array_new_with_free_func((GDestroyNotify) stream_free);

    // Create an empty vector to store x-calls
    call->xcalls = g_ptr_array_new();

    // Initialize call filter status
    call->filtered = -1;

    // Set message callid
    call->callid = callid;
    call->xcallid = xcallid;

    return call;
}

void
call_destroy(gpointer item)
{
    Call *call = item;
    // Remove all call messages
    g_ptr_array_free(call->msgs, TRUE);
    // Remove all call streams
    g_ptr_array_free(call->streams, TRUE);
    // Remove all xcalls
    g_ptr_array_free(call->xcalls, TRUE);

    // Deallocate call memory
    g_free(call->reasontxt);
    g_free(call);
}

void
call_add_message(Call *call, Message *msg)
{
    // Set the message owner
    msg->call = call;
    // Put this msg at the end of the msg list
    g_ptr_array_add(call->msgs, msg);
    // Flag this call as changed
    call->changed = TRUE;
}

void
call_add_stream(Call *call, Stream *stream)
{
    // Store stream
    g_ptr_array_add(call->streams, stream);
    // Flag this call as changed
    call->changed = TRUE;
}

guint
call_msg_count(const Call *call)
{
    return g_ptr_array_len(call->msgs);
}

enum CallState
call_state(Call *call)
{
    return call->state;
}

int
call_is_invite(Call *call)
{
    Message *first = g_ptr_array_first(call->msgs);
    g_return_val_if_fail(first != NULL, 0);

    return msg_get_method(first) == SIP_METHOD_INVITE;
}

void
call_update_state(Call *call, Message *msg)
{
    if (!call_is_invite(call))
        return;

    // Get current message Method / Response Code
    guint msg_reqresp = msg_get_method(msg);
    guint64 msg_cseq = msg_get_cseq(msg);

    // If this message is actually a call, get its current state
    if (call->state) {
        if (call->state == CALL_STATE_CALLSETUP) {
            if (msg_reqresp == SIP_METHOD_ACK && call->invitecseq == msg_cseq) {
                // Alice and Bob are talking
                call->state = CALL_STATE_INCALL;
                call->cstart_msg = msg;
            } else if (msg_reqresp == SIP_METHOD_CANCEL) {
                // Alice is not in the mood
                call->state = CALL_STATE_CANCELLED;
            } else if ((msg_reqresp == 480) || (msg_reqresp == 486) || (msg_reqresp == 600)) {
                // Bob is busy
                call->state = CALL_STATE_BUSY;
            } else if (msg_reqresp > 400 && call->invitecseq == msg_cseq) {
                // Bob is not in the mood
                call->state = CALL_STATE_REJECTED;
            } else if (msg_reqresp > 300) {
                // Bob has diversion
                call->state = CALL_STATE_DIVERTED;
            }
        } else if (call->state == CALL_STATE_INCALL) {
            if (msg_reqresp == SIP_METHOD_BYE) {
                // Thanks for all the fish!
                call->state = CALL_STATE_COMPLETED;
                call->cend_msg = msg;
            }
        } else if (msg_reqresp == SIP_METHOD_INVITE && call->state != CALL_STATE_INCALL) {
            // Call is being setup (after proper authentication)
            call->invitecseq = msg_cseq;
            call->state = CALL_STATE_CALLSETUP;
        }
    } else {
        // This is actually a call
        if (msg_reqresp == SIP_METHOD_INVITE) {
            call->invitecseq = msg_cseq;
            call->state = CALL_STATE_CALLSETUP;
        }
    }
}

const gchar *
call_state_to_str(enum CallState state)
{
    switch (state) {
        case CALL_STATE_CALLSETUP:
            return "CALL SETUP";
        case CALL_STATE_INCALL:
            return "IN CALL";
        case CALL_STATE_CANCELLED:
            return "CANCELLED";
        case CALL_STATE_REJECTED:
            return "REJECTED";
        case CALL_STATE_BUSY:
            return "BUSY";
        case CALL_STATE_DIVERTED:
            return "DIVERTED";
        case CALL_STATE_COMPLETED:
            return "COMPLETED";
    }
    return "";
}

gint
call_attr_compare(const Call *one, const Call *two, Attribute *attr)
{
    const gchar *onevalue = NULL, *twovalue = NULL;
    int oneintvalue = 0, twointvalue = 0;
    int comparetype; /* TODO 0 = string compare, 1 = int compare */
    Message *msg_one = g_ptr_array_first(one->msgs);
    Message *msg_two = g_ptr_array_first(two->msgs);

    if (g_strcmp0(attr->name, ATTR_CALLINDEX) == 0) {
        oneintvalue = one->index;
        twointvalue = two->index;
        comparetype = 1;
    } else if (g_strcmp0(attr->name, ATTR_MSGCNT) == 0) {
        oneintvalue = call_msg_count(one);
        twointvalue = call_msg_count(two);
        comparetype = 1;
    } else {
        // Get attribute values
        onevalue = msg_get_attribute(msg_one, attr);
        twovalue = msg_get_attribute(msg_two, attr);
        comparetype = 0;
    }

    switch (comparetype) {
        case 0:
            if (twovalue == NULL && onevalue == NULL)
                return 0;
            if (twovalue == NULL)
                return 1;
            if (onevalue == NULL)
                return -1;
            return strcmp(onevalue, twovalue);
        case 1:
            if (oneintvalue == twointvalue) return 0;
            if (oneintvalue > twointvalue) return 1;
            if (oneintvalue < twointvalue) return -1;
            /* fall-thru */
        default:
            return 0;
    }
}

void
call_add_xcall(Call *call, Call *xcall)
{
    if (!call || !xcall)
        return;

    // Mark this call as changed
    call->changed = TRUE;

    // Add the xcall to the list
    g_ptr_array_add(call->xcalls, xcall);
}

Stream *
call_find_stream(Call *call, const Address src, const Address dst, guint32 ssrc)
{
    Stream *stream = NULL;

    // Create an iterator for call streams
    for (guint i = 0; i < g_ptr_array_len(call->streams); i++) {
        stream = g_ptr_array_index(call->streams, i);
        if (addressport_equals(src, stream->src) &&
            addressport_equals(dst, stream->dst)
            && ssrc == stream->ssrc) {
            return stream;
        }
    }

    // Nothing found
    return NULL;
}
