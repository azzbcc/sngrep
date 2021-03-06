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
 * @file packet_rtcp.c
 * @author Ivan Alonso [aka Kaian] <kaian@irontec.com>
 *
 * @brief Source of functions defined in packet_rtcp.h
 */

#include "config.h"
#include <glib.h>
#include "glib-extra/glib.h"
#include "storage/storage.h"
#include "packet.h"
#include "packet_ip.h"
#include "packet_udp.h"
#include "packet_rtcp.h"

G_DEFINE_TYPE(PacketDissectorRtcp, packet_dissector_rtcp, PACKET_TYPE_DISSECTOR)

// Version is the first 2 bits of the first octet
#define RTP_VERSION(octet) ((octet) >> 6)

// Payload type is the last 7 bits
#define RTP_PAYLOAD_TYPE(octet) (guint8)((octet) & 0x7F)

// Handled RTP versions
#define RTP_VERSION_RFC1889 2

// RTCP common header length
#define RTCP_HDR_LENGTH 4

/**
 * @brief Check if the data is a RTCP packet
 * RFC 5761 Section 4.  Distinguishable RTP and RTCP Packets
 * RFC 5764 Section 5.1.2.  Reception (packet demultiplexing)
 */
static gboolean
packet_rtcp_valid(GBytes *data)
{
    g_return_val_if_fail(data != NULL, FALSE);
    struct rtcp_hdr_generic *hdr = (struct rtcp_hdr_generic *) g_bytes_get_data(data, NULL);
    guint8 *content = (guint8*) g_bytes_get_data(data, NULL);

    if ((g_bytes_get_size(data) >= RTCP_HDR_LENGTH) &&
        (RTP_VERSION(content[0]) == RTP_VERSION_RFC1889) &&
        (content[0] > 127 && content[0] < 192) &&
        (hdr->type >= 192 && hdr->type <= 223)) {
        return 0;
    }

    // Not a RTCP packet
    return FALSE;
}

static GBytes *
packet_dissector_rtcp_parse(G_GNUC_UNUSED PacketDissector *self, Packet *packet, GBytes *data)
{
    struct rtcp_hdr_generic hdr;
    struct rtcp_hdr_sr hdr_sr;
    struct rtcp_hdr_xr hdr_xr;
    struct rtcp_blk_xr blk_xr;
    struct rtcp_blk_xr_voip blk_xr_voip;

    // Not RTCP Packet
    if (!packet_rtcp_valid(data)) {
        return data;
    }

    // Allocate RTCP packet data
    PacketRtcpData *rtcp = g_malloc0(sizeof(PacketRtcpData));
    rtcp->proto.id = PACKET_PROTO_RTCP;

    // Parse all packet payload headers
    while (g_bytes_get_size(data) > 0) {

        // Check we have at least rtcp generic info
        if (g_bytes_get_size(data) < sizeof(struct rtcp_hdr_generic))
            break;

        // Copy into RTCP generic header
        memcpy(&hdr, g_bytes_get_data(data, NULL), sizeof(hdr));

        // Check RTP version
        if (RTP_VERSION(hdr.version) != RTP_VERSION_RFC1889)
            break;

        // Header length
        guint hlen = (guint) g_ntohs(hdr.len) * 4 + 4;

        // No enough data for this RTCP header
        if (hlen > g_bytes_get_size(data))
            break;

        // Check RTCP packet header type
        switch (hdr.type) {
            case RTCP_HDR_SR:
                // Get Sender Report header
                memcpy(&hdr_sr, g_bytes_get_data(data, NULL), sizeof(hdr_sr));
                rtcp->spc = ntohl(hdr_sr.spc);
                break;
            case RTCP_HDR_RR:
            case RTCP_HDR_SDES:
            case RTCP_HDR_BYE:
            case RTCP_HDR_APP:
            case RTCP_RTPFB:
            case RTCP_PSFB:
                break;
            case RTCP_XR:
                // Get Sender Report Extended header
                memcpy(&hdr_xr, g_bytes_get_data(data, NULL), sizeof(hdr_xr));
                gsize bsize = sizeof(hdr_xr);

                // Read all report blocks
                while (bsize < (guint) ntohs(hdr_xr.len) * 4 + 4) {
                    // Read block header
                    guint8 *content = (guint8 *) g_bytes_get_data(data, NULL);
                    memcpy(&blk_xr, content + bsize, sizeof(blk_xr));
                    // Check block type
                    switch (blk_xr.type) {
                        case RTCP_XR_VOIP_METRCS:
                            memcpy(&blk_xr_voip, content + sizeof(hdr_xr), sizeof(blk_xr_voip));
                            rtcp->fdiscard = blk_xr_voip.drate;
                            rtcp->flost = blk_xr_voip.lrate;
                            rtcp->mosl = blk_xr_voip.moslq;
                            rtcp->mosc = blk_xr_voip.moscq;
                            break;
                        default:
                            break;
                    }
                    bsize += ntohs(blk_xr.len) * 4 + 4;
                }
                break;
            case RTCP_AVB:
            case RTCP_RSI:
            case RTCP_TOKEN:
            default:
                // Not handled headers. Skip the rest of this packet
                data = g_bytes_offset(data, g_bytes_get_size(data));
                break;
        }

        // Remove this header data
        data = g_bytes_offset(data, hlen);
    }

    // Set packet RTP informaiton
    packet_set_protocol_data(packet, PACKET_PROTO_RTCP, rtcp);

    // Add data to storage
    storage_add_packet(packet);

    return NULL;
}

static void
packet_dissector_rtcp_class_init(PacketDissectorRtcpClass *klass)
{
    PacketDissectorClass *dissector_class = PACKET_DISSECTOR_CLASS(klass);
    dissector_class->dissect = packet_dissector_rtcp_parse;
}

static void
packet_dissector_rtcp_init(G_GNUC_UNUSED PacketDissectorRtcp *self)
{
}

PacketDissector *
packet_dissector_rtcp_new()
{
    return g_object_new(
        PACKET_DISSECTOR_TYPE_RTCP,
        "id", PACKET_PROTO_RTCP,
        "name", "RTCP",
        NULL
    );
}
