#ifndef APPLICATIONS_DISPLAY_H_
#define APPLICATIONS_DISPLAY_H_

#include "msgs.h"

// Call to start the speed thread. Must be done before sending a message to the thread
void display_init (void);

// repeated calls show a dot on the bottom of the display and increment position to the right on every call
void display_dots(uint16_t pos);

void display_start(void); // start the LED hardware - call before any other display activity

void send_to_display (MESSAGE event);

#endif /* APPLICATIONS_DISPLAY_H_ */
