/*
 * Copyright (C) 2014 Linaro Ltd
 *
 * Author: Ulf Hansson <ulf.hansson@linaro.org>
 *
 * License terms: GNU General Public License (GPL) version 2
 */
#ifndef _LINUX_PWRSEQ_H
#define _LINUX_PWRSEQ_H

struct pwrseq {
	const struct pwrseq_ops *ops;
	struct device *dev;
	struct list_head pwrseq_node;
	struct module *owner;
	bool create_dev_from_alloc; /* dynamic create pwrseq device */
};

/* This structure is used for recording powered on pwrseq node */
struct pwrseq_node_powered_on {
	struct pwrseq *pwrseq_on;
	struct list_head list;
};

struct pwrseq_ops {
	int (*pre_power_on)(struct pwrseq *pwrseq);
	void (*post_power_on)(struct pwrseq *pwrseq);
	void (*power_off)(struct pwrseq *pwrseq);
};

#ifdef CONFIG_POWER_SEQ

int pwrseq_register(struct pwrseq *pwrseq);
void pwrseq_unregister(struct pwrseq *pwrseq);

int pwrseq_pre_power_on(struct pwrseq *pwrseq);
void pwrseq_post_power_on(struct pwrseq *pwrseq);
void pwrseq_power_off(struct pwrseq *pwrseq);
struct pwrseq *pwrseq_alloc(struct device_node *np, const char *dev_name);
void pwrseq_free(struct pwrseq *pwrseq);

#else /* CONFIG_POWER_SEQ */

static inline int pwrseq_register(struct pwrseq *pwrseq)
{
	return -ENOSYS;
}
static inline void pwrseq_unregister(struct pwrseq *pwrseq) {}
static inline int pwrseq_pre_power_on(struct pwrseq *pwrseq) {}
static inline void pwrseq_post_power_on(struct pwrseq *pwrseq) {}
static inline void pwrseq_power_off(struct pwrseq *pwrseq) {}
static inline struct pwrseq *pwrseq_alloc(struct device_node *np,
		const char *dev_name) {}
static inline void pwrseq_free(struct mmc_host *host) {}

#endif /* CONFIG_POWER_SEQ */

#endif /* _LINUX_PWRSEQ_H */
