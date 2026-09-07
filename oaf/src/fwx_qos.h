// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef __FWX_QOS_H__
#define __FWX_QOS_H__

#include <linux/types.h>

#define FWX_QOS_MAX_RULES 2048
#define FWX_QOS_MAX_WRITE 65535
#define FWX_QOS_CLASS_MAX 4094
#define FWX_QOS_CLASS_SHIFT 16
#define FWX_QOS_CLASS_MASK 0x0FFF0000U
#define FWX_QOS_SKB_MARK_MASK FWX_QOS_CLASS_MASK

struct fwx_qos_rule {
	u32 app_id;
	u16 class_id;
	u8 any_mac;
	u8 mac[6];
};

int fwx_qos_init(void);
void fwx_qos_exit(void);

#endif
