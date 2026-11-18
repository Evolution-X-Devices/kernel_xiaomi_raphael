/*
 * See wma_frame_inject.h for the approach and why it's shaped this way.
 * Vdev creation lives in wlan_hdd_main.c now (hdd_inject_get_tx_vdev_id());
 * this file only builds and sends the WMI mgmt-TX command.
 */
#include "wma.h"
#include "wma_api.h"
#include "cds_api.h"
#include "wmi_unified_api.h"
#include "wmi_unified.h"
#include "qdf_nbuf.h"
#include "qdf_types.h"
#include "qdf_mem.h"
#include "../inc/wma_frame_inject.h"
#include "wlan_objmgr_vdev_obj.h"
#include "wlan_objmgr_peer_obj.h"
#include "wlan_mgmt_txrx_utils_api.h"

QDF_STATUS wma_inject_frame_via_vdev(void *wma_context, uint8_t tx_vdev_id,
				      uint8_t *frame, uint32_t frame_len)
{
	tp_wma_handle wma_handle = (tp_wma_handle)wma_context;
	struct wmi_mgmt_params mgmt_param;
	qdf_nbuf_t wmi_buf;
	uint8_t *frame_data;
	struct wlan_objmgr_vdev *vdev;
	struct wlan_objmgr_peer *self_peer;
	QDF_STATUS status;

	if (!wma_handle || !frame || !frame_len) {
		WMA_LOGE("wma_inject: invalid parameters");
		return QDF_STATUS_E_INVAL;
	}
	if (tx_vdev_id >= wma_handle->max_bssid ||
	    !wma_handle->interfaces[tx_vdev_id].vdev_active) {
		WMA_LOGE("wma_inject: tx vdev %u not active", tx_vdev_id);
		return QDF_STATUS_E_INVAL;
	}

	/*
	 * Send through the driver's own mgmt-txrx path (wlan_mgmt_txrx_mgmt_
	 * frame_tx) rather than calling wmi_mgmt_unified_cmd_send() directly.
	 * That matters for a subtle reason: the WMI mgmt-TX command DMA-maps the
	 * frame nbuf and hands it to firmware, and the nbuf is only unmapped and
	 * freed later, from the mgmt-TX *completion* handler, keyed by the
	 * descriptor id echoed back by firmware. Sending directly left every
	 * injected frame with desc_id 0 and no registered descriptor, so its
	 * completion could never be matched ("Mgmt desc empty for id 0" in
	 * dmesg) and the mapped nbuf leaked -- after a few thousand injected
	 * frames the mgmt-TX descriptor pool exhausts and ALL mgmt TX (deauth/
	 * assoc) silently stalls until a reboot. Going through mgmt-txrx
	 * allocates a real descriptor, so the completion frees the nbuf and
	 * releases the descriptor; with NULL completion callbacks the handler
	 * simply frees the nbuf. See notes/03-injection.md.
	 *
	 * The path needs an objmgr vdev + its self-peer (the completion handler
	 * unconditionally releases a peer ref, so a valid peer is required); the
	 * hidden injection adapter is a real STA vdev, so it has a self-peer.
	 */
	vdev = wlan_objmgr_get_vdev_by_id_from_psoc(wma_handle->psoc, tx_vdev_id,
						    WLAN_LEGACY_WMA_ID);
	if (!vdev) {
		WMA_LOGE("wma_inject: no objmgr vdev for vdev %u", tx_vdev_id);
		return QDF_STATUS_E_INVAL;
	}
	self_peer = wlan_vdev_get_selfpeer(vdev);
	if (!self_peer) {
		WMA_LOGE("wma_inject: no self peer for vdev %u", tx_vdev_id);
		wlan_objmgr_vdev_release_ref(vdev, WLAN_LEGACY_WMA_ID);
		return QDF_STATUS_E_INVAL;
	}

	wmi_buf = qdf_nbuf_alloc(NULL, frame_len, 0, 0, false);
	if (!wmi_buf) {
		WMA_LOGE("wma_inject: qdf_nbuf_alloc failed");
		wlan_objmgr_vdev_release_ref(vdev, WLAN_LEGACY_WMA_ID);
		return QDF_STATUS_E_NOMEM;
	}
	frame_data = qdf_nbuf_put_tail(wmi_buf, frame_len);
	if (!frame_data) {
		qdf_nbuf_free(wmi_buf);
		wlan_objmgr_vdev_release_ref(vdev, WLAN_LEGACY_WMA_ID);
		return QDF_STATUS_E_NOMEM;
	}
	qdf_mem_copy(frame_data, frame, frame_len);

	qdf_mem_zero(&mgmt_param, sizeof(mgmt_param));
	mgmt_param.tx_frame = wmi_buf;
	mgmt_param.frm_len = frame_len;
	mgmt_param.vdev_id = tx_vdev_id;
	mgmt_param.chanfreq = 0; /* match how real probe requests are sent */
	mgmt_param.pdata = frame_data;
	mgmt_param.qdf_ctx = cds_get_context(QDF_MODULE_ID_QDF_DEVICE);
	/*
	 * The monitor's radiotap rate is discarded before this WMI command. Ask
	 * firmware for a concrete legacy rate instead of letting it pick its
	 * management default: 54 Mbps OFDM, one spatial stream, 20 MHz, data.
	 * This is the highest legacy rate the management-TX path understands;
	 * HT/MCS is not available here.
	 */
	mgmt_param.tx_params_valid = true;
	mgmt_param.use_6mbps = 0;
	mgmt_param.tx_param.mcs_mask = 1 << 11; /* OFDM 64-QAM 3/4: 54 Mbps */
	mgmt_param.tx_param.nss_mask = 1;       /* Nss=1 */
	mgmt_param.tx_param.retry_limit = 4;
	mgmt_param.tx_param.bw_mask = 1 << 2;   /* 20 MHz */
	mgmt_param.tx_param.preamble_type = 1 << 0; /* OFDM */
	mgmt_param.tx_param.frame_type = 1;     /* data */

	/* NULL download/OTA completion callbacks => the completion handler just
	 * frees the nbuf. mgmt-txrx takes its own peer ref for the lifetime of
	 * the frame, so our vdev ref can be dropped as soon as this returns.
	 */
	status = wlan_mgmt_txrx_mgmt_frame_tx(self_peer, NULL, wmi_buf,
					      NULL, NULL,
					      WLAN_UMAC_COMP_MGMT_TXRX,
					      &mgmt_param);
	WMA_LOGD("wma_inject: sent frame len=%u via vdev=%u, status=%d",
		 frame_len, tx_vdev_id, status);
	if (QDF_IS_STATUS_ERROR(status)) {
		WMA_LOGE("wma_inject: mgmt_frame_tx failed: %d", status);
		/* On failure mgmt-txrx/WMI leave the frame nbuf for the caller
		 * (it is unmapped but not freed there) -- free it here.
		 */
		qdf_nbuf_free(wmi_buf);
	}
	wlan_objmgr_vdev_release_ref(vdev, WLAN_LEGACY_WMA_ID);
	return status;
}
