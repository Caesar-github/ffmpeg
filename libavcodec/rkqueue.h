#ifndef _RKQUEUE_H_
#define _RKQUEUE_H_

#include <rockchip/mpp_frame.h>

#include "avcodec.h"

// types declaration
typedef struct FrameEntry {
  MppFrame frame;
  struct FrameEntry *next;
  struct FrameEntry *prev;
} FrameEntry;

typedef struct FrameQueue {
  FrameEntry *head;
  FrameEntry *tail;
  int size;
} FrameQueue;

// fucntions prototypes
void ffrk_init_queue(FrameQueue *queue);
int ffrk_queue_frame(AVCodecContext *avctx, FrameQueue *queue, MppFrame frame);
MppFrame ffrk_dequeue_frame(AVCodecContext *avctx, FrameQueue *queue);
void ffrk_empty_queue(AVCodecContext *avctx, FrameQueue *queue);

#endif /* _RKQUEUE_H_ */
