/*
 * Minimal WiFi frame injection support for monitor-mode interfaces.
 *
 * QCACLD-3.0 firmware silently drops management-frame TX from a monitor-type
 * vdev. The only vdev types firmware accepts mgmt TX from are AP and STA.
 *
 * The obvious fix -- hand-roll a second vdev via raw WMI calls
 * (vdev_create/vdev_start/peer_create) -- crashes the firmware/host: those
 * calls alone skip a huge amount of state the real vdev lifecycle sets up
 * (CDP datapath attach, objmgr vdev object, PE/SME session, and more found
 * by reading wma_vdev_attach() in wma_dev_if.c and hdd_vdev_create() in
 * wlan_hdd_main.c). Rather than keep reimplementing that piece by piece,
 * the hidden vdev is created via the driver's own proven adapter-creation
 * path (hdd_open_adapter() + hdd_start_adapter(), the same functions that
 * create wlan1/p2p0 at boot) -- see hdd_inject_get_tx_vdev_id() in
 * wlan_hdd_main.c. This file's only job is sending a frame given an
 * already-valid vdev_id, through the same WMI mgmt-TX path real management
 * frames already use (wmi_mgmt_unified_cmd_send).
 */
#ifndef __WMA_FRAME_INJECT_H
#define __WMA_FRAME_INJECT_H

#include "qdf_status.h"

/*
 * Deliberately does NOT include "wma.h" here: that header (and what it pulls
 * in, e.g. cds_ieee80211_common.h) conflicts with net/ieee80211_radiotap.h's
 * IEEE80211_CHAN_* macros, which callers outside wma/ (namely
 * wlan_hdd_main.c) already have included. Callers pass the WMA handle as an
 * opaque void*; wma_frame_inject.c itself includes wma.h and casts it to
 * tp_wma_handle internally, where that conflict doesn't arise.
 */

/**
 * wma_inject_frame_via_vdev() - send a raw 802.11 frame through the WMI
 * mgmt-TX path, tagged with an already-valid (non-monitor-type) vdev
 * @wma_handle: WMA handle (opaque here; see note above)
 * @tx_vdev_id: vdev id to send as -- must be a real, already-started,
 *              non-monitor-type vdev (see wlan_hdd_main.c for how one gets
 *              created for this purpose)
 * @frame: raw 802.11 frame bytes (starting at the frame control field, no
 *         radiotap header)
 * @frame_len: length of frame in bytes
 *
 * Return: QDF_STATUS_SUCCESS if the frame was handed to firmware (does not
 * guarantee it was actually transmitted over the air).
 */
QDF_STATUS wma_inject_frame_via_vdev(void *wma_handle, uint8_t tx_vdev_id,
				      uint8_t *frame, uint32_t frame_len);

#endif /* __WMA_FRAME_INJECT_H */
