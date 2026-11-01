#ifndef BREAKPOINT_PET_H
#define BREAKPOINT_PET_H

#include "qm.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the breakpoint-pet state machine and its worker. */
qm_err_t breakpoint_pet_init(void);

/* Queue a final ASR result for the state machine. */
qm_err_t breakpoint_pet_receive_voice(const char *text);

/* Camera adapter hooks. The live camera task can call these later. */
qm_err_t breakpoint_pet_pre_leave_detected(void);
qm_err_t breakpoint_pet_return_detected(void);

#ifdef __cplusplus
}
#endif

#endif
