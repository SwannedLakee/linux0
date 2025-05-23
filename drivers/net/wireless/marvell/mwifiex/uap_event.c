// SPDX-License-Identifier: GPL-2.0-only
/*
 * NXP Wireless LAN device driver: AP event handling
 *
 * Copyright 2011-2020 NXP
 */

#include "decl.h"
#include "main.h"
#include "11n.h"

#define MWIFIEX_BSS_START_EVT_FIX_SIZE    12

static int mwifiex_check_uap_capabilities(struct mwifiex_private *priv,
					  struct sk_buff *event)
{
	int evt_len;
	u8 *curr;
	u16 tlv_len;
	struct mwifiex_ie_types_data *tlv_hdr;
	struct ieee_types_wmm_parameter *wmm_param_ie = NULL;
	int mask = IEEE80211_WMM_IE_AP_QOSINFO_PARAM_SET_CNT_MASK;

	priv->wmm_enabled = false;
	skb_pull(event, MWIFIEX_BSS_START_EVT_FIX_SIZE);
	evt_len = event->len;
	curr = event->data;

	mwifiex_dbg_dump(priv->adapter, EVT_D, "uap capabilities:",
			 event->data, event->len);

	skb_push(event, MWIFIEX_BSS_START_EVT_FIX_SIZE);

	while ((evt_len >= sizeof(tlv_hdr->header))) {
		tlv_hdr = (struct mwifiex_ie_types_data *)curr;
		tlv_len = le16_to_cpu(tlv_hdr->header.len);

		if (evt_len < tlv_len + sizeof(tlv_hdr->header))
			break;

		switch (le16_to_cpu(tlv_hdr->header.type)) {
		case WLAN_EID_HT_CAPABILITY:
			priv->ap_11n_enabled = true;
			break;

		case WLAN_EID_VHT_CAPABILITY:
			priv->ap_11ac_enabled = true;
			break;

		case WLAN_EID_VENDOR_SPECIFIC:
			/* Point the regular IEEE IE 2 bytes into the Marvell IE
			 * and setup the IEEE IE type and length byte fields
			 */
			wmm_param_ie = (void *)(curr + 2);
			wmm_param_ie->vend_hdr.len = (u8)tlv_len;
			wmm_param_ie->vend_hdr.element_id =
						WLAN_EID_VENDOR_SPECIFIC;
			mwifiex_dbg(priv->adapter, EVENT,
				    "info: check uap capabilities:\t"
				    "wmm parameter set count: %d\n",
				    wmm_param_ie->qos_info_bitmap & mask);

			mwifiex_wmm_setup_ac_downgrade(priv);
			priv->wmm_enabled = true;
			mwifiex_wmm_setup_queue_priorities(priv, wmm_param_ie);
			break;

		default:
			break;
		}

		curr += (tlv_len + sizeof(tlv_hdr->header));
		evt_len -= (tlv_len + sizeof(tlv_hdr->header));
	}

	return 0;
}

/*
 * This function handles AP interface specific events generated by firmware.
 *
 * Event specific routines are called by this function based
 * upon the generated event cause.
 *
 *
 * Events supported for AP -
 *      - EVENT_UAP_STA_ASSOC
 *      - EVENT_UAP_STA_DEAUTH
 *      - EVENT_UAP_BSS_ACTIVE
 *      - EVENT_UAP_BSS_START
 *      - EVENT_UAP_BSS_IDLE
 *      - EVENT_UAP_MIC_COUNTERMEASURES:
 */
int mwifiex_process_uap_event(struct mwifiex_private *priv)
{
	struct mwifiex_adapter *adapter = priv->adapter;
	int len, i;
	u32 eventcause = adapter->event_cause;
	struct station_info *sinfo;
	struct mwifiex_assoc_event *event;
	struct mwifiex_sta_node *node;
	u8 *deauth_mac;
	struct host_cmd_ds_11n_batimeout *ba_timeout;
	u16 ctrl;

	switch (eventcause) {
	case EVENT_UAP_STA_ASSOC:
		sinfo = kzalloc(sizeof(*sinfo), GFP_KERNEL);
		if (!sinfo)
			return -ENOMEM;

		event = (struct mwifiex_assoc_event *)
			(adapter->event_body + MWIFIEX_UAP_EVENT_EXTRA_HEADER);
		if (le16_to_cpu(event->type) == TLV_TYPE_UAP_MGMT_FRAME) {
			len = -1;

			if (ieee80211_is_assoc_req(event->frame_control))
				len = 0;
			else if (ieee80211_is_reassoc_req(event->frame_control))
				/* There will be ETH_ALEN bytes of
				 * current_ap_addr before the re-assoc ies.
				 */
				len = ETH_ALEN;

			if (len != -1) {
				sinfo->assoc_req_ies = &event->data[len];
				len = (u8 *)sinfo->assoc_req_ies -
				      (u8 *)&event->frame_control;
				sinfo->assoc_req_ies_len =
					le16_to_cpu(event->len) - (u16)len;
			}
		}
		cfg80211_new_sta(priv->netdev, event->sta_addr, sinfo,
				 GFP_KERNEL);

		node = mwifiex_add_sta_entry(priv, event->sta_addr);
		if (!node) {
			mwifiex_dbg(adapter, ERROR,
				    "could not create station entry!\n");
			kfree(sinfo);
			return -1;
		}

		if (!priv->ap_11n_enabled) {
			kfree(sinfo);
			break;
		}

		mwifiex_set_sta_ht_cap(priv, sinfo->assoc_req_ies,
				       sinfo->assoc_req_ies_len, node);

		for (i = 0; i < MAX_NUM_TID; i++) {
			if (node->is_11n_enabled)
				node->ampdu_sta[i] =
					      priv->aggr_prio_tbl[i].ampdu_user;
			else
				node->ampdu_sta[i] = BA_STREAM_NOT_ALLOWED;
		}
		memset(node->rx_seq, 0xff, sizeof(node->rx_seq));
		kfree(sinfo);
		break;
	case EVENT_UAP_STA_DEAUTH:
		deauth_mac = adapter->event_body +
			     MWIFIEX_UAP_EVENT_EXTRA_HEADER;
		cfg80211_del_sta(priv->netdev, deauth_mac, GFP_KERNEL);

		if (priv->ap_11n_enabled) {
			mwifiex_11n_del_rx_reorder_tbl_by_ta(priv, deauth_mac);
			mwifiex_del_tx_ba_stream_tbl_by_ra(priv, deauth_mac);
		}
		mwifiex_wmm_del_peer_ra_list(priv, deauth_mac);
		mwifiex_del_sta_entry(priv, deauth_mac);
		break;
	case EVENT_UAP_BSS_IDLE:
		priv->media_connected = false;
		priv->port_open = false;
		mwifiex_clean_txrx(priv);
		mwifiex_del_all_sta_list(priv);
		break;
	case EVENT_UAP_BSS_ACTIVE:
		priv->media_connected = true;
		priv->port_open = true;
		break;
	case EVENT_UAP_BSS_START:
		mwifiex_dbg(adapter, EVENT,
			    "AP EVENT: event id: %#x\n", eventcause);
		priv->port_open = false;
		eth_hw_addr_set(priv->netdev, adapter->event_body + 2);
		if (priv->hist_data)
			mwifiex_hist_data_reset(priv);
		mwifiex_check_uap_capabilities(priv, adapter->event_skb);
		break;
	case EVENT_UAP_MIC_COUNTERMEASURES:
		/* For future development */
		mwifiex_dbg(adapter, EVENT,
			    "AP EVENT: event id: %#x\n", eventcause);
		break;
	case EVENT_AMSDU_AGGR_CTRL:
		ctrl = get_unaligned_le16(adapter->event_body);
		mwifiex_dbg(adapter, EVENT,
			    "event: AMSDU_AGGR_CTRL %d\n", ctrl);

		if (priv->media_connected) {
			adapter->tx_buf_size =
				min_t(u16, adapter->curr_tx_buf_size, ctrl);
			mwifiex_dbg(adapter, EVENT,
				    "event: tx_buf_size %d\n",
				    adapter->tx_buf_size);
		}
		break;
	case EVENT_ADDBA:
		mwifiex_dbg(adapter, EVENT, "event: ADDBA Request\n");
		if (priv->media_connected)
			mwifiex_send_cmd(priv, HostCmd_CMD_11N_ADDBA_RSP,
					 HostCmd_ACT_GEN_SET, 0,
					 adapter->event_body, false);
		break;
	case EVENT_DELBA:
		mwifiex_dbg(adapter, EVENT, "event: DELBA Request\n");
		if (priv->media_connected)
			mwifiex_11n_delete_ba_stream(priv, adapter->event_body);
		break;
	case EVENT_BA_STREAM_TIEMOUT:
		mwifiex_dbg(adapter, EVENT, "event:  BA Stream timeout\n");
		if (priv->media_connected) {
			ba_timeout = (void *)adapter->event_body;
			mwifiex_11n_ba_stream_timeout(priv, ba_timeout);
		}
		break;
	case EVENT_EXT_SCAN_REPORT:
		mwifiex_dbg(adapter, EVENT, "event: EXT_SCAN Report\n");
		if (adapter->ext_scan)
			return mwifiex_handle_event_ext_scan_report(priv,
						adapter->event_skb->data);
		break;
	case EVENT_TX_STATUS_REPORT:
		mwifiex_dbg(adapter, EVENT, "event: TX_STATUS Report\n");
		mwifiex_parse_tx_status_event(priv, adapter->event_body);
		break;
	case EVENT_PS_SLEEP:
		mwifiex_dbg(adapter, EVENT, "info: EVENT: SLEEP\n");

		adapter->ps_state = PS_STATE_PRE_SLEEP;

		mwifiex_check_ps_cond(adapter);
		break;

	case EVENT_PS_AWAKE:
		mwifiex_dbg(adapter, EVENT, "info: EVENT: AWAKE\n");
		if (!adapter->pps_uapsd_mode &&
		    priv->media_connected && adapter->sleep_period.period) {
				adapter->pps_uapsd_mode = true;
				mwifiex_dbg(adapter, EVENT,
					    "event: PPS/UAPSD mode activated\n");
		}
		adapter->tx_lock_flag = false;
		if (adapter->pps_uapsd_mode && adapter->gen_null_pkt) {
			if (mwifiex_check_last_packet_indication(priv)) {
				if (adapter->data_sent ||
				    (adapter->if_ops.is_port_ready &&
				     !adapter->if_ops.is_port_ready(priv))) {
					adapter->ps_state = PS_STATE_AWAKE;
					adapter->pm_wakeup_card_req = false;
					adapter->pm_wakeup_fw_try = false;
					break;
				}
				if (!mwifiex_send_null_packet
					(priv,
					 MWIFIEX_TxPD_POWER_MGMT_NULL_PACKET |
					 MWIFIEX_TxPD_POWER_MGMT_LAST_PACKET))
						adapter->ps_state =
							PS_STATE_SLEEP;
					return 0;
			}
		}
		adapter->ps_state = PS_STATE_AWAKE;
		adapter->pm_wakeup_card_req = false;
		adapter->pm_wakeup_fw_try = false;
		break;

	case EVENT_CHANNEL_REPORT_RDY:
		mwifiex_dbg(adapter, EVENT, "event: Channel Report\n");
		mwifiex_11h_handle_chanrpt_ready(priv, adapter->event_skb);
		break;
	case EVENT_RADAR_DETECTED:
		mwifiex_dbg(adapter, EVENT, "event: Radar detected\n");
		mwifiex_11h_handle_radar_detected(priv, adapter->event_skb);
		break;
	case EVENT_BT_COEX_WLAN_PARA_CHANGE:
		mwifiex_dbg(adapter, EVENT, "event: BT coex wlan param update\n");
		mwifiex_bt_coex_wlan_param_update_event(priv,
							adapter->event_skb);
		break;
	case EVENT_TX_DATA_PAUSE:
		mwifiex_dbg(adapter, EVENT, "event: TX DATA PAUSE\n");
		mwifiex_process_tx_pause_event(priv, adapter->event_skb);
		break;

	case EVENT_MULTI_CHAN_INFO:
		mwifiex_dbg(adapter, EVENT, "event: multi-chan info\n");
		mwifiex_process_multi_chan_event(priv, adapter->event_skb);
		break;
	case EVENT_RXBA_SYNC:
		dev_dbg(adapter->dev, "EVENT: RXBA_SYNC\n");
		mwifiex_11n_rxba_sync_event(priv, adapter->event_body,
					    adapter->event_skb->len -
					    sizeof(eventcause));
		break;

	case EVENT_REMAIN_ON_CHAN_EXPIRED:
		mwifiex_dbg(adapter, EVENT,
			    "event: uap: Remain on channel expired\n");
		cfg80211_remain_on_channel_expired(&priv->wdev,
						   priv->roc_cfg.cookie,
						   &priv->roc_cfg.chan,
						   GFP_ATOMIC);
		memset(&priv->roc_cfg, 0x00, sizeof(struct mwifiex_roc_cfg));
		break;

	default:
		mwifiex_dbg(adapter, EVENT,
			    "event: unknown event id: %#x\n", eventcause);
		break;
	}

	return 0;
}
