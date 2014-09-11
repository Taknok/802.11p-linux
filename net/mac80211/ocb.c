/*
 * OCB mode implementation
 *
 * Copyright: (c) 2014 Czech Technical University in Prague
 *            (c) 2014 Volkswagen Group Research
 * Author:    Rostislav Lisovy <rostislav.lisovy@fel.cvut.cz>
 * Funded by: Volkswagen Group Research
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/delay.h>
#include <linux/if_ether.h>
#include <linux/skbuff.h>
#include <linux/if_arp.h>
#include <linux/etherdevice.h>
#include <linux/rtnetlink.h>
#include <net/mac80211.h>
#include <asm/unaligned.h>

#include "ieee80211_i.h"
#include "driver-ops.h"
#include "rate.h"

#define IEEE80211_OCB_HOUSEKEEPING_INTERVAL		(60 * HZ)
#define IEEE80211_OCB_PEER_INACTIVITY_LIMIT		(240 * HZ)
#define IEEE80211_OCB_MAX_STA_ENTRIES			128

enum ocb_deferred_task_flags {
	OCB_WORK_HOUSEKEEPING,
};

void ieee80211_ocb_rx_no_sta(struct ieee80211_sub_if_data *sdata,
			     const u8 *bssid, const u8 *addr,
			     u32 supp_rates)
{
	struct ieee80211_if_ocb *ifocb = &sdata->u.ocb;
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_chanctx_conf *chanctx_conf;
	struct ieee80211_supported_band *sband;
	enum nl80211_bss_scan_width scan_width;
	struct sta_info *sta;
	int band;

	/* XXX: Consider removing the least recently used entry and
	 *      allow new one to be added.
	 */
	if (local->num_sta >= IEEE80211_OCB_MAX_STA_ENTRIES) {
		net_info_ratelimited("%s: No room for a new OCB STA entry %pM\n",
				     sdata->name, addr);
		return;
	}

	ocb_dbg(sdata, "Adding new OCB station %pM\n", addr);

	rcu_read_lock();
	chanctx_conf = rcu_dereference(sdata->vif.chanctx_conf);
	if (WARN_ON_ONCE(!chanctx_conf)) {
		rcu_read_unlock();
		return;
	}
	band = chanctx_conf->def.chan->band;
	scan_width = cfg80211_chandef_to_scan_width(&chanctx_conf->def);
	rcu_read_unlock();

	sta = sta_info_alloc(sdata, addr, GFP_KERNEL);
	if (!sta)
		return;

	sta->last_rx = jiffies;

	/* Add only mandatory rates for now */
	sband = local->hw.wiphy->bands[band];
	sta->sta.supp_rates[band] = ieee80211_mandatory_rates(sband, scan_width);
	/* sta->sta.supp_rates[band] |= supp_rates; */

	spin_lock(&ifocb->incomplete_lock);
	list_add(&sta->list, &ifocb->incomplete_stations);
	spin_unlock(&ifocb->incomplete_lock);
	ieee80211_queue_work(&local->hw, &sdata->work);
}

/* TODO: Almost the same as ieee80211_ibss_finish_sta()
 *       Maybe use the same function for both?
 */
static struct sta_info *ieee80211_ocb_finish_sta(struct sta_info *sta)
	__acquires(RCU)
{
	struct ieee80211_sub_if_data *sdata = sta->sdata;
	u8 addr[ETH_ALEN];

	ether_addr_copy(addr, sta->sta.addr);

	ocb_dbg(sdata, "Adding new IBSS station %pM (dev=%s)\n",
		addr, sdata->name);

	sta_info_move_state(sta, IEEE80211_STA_AUTH);
	sta_info_move_state(sta, IEEE80211_STA_ASSOC);
	sta_info_move_state(sta, IEEE80211_STA_AUTHORIZED);

	rate_control_rate_init(sta);

	/* If it fails, maybe we raced another insertion? */
	if (sta_info_insert_rcu(sta))
		return sta_info_get(sdata, addr);
	return sta;
}

static void ieee80211_ocb_housekeeping(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_if_ocb *ifocb = &sdata->u.ocb;

	ocb_dbg(sdata, "Running ocb housekeeping\n");

	ieee80211_sta_expire(sdata, IEEE80211_OCB_PEER_INACTIVITY_LIMIT);

	mod_timer(&ifocb->housekeeping_timer,
		  round_jiffies(jiffies + IEEE80211_OCB_HOUSEKEEPING_INTERVAL));
}

void ieee80211_ocb_work(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_if_ocb *ifocb = &sdata->u.ocb;
	struct sta_info *sta;

	sdata_lock(sdata);

	spin_lock_bh(&ifocb->incomplete_lock);
	while (!list_empty(&ifocb->incomplete_stations)) {
		sta = list_first_entry(&ifocb->incomplete_stations,
				       struct sta_info, list);
		list_del(&sta->list);
		spin_unlock_bh(&ifocb->incomplete_lock);

		ieee80211_ocb_finish_sta(sta);
		rcu_read_unlock();
		spin_lock_bh(&ifocb->incomplete_lock);
	}
	spin_unlock_bh(&ifocb->incomplete_lock);

	if (test_and_clear_bit(OCB_WORK_HOUSEKEEPING, &ifocb->wrkq_flags))
		ieee80211_ocb_housekeeping(sdata);

	sdata_unlock(sdata);
}

static void ieee80211_ocb_housekeeping_timer(unsigned long data)
{
	struct ieee80211_sub_if_data *sdata = (void *)data;
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_ocb *ifocb = &sdata->u.ocb;

	set_bit(OCB_WORK_HOUSEKEEPING, &ifocb->wrkq_flags);

	ieee80211_queue_work(&local->hw, &sdata->work);
}

void ieee80211_ocb_setup_sdata(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_if_ocb *ifocb = &sdata->u.ocb;

	setup_timer(&ifocb->housekeeping_timer,
		    ieee80211_ocb_housekeeping_timer,
		    (unsigned long)sdata);
	INIT_LIST_HEAD(&ifocb->incomplete_stations);
	spin_lock_init(&ifocb->incomplete_lock);
}

int ieee80211_ocb_join(struct ieee80211_sub_if_data *sdata,
		       struct ocb_setup *setup)
{
	struct ieee80211_local *local = sdata->local;
	struct ieee80211_if_ocb *ifocb = &sdata->u.ocb;
	u32 changed = BSS_CHANGED_OCB;
	int err;

	sdata->flags |= IEEE80211_SDATA_OPERATING_GMODE;
	sdata->smps_mode = IEEE80211_SMPS_OFF;
	sdata->needed_rx_chains = sdata->local->rx_chains;

	mutex_lock(&sdata->local->mtx);
	err = ieee80211_vif_use_channel(sdata, &setup->chandef,
					IEEE80211_CHANCTX_EXCLUSIVE);
	mutex_unlock(&sdata->local->mtx);
	if (err)
		return err;

	ieee80211_bss_info_change_notify(sdata, changed);

	set_bit(OCB_WORK_HOUSEKEEPING, &ifocb->wrkq_flags);
	ieee80211_queue_work(&local->hw, &sdata->work);

	netif_carrier_on(sdata->dev);
	return 0;
}

int ieee80211_ocb_leave(struct ieee80211_sub_if_data *sdata)
{
	struct ieee80211_if_ocb *ifocb = &sdata->u.ocb;
	struct ieee80211_local *local = sdata->local;
	struct sta_info *sta;

	sta_info_flush(sdata);

	spin_lock_bh(&ifocb->incomplete_lock);
	while (!list_empty(&ifocb->incomplete_stations)) {
		sta = list_first_entry(&ifocb->incomplete_stations,
				       struct sta_info, list);
		list_del(&sta->list);
		spin_unlock_bh(&ifocb->incomplete_lock);

		sta_info_free(local, sta);
		spin_lock_bh(&ifocb->incomplete_lock);
	}
	spin_unlock_bh(&ifocb->incomplete_lock);

	netif_carrier_off(sdata->dev);
	clear_bit(SDATA_STATE_OFFCHANNEL, &sdata->state);
	ieee80211_bss_info_change_notify(sdata, BSS_CHANGED_OCB);

#if 0 /* TODO */
	drv_leave_ocb(local, sdata);
#endif
	mutex_lock(&sdata->local->mtx);
	ieee80211_vif_release_channel(sdata);
	mutex_unlock(&sdata->local->mtx);

	skb_queue_purge(&sdata->skb_queue);

	del_timer_sync(&sdata->u.ocb.housekeeping_timer);

	return 0;
}

