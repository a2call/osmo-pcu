/*
 * MsTest.cpp
 *
 * Copyright (C) 2015 by Sysmocom s.f.m.c. GmbH
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "tbf.h"
#include "gprs_debug.h"
#include "gprs_ms.h"
#include "gprs_ms_storage.h"
#include "bts.h"

extern "C" {
#include "pcu_vty.h"

#include <osmocom/core/application.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/utils.h>
#include <osmocom/vty/vty.h>
}

#include <errno.h>
#include <unistd.h>

void *tall_pcu_ctx;
int16_t spoof_mnc = 0, spoof_mcc = 0;

static void test_ms_state()
{
	uint32_t tlli = 0xffeeddbb;
	gprs_rlcmac_dl_tbf *dl_tbf;
	gprs_rlcmac_ul_tbf *ul_tbf;
	GprsMs *ms;

	printf("=== start %s ===\n", __func__);

	ms = new GprsMs(NULL, tlli);
	OSMO_ASSERT(ms->is_idle());

	dl_tbf = talloc_zero(tall_pcu_ctx, struct gprs_rlcmac_dl_tbf);
	new (dl_tbf) gprs_rlcmac_dl_tbf(NULL);
	ul_tbf = talloc_zero(tall_pcu_ctx, struct gprs_rlcmac_ul_tbf);
	new (ul_tbf) gprs_rlcmac_ul_tbf(NULL);

	ms->attach_tbf(ul_tbf);
	OSMO_ASSERT(!ms->is_idle());
	OSMO_ASSERT(ms->ul_tbf() == ul_tbf);
	OSMO_ASSERT(ms->dl_tbf() == NULL);

	ms->attach_tbf(dl_tbf);
	OSMO_ASSERT(!ms->is_idle());
	OSMO_ASSERT(ms->ul_tbf() == ul_tbf);
	OSMO_ASSERT(ms->dl_tbf() == dl_tbf);

	OSMO_ASSERT(ms->tbf(GPRS_RLCMAC_UL_TBF) == ul_tbf);
	OSMO_ASSERT(ms->tbf(GPRS_RLCMAC_DL_TBF) == dl_tbf);

	ms->detach_tbf(ul_tbf);
	OSMO_ASSERT(!ms->is_idle());
	OSMO_ASSERT(ms->ul_tbf() == NULL);
	OSMO_ASSERT(ms->dl_tbf() == dl_tbf);

	ms->detach_tbf(dl_tbf);
	/* The ms object is freed now */
	ms = NULL;

	talloc_free(dl_tbf);
	talloc_free(ul_tbf);

	printf("=== end %s ===\n", __func__);
}

static void test_ms_callback()
{
	uint32_t tlli = 0xffeeddbb;
	gprs_rlcmac_dl_tbf *dl_tbf;
	gprs_rlcmac_ul_tbf *ul_tbf;
	GprsMs *ms;
	static enum {UNKNOWN, IS_IDLE, IS_ACTIVE} last_cb = UNKNOWN;

	struct MyCallback: public GprsMs::Callback {
		virtual void ms_idle(class GprsMs *ms) {
			OSMO_ASSERT(ms->is_idle());
			printf("  ms_idle() was called\n");
			last_cb = IS_IDLE;
		}
		virtual void ms_active(class GprsMs *ms) {
			OSMO_ASSERT(!ms->is_idle());
			printf("  ms_active() was called\n");
			last_cb = IS_ACTIVE;
		}
	} cb;

	printf("=== start %s ===\n", __func__);

	ms = new GprsMs(NULL, tlli);
	ms->set_callback(&cb);

	OSMO_ASSERT(ms->is_idle());

	dl_tbf = talloc_zero(tall_pcu_ctx, struct gprs_rlcmac_dl_tbf);
	new (dl_tbf) gprs_rlcmac_dl_tbf(NULL);
	ul_tbf = talloc_zero(tall_pcu_ctx, struct gprs_rlcmac_ul_tbf);
	new (ul_tbf) gprs_rlcmac_ul_tbf(NULL);

	OSMO_ASSERT(last_cb == UNKNOWN);

	ms->attach_tbf(ul_tbf);
	OSMO_ASSERT(!ms->is_idle());
	OSMO_ASSERT(ms->ul_tbf() == ul_tbf);
	OSMO_ASSERT(ms->dl_tbf() == NULL);
	OSMO_ASSERT(last_cb == IS_ACTIVE);

	last_cb = UNKNOWN;

	ms->attach_tbf(dl_tbf);
	OSMO_ASSERT(!ms->is_idle());
	OSMO_ASSERT(ms->ul_tbf() == ul_tbf);
	OSMO_ASSERT(ms->dl_tbf() == dl_tbf);
	OSMO_ASSERT(last_cb == UNKNOWN);

	ms->detach_tbf(ul_tbf);
	OSMO_ASSERT(!ms->is_idle());
	OSMO_ASSERT(ms->ul_tbf() == NULL);
	OSMO_ASSERT(ms->dl_tbf() == dl_tbf);
	OSMO_ASSERT(last_cb == UNKNOWN);

	ms->detach_tbf(dl_tbf);
	OSMO_ASSERT(ms->is_idle());
	OSMO_ASSERT(ms->ul_tbf() == NULL);
	OSMO_ASSERT(ms->dl_tbf() == NULL);
	OSMO_ASSERT(last_cb == IS_IDLE);

	last_cb = UNKNOWN;
	delete ms;

	talloc_free(dl_tbf);
	talloc_free(ul_tbf);

	printf("=== end %s ===\n", __func__);
}

static void test_ms_replace_tbf()
{
	uint32_t tlli = 0xffeeddbb;
	gprs_rlcmac_dl_tbf *dl_tbf[2];
	gprs_rlcmac_ul_tbf *ul_tbf;
	GprsMs *ms;
	static bool was_idle;

	struct MyCallback: public GprsMs::Callback {
		virtual void ms_idle(class GprsMs *ms) {
			OSMO_ASSERT(ms->is_idle());
			printf("  ms_idle() was called\n");
			was_idle = true;
		}
		virtual void ms_active(class GprsMs *ms) {
			OSMO_ASSERT(!ms->is_idle());
			printf("  ms_active() was called\n");
		}
	} cb;

	printf("=== start %s ===\n", __func__);

	ms = new GprsMs(NULL, tlli);
	ms->set_callback(&cb);

	OSMO_ASSERT(ms->is_idle());
	was_idle = false;

	dl_tbf[0] = talloc_zero(tall_pcu_ctx, struct gprs_rlcmac_dl_tbf);
	new (dl_tbf[0]) gprs_rlcmac_dl_tbf(NULL);
	dl_tbf[1] = talloc_zero(tall_pcu_ctx, struct gprs_rlcmac_dl_tbf);
	new (dl_tbf[1]) gprs_rlcmac_dl_tbf(NULL);
	ul_tbf = talloc_zero(tall_pcu_ctx, struct gprs_rlcmac_ul_tbf);
	new (ul_tbf) gprs_rlcmac_ul_tbf(NULL);

	ms->attach_tbf(dl_tbf[0]);
	OSMO_ASSERT(!ms->is_idle());
	OSMO_ASSERT(ms->ul_tbf() == NULL);
	OSMO_ASSERT(ms->dl_tbf() == dl_tbf[0]);
	OSMO_ASSERT(llist_empty(&ms->old_tbfs()));
	OSMO_ASSERT(!was_idle);

	ms->attach_tbf(dl_tbf[1]);
	OSMO_ASSERT(!ms->is_idle());
	OSMO_ASSERT(ms->ul_tbf() == NULL);
	OSMO_ASSERT(ms->dl_tbf() == dl_tbf[1]);
	OSMO_ASSERT(!llist_empty(&ms->old_tbfs()));
	OSMO_ASSERT(!was_idle);

	ms->attach_tbf(ul_tbf);
	OSMO_ASSERT(!ms->is_idle());
	OSMO_ASSERT(ms->ul_tbf() == ul_tbf);
	OSMO_ASSERT(ms->dl_tbf() == dl_tbf[1]);
	OSMO_ASSERT(!llist_empty(&ms->old_tbfs()));
	OSMO_ASSERT(!was_idle);

	ms->detach_tbf(ul_tbf);
	OSMO_ASSERT(!ms->is_idle());
	OSMO_ASSERT(ms->ul_tbf() == NULL);
	OSMO_ASSERT(ms->dl_tbf() == dl_tbf[1]);
	OSMO_ASSERT(!llist_empty(&ms->old_tbfs()));
	OSMO_ASSERT(!was_idle);

	ms->detach_tbf(dl_tbf[0]);
	OSMO_ASSERT(!ms->is_idle());
	OSMO_ASSERT(ms->ul_tbf() == NULL);
	OSMO_ASSERT(ms->dl_tbf() == dl_tbf[1]);
	OSMO_ASSERT(llist_empty(&ms->old_tbfs()));
	OSMO_ASSERT(!was_idle);

	ms->detach_tbf(dl_tbf[1]);
	OSMO_ASSERT(ms->is_idle());
	OSMO_ASSERT(ms->ul_tbf() == NULL);
	OSMO_ASSERT(ms->dl_tbf() == NULL);
	OSMO_ASSERT(llist_empty(&ms->old_tbfs()));
	OSMO_ASSERT(was_idle);

	delete ms;

	talloc_free(dl_tbf[0]);
	talloc_free(dl_tbf[1]);
	talloc_free(ul_tbf);

	printf("=== end %s ===\n", __func__);
}

static void test_ms_change_tlli()
{
	uint32_t start_tlli = 0xaa000000;
	uint32_t new_ms_tlli = 0xff001111;
	uint32_t other_sgsn_tlli = 0xff00eeee;
	GprsMs *ms;

	printf("=== start %s ===\n", __func__);

	ms = new GprsMs(NULL, start_tlli);

	OSMO_ASSERT(ms->is_idle());

	/* MS announces TLLI, SGSN uses it immediately */
	ms->set_tlli(new_ms_tlli);
	OSMO_ASSERT(ms->tlli() == new_ms_tlli);
	OSMO_ASSERT(ms->check_tlli(new_ms_tlli));
	OSMO_ASSERT(ms->check_tlli(start_tlli));

	ms->confirm_tlli(new_ms_tlli);
	OSMO_ASSERT(ms->tlli() == new_ms_tlli);
	OSMO_ASSERT(ms->check_tlli(new_ms_tlli));
	OSMO_ASSERT(!ms->check_tlli(start_tlli));

	/* MS announces TLLI, SGSN uses it later */
	ms->set_tlli(start_tlli);
	ms->confirm_tlli(start_tlli);

	ms->set_tlli(new_ms_tlli);
	OSMO_ASSERT(ms->tlli() == new_ms_tlli);
	OSMO_ASSERT(ms->check_tlli(new_ms_tlli));
	OSMO_ASSERT(ms->check_tlli(start_tlli));

	ms->confirm_tlli(start_tlli);
	OSMO_ASSERT(ms->tlli() == new_ms_tlli);
	OSMO_ASSERT(ms->check_tlli(new_ms_tlli));
	OSMO_ASSERT(ms->check_tlli(start_tlli));

	ms->set_tlli(new_ms_tlli);
	OSMO_ASSERT(ms->tlli() == new_ms_tlli);
	OSMO_ASSERT(ms->check_tlli(new_ms_tlli));
	OSMO_ASSERT(ms->check_tlli(start_tlli));

	ms->confirm_tlli(new_ms_tlli);
	OSMO_ASSERT(ms->tlli() == new_ms_tlli);
	OSMO_ASSERT(ms->check_tlli(new_ms_tlli));
	OSMO_ASSERT(!ms->check_tlli(start_tlli));

	/* MS announces TLLI, SGSN uses it later after another new TLLI */
	ms->set_tlli(start_tlli);
	ms->confirm_tlli(start_tlli);

	ms->set_tlli(new_ms_tlli);
	OSMO_ASSERT(ms->tlli() == new_ms_tlli);
	OSMO_ASSERT(ms->check_tlli(new_ms_tlli));
	OSMO_ASSERT(ms->check_tlli(start_tlli));

	ms->confirm_tlli(other_sgsn_tlli);
	OSMO_ASSERT(ms->tlli() == new_ms_tlli);
	OSMO_ASSERT(ms->check_tlli(new_ms_tlli));
	OSMO_ASSERT(ms->check_tlli(other_sgsn_tlli));

	ms->set_tlli(new_ms_tlli);
	OSMO_ASSERT(ms->tlli() == new_ms_tlli);
	OSMO_ASSERT(ms->check_tlli(new_ms_tlli));
	OSMO_ASSERT(ms->check_tlli(other_sgsn_tlli));

	ms->confirm_tlli(new_ms_tlli);
	OSMO_ASSERT(ms->tlli() == new_ms_tlli);
	OSMO_ASSERT(ms->check_tlli(new_ms_tlli));
	OSMO_ASSERT(!ms->check_tlli(start_tlli));
	OSMO_ASSERT(!ms->check_tlli(other_sgsn_tlli));

	/* SGSN uses the new TLLI before it is announced by the MS (shouldn't
	 * happen in normal use) */
	ms->set_tlli(start_tlli);
	ms->confirm_tlli(start_tlli);

	ms->confirm_tlli(new_ms_tlli);
	OSMO_ASSERT(ms->tlli() == start_tlli);
	OSMO_ASSERT(ms->check_tlli(new_ms_tlli));
	OSMO_ASSERT(ms->check_tlli(start_tlli));

	ms->set_tlli(new_ms_tlli);
	OSMO_ASSERT(ms->tlli() == new_ms_tlli);
	OSMO_ASSERT(ms->check_tlli(new_ms_tlli));
	OSMO_ASSERT(!ms->check_tlli(start_tlli));

	delete ms;

	printf("=== end %s ===\n", __func__);
}

static void test_ms_storage()
{
	uint32_t tlli = 0xffeeddbb;
	const char *imsi1 = "001001987654321";
	const char *imsi2 = "001001987654322";

	gprs_rlcmac_ul_tbf *ul_tbf;
	GprsMs *ms, *ms_tmp;
	GprsMsStorage store(NULL);

	printf("=== start %s ===\n", __func__);

	ul_tbf = talloc_zero(tall_pcu_ctx, struct gprs_rlcmac_ul_tbf);
	new (ul_tbf) gprs_rlcmac_ul_tbf(NULL);

	ms = store.get_ms(tlli + 0);
	OSMO_ASSERT(ms == NULL);

	ms = store.create_ms(tlli + 0, GPRS_RLCMAC_UL_TBF);
	OSMO_ASSERT(ms != NULL);
	OSMO_ASSERT(ms->tlli() == tlli + 0);
	ms->set_imsi(imsi1);
	OSMO_ASSERT(strcmp(ms->imsi(), imsi1) == 0);

	ms_tmp = store.get_ms(tlli + 0);
	OSMO_ASSERT(ms == ms_tmp);
	OSMO_ASSERT(ms->tlli() == tlli + 0);

	ms_tmp = store.get_ms(0, 0, imsi1);
	OSMO_ASSERT(ms == ms_tmp);
	OSMO_ASSERT(strcmp(ms->imsi(), imsi1) == 0);
	ms_tmp = store.get_ms(0, 0, imsi2);
	OSMO_ASSERT(ms_tmp == NULL);

	ms = store.create_ms(tlli + 1, GPRS_RLCMAC_UL_TBF);
	OSMO_ASSERT(ms != NULL);
	OSMO_ASSERT(ms->tlli() == tlli + 1);
	ms->set_imsi(imsi2);
	OSMO_ASSERT(strcmp(ms->imsi(), imsi2) == 0);

	ms_tmp = store.get_ms(tlli + 1);
	OSMO_ASSERT(ms == ms_tmp);
	OSMO_ASSERT(ms->tlli() == tlli + 1);

	ms_tmp = store.get_ms(0, 0, imsi1);
	OSMO_ASSERT(ms_tmp != NULL);
	OSMO_ASSERT(ms_tmp != ms);
	ms_tmp = store.get_ms(0, 0, imsi2);
	OSMO_ASSERT(ms == ms_tmp);
	OSMO_ASSERT(strcmp(ms->imsi(), imsi2) == 0);

	/* delete ms */
	ms = store.get_ms(tlli + 0);
	OSMO_ASSERT(ms != NULL);
	ms->attach_tbf(ul_tbf);
	ms->detach_tbf(ul_tbf);
	ms = store.get_ms(tlli + 0);
	OSMO_ASSERT(ms == NULL);
	ms = store.get_ms(tlli + 1);
	OSMO_ASSERT(ms != NULL);

	/* delete ms */
	ms = store.get_ms(tlli + 1);
	OSMO_ASSERT(ms != NULL);
	ms->attach_tbf(ul_tbf);
	ms->detach_tbf(ul_tbf);
	ms = store.get_ms(tlli + 1);
	OSMO_ASSERT(ms == NULL);

	talloc_free(ul_tbf);

	printf("=== end %s ===\n", __func__);
}

static void test_ms_timeout()
{
	uint32_t tlli = 0xffeeddbb;
	gprs_rlcmac_dl_tbf *dl_tbf;
	gprs_rlcmac_ul_tbf *ul_tbf;
	GprsMs *ms;
	static enum {UNKNOWN, IS_IDLE, IS_ACTIVE} last_cb = UNKNOWN;

	struct MyCallback: public GprsMs::Callback {
		virtual void ms_idle(class GprsMs *ms) {
			OSMO_ASSERT(ms->is_idle());
			printf("  ms_idle() was called\n");
			last_cb = IS_IDLE;
		}
		virtual void ms_active(class GprsMs *ms) {
			OSMO_ASSERT(!ms->is_idle());
			printf("  ms_active() was called\n");
			last_cb = IS_ACTIVE;
		}
	} cb;

	printf("=== start %s ===\n", __func__);

	ms = new GprsMs(NULL, tlli);
	ms->set_callback(&cb);
	ms->set_timeout(1);

	OSMO_ASSERT(ms->is_idle());

	dl_tbf = talloc_zero(tall_pcu_ctx, struct gprs_rlcmac_dl_tbf);
	new (dl_tbf) gprs_rlcmac_dl_tbf(NULL);
	ul_tbf = talloc_zero(tall_pcu_ctx, struct gprs_rlcmac_ul_tbf);
	new (ul_tbf) gprs_rlcmac_ul_tbf(NULL);

	OSMO_ASSERT(last_cb == UNKNOWN);

	ms->attach_tbf(ul_tbf);
	OSMO_ASSERT(!ms->is_idle());
	OSMO_ASSERT(last_cb == IS_ACTIVE);

	last_cb = UNKNOWN;

	ms->attach_tbf(dl_tbf);
	OSMO_ASSERT(!ms->is_idle());
	OSMO_ASSERT(last_cb == UNKNOWN);

	ms->detach_tbf(ul_tbf);
	OSMO_ASSERT(!ms->is_idle());
	OSMO_ASSERT(last_cb == UNKNOWN);

	ms->detach_tbf(dl_tbf);
	OSMO_ASSERT(!ms->is_idle());
	OSMO_ASSERT(last_cb == UNKNOWN);

	usleep(1100000);
	osmo_timers_update();

	OSMO_ASSERT(ms->is_idle());
	OSMO_ASSERT(last_cb == IS_IDLE);

	last_cb = UNKNOWN;
	delete ms;
	talloc_free(dl_tbf);
	talloc_free(ul_tbf);

	printf("=== end %s ===\n", __func__);
}

static void test_ms_cs_selection()
{
	BTS the_bts;
	gprs_rlcmac_bts *bts = the_bts.bts_data();
	uint32_t tlli = 0xffeeddbb;

	gprs_rlcmac_dl_tbf *dl_tbf;
	GprsMs *ms;

	printf("=== start %s ===\n", __func__);

	bts->initial_cs_dl = 4;
	bts->initial_cs_ul = 1;
	bts->cs_downgrade_threshold = 0;

	ms = new GprsMs(&the_bts, tlli);

	OSMO_ASSERT(ms->is_idle());

	dl_tbf = talloc_zero(tall_pcu_ctx, struct gprs_rlcmac_dl_tbf);
	new (dl_tbf) gprs_rlcmac_dl_tbf(NULL);

	dl_tbf->set_ms(ms);
	OSMO_ASSERT(!ms->is_idle());

	OSMO_ASSERT(ms->current_cs_dl().to_num() == 4);

	bts->cs_downgrade_threshold = 200;

	OSMO_ASSERT(ms->current_cs_dl().to_num() == 3);

	talloc_free(dl_tbf);

	printf("=== end %s ===\n", __func__);
}

static const struct log_info_cat default_categories[] = {
	{"DPCU", "", "GPRS Packet Control Unit (PCU)", LOGL_INFO, 1},
};

static int filter_fn(const struct log_context *ctx,
	struct log_target *tar)
{
	return 1;
}

const struct log_info debug_log_info = {
	filter_fn,
	(struct log_info_cat*)default_categories,
	ARRAY_SIZE(default_categories),
};

int main(int argc, char **argv)
{
	struct vty_app_info pcu_vty_info = {0};

	tall_pcu_ctx = talloc_named_const(NULL, 1, "MsTest context");
	if (!tall_pcu_ctx)
		abort();

	msgb_set_talloc_ctx(tall_pcu_ctx);
	osmo_init_logging(&debug_log_info);
	log_set_use_color(osmo_stderr_target, 0);
	log_set_print_filename(osmo_stderr_target, 0);
	log_set_log_level(osmo_stderr_target, LOGL_INFO);

	vty_init(&pcu_vty_info);
	pcu_vty_init(&debug_log_info);

	test_ms_state();
	test_ms_callback();
	test_ms_replace_tbf();
	test_ms_change_tlli();
	test_ms_storage();
	test_ms_timeout();
	test_ms_cs_selection();

	if (getenv("TALLOC_REPORT_FULL"))
		talloc_report_full(tall_pcu_ctx, stderr);

	return EXIT_SUCCESS;
}

extern "C" {
void l1if_pdch_req() { abort(); }
void l1if_connect_pdch() { abort(); }
void l1if_close_pdch() { abort(); }
void l1if_open_pdch() { abort(); }
}
