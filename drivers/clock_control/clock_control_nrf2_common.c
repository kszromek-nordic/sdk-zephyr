/*
 * Copyright (c) 2024 Nordic Semiconductor ASA
 * SPDX-License-Identifier: Apache-2.0
 */

#include "clock_control_nrf2_common.h"
#include <zephyr/kernel.h>
#include <hal/nrf_lrcconf.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(clock_control_nrf2, CONFIG_CLOCK_CONTROL_LOG_LEVEL);

#define FLAG_UPDATE_IN_PROGRESS BIT(FLAGS_COMMON_BITS - 1)
#define FLAG_UPDATE_NEEDED      BIT(FLAGS_COMMON_BITS - 2)

#define ONOFF_CNT_MAX (FLAGS_COMMON_BITS - 2)

#define CONTAINER_OF_ITEM(ptr, idx, type, array) \
	(type *)((char *)ptr - \
		 (idx * sizeof(array[0])) - \
		 offsetof(type, array[0]))

/*
 * Definition of `struct clock_config_generic`.
 * Used to access `clock_config_*` structures in a common way.
 */
NRF2_STRUCT_CLOCK_CONFIG(generic, ONOFF_CNT_MAX);

static sys_slist_t poweron_main_list;
static struct k_spinlock poweron_main_lock;

static void update_config(struct clock_config_generic *cfg)
{
	atomic_val_t prev_flags = atomic_or(&cfg->flags, FLAG_UPDATE_NEEDED);

	/* If the update work is already scheduled (FLAG_UPDATE_NEEDED was
	 * set before the above OR operation) or is currently being executed,
	 * it is not to be submitted again. In the latter case, it will be
	 * submitted by nrf2_clock_config_update_end().
	 */
	if (prev_flags & (FLAG_UPDATE_NEEDED | FLAG_UPDATE_IN_PROGRESS)) {
		return;
	}

	k_work_submit(&cfg->work);
}

static void onoff_start_option(struct onoff_manager *mgr,
			       onoff_notify_fn notify)
{
	struct clock_onoff *onoff =
		CONTAINER_OF(mgr, struct clock_onoff, mgr);
	struct clock_config_generic *cfg =
		CONTAINER_OF_ITEM(onoff, onoff->idx,
				  struct clock_config_generic, onoff);

	onoff->notify = notify;

	(void)atomic_or(&cfg->flags, BIT(onoff->idx));
	update_config(cfg);
}

static void onoff_stop_option(struct onoff_manager *mgr,
			      onoff_notify_fn notify)
{
	struct clock_onoff *onoff =
		CONTAINER_OF(mgr, struct clock_onoff, mgr);
	struct clock_config_generic *cfg =
		CONTAINER_OF_ITEM(onoff, onoff->idx,
				  struct clock_config_generic, onoff);

	(void)atomic_and(&cfg->flags, ~BIT(onoff->idx));
	update_config(cfg);

	notify(mgr, 0);
}

static inline uint8_t get_index_of_highest_bit(uint32_t value)
{
	return value ? (uint8_t)(31 - __builtin_clz(value)) : 0;
}

int nrf2_clock_config_init(void *clk_cfg, uint8_t onoff_cnt,
			   k_work_handler_t update_work_handler)
{
	struct clock_config_generic *cfg = clk_cfg;

	__ASSERT_NO_MSG(onoff_cnt <= ONOFF_CNT_MAX);

	for (int i = 0; i < onoff_cnt; ++i) {
		static const struct onoff_transitions transitions = {
			.start = onoff_start_option,
			.stop  = onoff_stop_option
		};
		int rc;

		rc = onoff_manager_init(&cfg->onoff[i].mgr, &transitions);
		if (rc < 0) {
			return rc;
		}

		cfg->onoff[i].idx = (uint8_t)i;
	}

	cfg->onoff_cnt = onoff_cnt;

	k_work_init(&cfg->work, update_work_handler);

	return 0;
}

uint8_t nrf2_clock_config_update_begin(struct k_work *work)
{
	struct clock_config_generic *cfg =
		CONTAINER_OF(work, struct clock_config_generic, work);
	uint32_t active_options;

	(void)atomic_or(&cfg->flags, FLAG_UPDATE_IN_PROGRESS);
	cfg->flags_snapshot = atomic_and(&cfg->flags, ~FLAG_UPDATE_NEEDED);

	active_options = cfg->flags_snapshot & BIT_MASK(ONOFF_CNT_MAX);
	return get_index_of_highest_bit(active_options);
}

void nrf2_clock_config_update_end(void *clk_cfg, int status)
{
	struct clock_config_generic *cfg = clk_cfg;
	atomic_val_t prev_flags;

	prev_flags = atomic_and(&cfg->flags, ~FLAG_UPDATE_IN_PROGRESS);
	if (!(prev_flags & FLAG_UPDATE_IN_PROGRESS)) {
		return;
	}

	for (int i = 0; i < cfg->onoff_cnt; ++i) {
		if (cfg->flags_snapshot & BIT(i)) {
			onoff_notify_fn notify = cfg->onoff[i].notify;

			if (notify) {
				/* If an option was to be activated now
				 * (it is waiting for a notification) and
				 * the activation failed, this option's flag
				 * must be cleared (the option can no longer
				 * be considered active).
				 */
				if (status < 0) {
					(void)atomic_and(&cfg->flags, ~BIT(i));
				}

				cfg->onoff[i].notify = NULL;
				notify(&cfg->onoff[i].mgr, status);
			}
		}
	}

	if (prev_flags & FLAG_UPDATE_NEEDED) {
		k_work_submit(&cfg->work);
	}
}

int api_nosys_on_off(const struct device *dev, clock_control_subsys_t sys)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(sys);

	return -ENOSYS;
}

void nrf2_clock_request_lrcconf_poweron_main(struct nrf2_clock_lrcconf_sink *sink)
{
	K_SPINLOCK(&poweron_main_lock) {
		if (sys_slist_len(&poweron_main_list) == 0) {
			LOG_DBG("%s forced on", "main domain");
			NRF_LRCCONF010->POWERON &= ~LRCCONF_POWERON_MAIN_Msk;
			NRF_LRCCONF010->POWERON |= LRCCONF_POWERON_MAIN_AlwaysOn;
		}

		sys_slist_find_and_remove(&poweron_main_list, &sink->node);
		sys_slist_append(&poweron_main_list, &sink->node);
	}
}

void nrf2_clock_release_lrcconf_poweron_main(struct nrf2_clock_lrcconf_sink *sink)
{
	K_SPINLOCK(&poweron_main_lock) {
		if (!sys_slist_find_and_remove(&poweron_main_list, &sink->node)) {
			K_SPINLOCK_BREAK;
		}

		if (sys_slist_len(&poweron_main_list) > 0) {
			K_SPINLOCK_BREAK;
		}

		LOG_DBG("%s automatic", "main domain");
		NRF_LRCCONF010->POWERON &= ~LRCCONF_POWERON_MAIN_Msk;
		NRF_LRCCONF010->POWERON |= LRCCONF_POWERON_MAIN_Automatic;
	}
}
