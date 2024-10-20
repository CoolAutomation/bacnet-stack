#ifndef TIMING_DEBUG_H
#define TIMING_DEBUG_H

int init_gpio();
void set_gpio(int value);
void fprintf_curtime(FILE *stream, const char *format, ...);

#endif
