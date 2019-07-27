#ifndef APPLICATIONS_DISPLAY_H_
#define APPLICATIONS_DISPLAY_H_

#include "msgs.h"

// Call to start the speed thread. Must be done before sending a message to the thread
void display_init (void);

void send_to_display (MESSAGE event);

#endif /* APPLICATIONS_DISPLAY_H_ */
