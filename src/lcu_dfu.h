#ifndef LCU_DFU_H
#define LCU_DFU_H

#include "can_dfu.h" /* generic peripheral — provides can_dfu_on_frame / can_dfu_is_active */

/* CAN IDs used by the LCU DFU protocol (extended frames) */
#define LCU_DFU_CMD_ID  0x7E4U  /* updater -> device: REQUEST or COMMIT */
#define LCU_DFU_DATA_ID 0x7E5U  /* updater -> device: firmware chunks   */
#define LCU_DFU_RSP_ID  0x7E6U  /* device  -> updater: status responses */

void lcu_dfu_init(void);

#endif /* LCU_DFU_H */
