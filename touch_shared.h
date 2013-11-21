#ifndef TOUCH_SHARED_H
#define TOUCH_SHARED_H

#ifdef __cplusplus
extern "C" {
#endif

#define TOUCH_PID 0x524
#define TOUCH_VID 0x596

#define TOUCH_REPORT 0

struct TouchEvent {
    int idx;
    int x;
    int y;
};

extern void submitTouch(struct TouchEvent ev);
extern void startTouchLoop(void);

#ifdef __cplusplus
}
#endif

#endif // TOUCH_SHARED_H
