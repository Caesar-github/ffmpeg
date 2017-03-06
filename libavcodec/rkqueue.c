#include "rkqueue.h"

void ffrk_init_queue(FrameQueue *queue)
{
    queue->head = NULL;
    queue->tail = NULL;
    queue->size = 0;
}

int ffrk_queue_frame(AVCodecContext *avctx, FrameQueue *queue, MppFrame frame)
{
    int ret = 0;

    // create our packet entry for the queue
    FrameEntry *frameentry = malloc(sizeof(*frameentry));
    if (!frameentry) {
        ret = AVERROR(ENOMEM);
        goto done;
    }

    frameentry->frame = frame;
    frameentry->next = NULL;
    frameentry->prev = NULL;

    // add the packet to start of the queue
    if (queue->head) {
        frameentry->next = queue->head;
        queue->head->prev = frameentry;
    }
    
    if (!queue->tail)
        queue->tail = frameentry;

    queue->head = frameentry;
    queue->size++;

    return 0;

done:
    return ret;
}

MppFrame ffrk_dequeue_frame(AVCodecContext *avctx, FrameQueue *queue)
{
    FrameEntry *frameentry;
    MppFrame *frame;

    if (!queue->tail)
        return NULL;

    frameentry = queue->tail;
    queue->tail = frameentry->prev;
    
    if (!queue->tail)
        queue->head = NULL;

    if (queue->tail)
        queue->tail->next = NULL;

    queue->size--;

    frame = frameentry->frame;
    free(frameentry);

    return frame;
}

void ffrk_empty_queue(AVCodecContext *avctx, FrameQueue *queue)
{
    MppFrame frame;

    while(frame = ffrk_dequeue_frame(avctx, queue)) {
	mpp_frame_deinit(&frame);
    }
}
