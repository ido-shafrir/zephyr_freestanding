#ifndef SWITCH_THREAD_H
#define SWITCH_THREAD_H

#define SWITCH0_NODE DT_ALIAS(sw0)

#define BTN_STACK_SIZE 1024
#define BTN_PRIORITY   2       /* Higher priority than blink */

extern struct k_thread btn_thread_data;
extern k_thread_stack_t btn_stack[];

/* entry point for the button thread, this is where the button handling logic will be implemented */
void btn_thread_entry(void *p1, void *p2, void *p3);

#endif /* SWITCH_THREAD_H */