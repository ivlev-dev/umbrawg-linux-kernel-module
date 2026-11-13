/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _UWG_FRAME_H
#define _UWG_FRAME_H

#include <linux/types.h>

#define UWG_FRAME_MAGIC		0x00
#define UWG_FRAME_HDR_LEN	4

enum uwg_frame_kind {
	UWG_FRAME_FEC_REPAIR	= 0x01,
	UWG_FRAME_CC_FEEDBACK	= 0x02,
	UWG_FRAME_CC_REQUEST	= 0x03,
};

#define UWG_CC_REQUEST_LEN	12
#define UWG_CC_FEEDBACK_LEN	40

#endif /* _UWG_FRAME_H */
